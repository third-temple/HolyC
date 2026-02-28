#include "hc_runtime.h"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <pthread.h>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#include <unistd.h>
#endif

extern "C" {

struct HcMemberMeta {
  char* key;
  std::int64_t value;
  HcMemberMeta* next;
};

struct CMemberLst {
  const char* str;
  std::int64_t offset;
  CMemberLst* next;
  HcMemberMeta* meta;
};

struct CHashClass {
  CMemberLst* member_lst_and_root;
  const char* class_name;
  CHashClass* next;
  CMemberLst* tail;
  std::int64_t next_offset;
};

struct CJob {
  pthread_t thread;
  const char* fn;
  std::int64_t arg;
  std::int64_t result;
  int joined;
};

struct HcSpawnRequest {
  const char* fn;
  const char* data;
};

namespace {

thread_local hc_try_frame* g_try_stack = nullptr;
thread_local std::int64_t g_exception_payload = 0;
thread_local const hc_reflection_field* g_reflection_fields = nullptr;
thread_local std::size_t g_reflection_field_count = 0;
thread_local CHashClass* g_hash_classes = nullptr;
thread_local bool g_reflection_cache_ready = false;
std::atomic<std::int64_t> g_next_task_id{1};
pthread_mutex_t g_spawn_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_spawn_cond = PTHREAD_COND_INITIALIZER;
std::int64_t g_spawn_inflight = 0;

void MarkSpawnStart() {
  pthread_mutex_lock(&g_spawn_mutex);
  ++g_spawn_inflight;
  pthread_mutex_unlock(&g_spawn_mutex);
}

void MarkSpawnDone() {
  pthread_mutex_lock(&g_spawn_mutex);
  if (g_spawn_inflight > 0) {
    --g_spawn_inflight;
  }
  if (g_spawn_inflight == 0) {
    pthread_cond_broadcast(&g_spawn_cond);
  }
  pthread_mutex_unlock(&g_spawn_mutex);
}

const char* LookupZString(const char* table, std::int64_t index) {
  if (table == nullptr || index < 0) {
    return "";
  }
  const char* cur = table;
  for (std::int64_t i = 0; i < index; ++i) {
    while (*cur != '\0') {
      ++cur;
    }
    ++cur;
  }
  return cur;
}

void PrintBinary(std::uint64_t value) {
  bool started = false;
  for (int bit = 63; bit >= 0; --bit) {
    const bool one = ((value >> bit) & 1ULL) != 0;
    if (one) {
      started = true;
    }
    if (started) {
      std::fputc(one ? '1' : '0', stdout);
    }
  }
  if (!started) {
    std::fputc('0', stdout);
  }
}

char* CopyCString(const char* text) {
  if (text == nullptr) {
    return nullptr;
  }
  const std::size_t len = std::strlen(text);
  char* out = static_cast<char*>(std::malloc(len + 1));
  if (out == nullptr) {
    return nullptr;
  }
  std::memcpy(out, text, len + 1);
  return out;
}

void FreeMetaList(HcMemberMeta* meta) {
  while (meta != nullptr) {
    HcMemberMeta* next = meta->next;
    std::free(meta->key);
    std::free(meta);
    meta = next;
  }
}

void ClearReflectionCache() {
  CHashClass* klass = g_hash_classes;
  while (klass != nullptr) {
    CHashClass* next_class = klass->next;
    CMemberLst* member = klass->member_lst_and_root;
    while (member != nullptr) {
      CMemberLst* next_member = member->next;
      std::free(const_cast<char*>(member->str));
      FreeMetaList(member->meta);
      std::free(member);
      member = next_member;
    }
    std::free(const_cast<char*>(klass->class_name));
    std::free(klass);
    klass = next_class;
  }
  g_hash_classes = nullptr;
  g_reflection_cache_ready = false;
}

std::size_t EstimateTypeSize(const char* type_name) {
  if (type_name == nullptr) {
    return 8;
  }
  if (std::strchr(type_name, '*') != nullptr) {
    return 8;
  }
  if (std::strcmp(type_name, "I8") == 0 || std::strcmp(type_name, "U8") == 0 ||
      std::strcmp(type_name, "Bool") == 0) {
    return 1;
  }
  if (std::strcmp(type_name, "I16") == 0 || std::strcmp(type_name, "U16") == 0) {
    return 2;
  }
  if (std::strcmp(type_name, "I32") == 0 || std::strcmp(type_name, "U32") == 0) {
    return 4;
  }
  return 8;
}

bool ParseIntLiteral(const char* text, std::int64_t* value_out) {
  if (text == nullptr || value_out == nullptr) {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const long long value = std::strtoll(text, &end, 0);
  if (errno != 0 || end == text || *end != '\0') {
    return false;
  }
  *value_out = static_cast<std::int64_t>(value);
  return true;
}

bool ParseSimpleIntExpr(const char* text, std::int64_t* value_out) {
  if (text == nullptr || value_out == nullptr) {
    return false;
  }

  if (ParseIntLiteral(text, value_out)) {
    return true;
  }

  const char* op = nullptr;
  for (std::size_t i = 1; text[i] != '\0'; ++i) {
    if (text[i] == '+' || text[i] == '-') {
      op = text + i;
    }
  }
  if (op == nullptr) {
    for (std::size_t i = 1; text[i] != '\0'; ++i) {
      if (text[i] == '*' || text[i] == '/' || text[i] == '%') {
        op = text + i;
      }
    }
  }
  if (op == nullptr) {
    if (std::strcmp(text, "TRUE") == 0) {
      *value_out = 1;
      return true;
    }
    if (std::strcmp(text, "FALSE") == 0 || std::strcmp(text, "NULL") == 0) {
      *value_out = 0;
      return true;
    }
    if (std::strcmp(text, "RED") == 0) {
      *value_out = 1;
      return true;
    }

    char* end = nullptr;
    errno = 0;
    const double as_double = std::strtod(text, &end);
    if (errno == 0 && end != text && *end == '\0') {
      *value_out = static_cast<std::int64_t>(as_double);
      return true;
    }
    return false;
  }

  const std::size_t left_len = static_cast<std::size_t>(op - text);
  char* left = static_cast<char*>(std::malloc(left_len + 1));
  if (left == nullptr) {
    return false;
  }
  std::memcpy(left, text, left_len);
  left[left_len] = '\0';

  const char* right = op + 1;
  std::int64_t lhs = 0;
  std::int64_t rhs = 0;
  const bool lhs_ok = ParseSimpleIntExpr(left, &lhs);
  const bool rhs_ok = ParseSimpleIntExpr(right, &rhs);
  std::free(left);
  if (!lhs_ok || !rhs_ok) {
    return false;
  }

  switch (*op) {
    case '+':
      *value_out = lhs + rhs;
      return true;
    case '-':
      *value_out = lhs - rhs;
      return true;
    case '*':
      *value_out = lhs * rhs;
      return true;
    case '/':
      if (rhs == 0) {
        return false;
      }
      *value_out = lhs / rhs;
      return true;
    case '%':
      if (rhs == 0) {
        return false;
      }
      *value_out = lhs % rhs;
      return true;
    default:
      return false;
  }
}

char* DecodeQuotedString(const char* text) {
  if (text == nullptr) {
    return CopyCString("");
  }
  const std::size_t len = std::strlen(text);
  if (len < 2 || text[0] != '"' || text[len - 1] != '"') {
    return CopyCString(text);
  }

  char* out = static_cast<char*>(std::malloc(len));
  if (out == nullptr) {
    return nullptr;
  }

  std::size_t j = 0;
  for (std::size_t i = 1; i + 1 < len; ++i) {
    char c = text[i];
    if (c == '\\' && i + 2 < len) {
      const char n = text[++i];
      switch (n) {
        case 'n':
          c = '\n';
          break;
        case 't':
          c = '\t';
          break;
        case 'r':
          c = '\r';
          break;
        case '\\':
          c = '\\';
          break;
        case '"':
          c = '"';
          break;
        default:
          c = n;
          break;
      }
    }
    out[j++] = c;
  }
  out[j] = '\0';
  return out;
}

std::int64_t ResolveSymbolAddress(const char* symbol_name) {
  if (symbol_name == nullptr || *symbol_name == '\0') {
    return 0;
  }
#if defined(__unix__) || defined(__APPLE__)
  const void* ptr = dlsym(RTLD_DEFAULT, symbol_name);
  return static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(ptr));
#else
  (void)symbol_name;
  return 0;
#endif
}

std::int64_t ParseMetaValue(const char* value_token) {
  if (value_token == nullptr) {
    return 1;
  }
  if (value_token[0] == '"') {
    const char* decoded = DecodeQuotedString(value_token);
    return static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(decoded));
  }
  if (value_token[0] == '&') {
    return ResolveSymbolAddress(value_token + 1);
  }

  std::int64_t value = 0;
  if (ParseSimpleIntExpr(value_token, &value)) {
    return value;
  }
  return 0;
}

void AddMetaEntry(CMemberLst* member, const char* key, std::int64_t value) {
  if (member == nullptr || key == nullptr) {
    return;
  }
  HcMemberMeta* meta = static_cast<HcMemberMeta*>(std::calloc(1, sizeof(HcMemberMeta)));
  if (meta == nullptr) {
    return;
  }
  meta->key = CopyCString(key);
  meta->value = value;
  meta->next = nullptr;

  if (member->meta == nullptr) {
    member->meta = meta;
    return;
  }

  HcMemberMeta* cur = member->meta;
  while (cur->next != nullptr) {
    cur = cur->next;
  }
  cur->next = meta;
}

std::size_t TokenizeAnnotations(char* text, char** tokens, std::size_t max_tokens) {
  if (text == nullptr || tokens == nullptr || max_tokens == 0) {
    return 0;
  }

  std::size_t count = 0;
  char* cur = text;
  while (*cur != '\0' && count < max_tokens) {
    while (std::isspace(static_cast<unsigned char>(*cur)) != 0) {
      ++cur;
    }
    if (*cur == '\0') {
      break;
    }

    char* start = cur;
    if (*cur == '"') {
      ++cur;
      while (*cur != '\0') {
        if (*cur == '"' && cur[-1] != '\\') {
          ++cur;
          break;
        }
        ++cur;
      }
    } else {
      while (*cur != '\0' && std::isspace(static_cast<unsigned char>(*cur)) == 0) {
        ++cur;
      }
    }

    if (*cur != '\0') {
      *cur = '\0';
      ++cur;
    }
    tokens[count++] = start;
  }
  return count;
}

void PopulateMemberMeta(CMemberLst* member, const char* annotations) {
  if (member == nullptr || annotations == nullptr || *annotations == '\0') {
    return;
  }

  char* copy = CopyCString(annotations);
  if (copy == nullptr) {
    return;
  }

  char* tokens[128];
  const std::size_t token_count = TokenizeAnnotations(copy, tokens, 128);
  for (std::size_t i = 0; i < token_count; i += 2) {
    const char* key = tokens[i];
    const char* value = (i + 1 < token_count) ? tokens[i + 1] : "1";
    AddMetaEntry(member, key, ParseMetaValue(value));
  }

  std::free(copy);
}

CHashClass* FindClassByName(const char* class_name) {
  for (CHashClass* klass = g_hash_classes; klass != nullptr; klass = klass->next) {
    if (klass->class_name != nullptr && class_name != nullptr &&
        std::strcmp(klass->class_name, class_name) == 0) {
      return klass;
    }
  }
  return nullptr;
}

CHashClass* FindOrCreateClass(const char* class_name) {
  CHashClass* existing = FindClassByName(class_name);
  if (existing != nullptr) {
    return existing;
  }

  CHashClass* klass = static_cast<CHashClass*>(std::calloc(1, sizeof(CHashClass)));
  if (klass == nullptr) {
    return nullptr;
  }
  klass->class_name = CopyCString(class_name);
  klass->member_lst_and_root = nullptr;
  klass->tail = nullptr;
  klass->next_offset = 0;
  klass->next = g_hash_classes;
  g_hash_classes = klass;
  return klass;
}

void AppendMemberField(CHashClass* klass, const hc_reflection_field& field) {
  if (klass == nullptr || field.field_name == nullptr) {
    return;
  }

  CMemberLst* member = static_cast<CMemberLst*>(std::calloc(1, sizeof(CMemberLst)));
  if (member == nullptr) {
    return;
  }

  member->str = CopyCString(field.field_name);
  member->offset = klass->next_offset;
  member->next = nullptr;
  member->meta = nullptr;
  klass->next_offset += static_cast<std::int64_t>(EstimateTypeSize(field.field_type));

  if (klass->member_lst_and_root == nullptr) {
    klass->member_lst_and_root = member;
    klass->tail = member;
  } else {
    klass->tail->next = member;
    klass->tail = member;
  }

  PopulateMemberMeta(member, field.annotations);
}

void EnsureReflectionCache() {
  if (g_reflection_cache_ready) {
    return;
  }
  ClearReflectionCache();

  if (g_reflection_fields == nullptr || g_reflection_field_count == 0) {
    g_reflection_cache_ready = true;
    return;
  }

  for (std::size_t i = 0; i < g_reflection_field_count; ++i) {
    const hc_reflection_field& field = g_reflection_fields[i];
    if (field.aggregate_name == nullptr || field.field_name == nullptr) {
      continue;
    }
    CHashClass* klass = FindOrCreateClass(field.aggregate_name);
    if (klass == nullptr) {
      continue;
    }
    AppendMemberField(klass, field);
  }

  g_reflection_cache_ready = true;
}

void* JobThreadMain(void* opaque) {
  CJob* job = static_cast<CJob*>(opaque);
  if (job == nullptr || job->fn == nullptr) {
    return nullptr;
  }

  using JobFn = void (*)(std::int64_t);
  JobFn fn = reinterpret_cast<JobFn>(reinterpret_cast<std::uintptr_t>(job->fn));
  fn(job->arg);
  job->result = 0;
  return nullptr;
}

void* SpawnThreadMain(void* opaque) {
  HcSpawnRequest* req = static_cast<HcSpawnRequest*>(opaque);
  if (req == nullptr) {
    MarkSpawnDone();
    return nullptr;
  }
  if (req->fn != nullptr) {
    using SpawnFn = void (*)(const char*);
    SpawnFn fn = reinterpret_cast<SpawnFn>(reinterpret_cast<std::uintptr_t>(req->fn));
    fn(req->data);
  }
  std::free(req);
  MarkSpawnDone();
  return nullptr;
}

void TaskSpawnCommandEntry(const char* command_data) {
  char* command = const_cast<char*>(command_data);
  if (command != nullptr && command[0] != '\0') {
    const int system_rc = std::system(command);
    if (system_rc == -1) {
      std::fprintf(stderr, "warning: Spawn command launch failed: %s\n", command);
    }
  }
  std::free(command);
}

std::size_t NormalizeStackSize(std::int64_t requested_size) {
  if (requested_size <= 0) {
    return 0;
  }
  std::size_t stack_size = static_cast<std::size_t>(requested_size);
  if (stack_size < static_cast<std::size_t>(PTHREAD_STACK_MIN)) {
    stack_size = static_cast<std::size_t>(PTHREAD_STACK_MIN);
  }
#if defined(_SC_PAGESIZE)
  const long page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size > 0) {
    const std::size_t page = static_cast<std::size_t>(page_size);
    const std::size_t rem = stack_size % page;
    if (rem != 0) {
      stack_size += page - rem;
    }
  }
#endif
  return stack_size;
}

}  // namespace

std::int64_t hc_runtime_abi_version() {
  return (static_cast<std::int64_t>(HC_RUNTIME_ABI_VERSION_MAJOR) << 32) |
         static_cast<std::int64_t>(HC_RUNTIME_ABI_VERSION_MINOR);
}

void hc_print_str(const char* text) {
  if (text == nullptr) {
    return;
  }
  std::fputs(text, stdout);
}

void hc_put_char(std::int64_t ch) {
  const int value = static_cast<int>(ch & 0xff);
  std::fputc(value, stdout);
}

void hc_print_fmt(const char* format, const std::int64_t* args, std::size_t arg_count) {
  if (format == nullptr) {
    return;
  }

  std::size_t arg_index = 0;
  auto next_arg = [&](std::int64_t fallback = 0) -> std::int64_t {
    if (args == nullptr || arg_index >= arg_count) {
      return fallback;
    }
    return args[arg_index++];
  };

  std::size_t i = 0;
  while (format[i] != '\0') {
    if (format[i] != '%') {
      std::fputc(format[i], stdout);
      ++i;
      continue;
    }

    const std::size_t spec_begin = i++;
    if (format[i] == '%') {
      std::fputc('%', stdout);
      ++i;
      continue;
    }

    while (format[i] == '-' || format[i] == '+' || format[i] == ' ' || format[i] == '#' ||
           format[i] == '0' || format[i] == '\'') {
      ++i;
    }
    bool width_from_arg = false;
    bool precision_from_arg = false;
    if (format[i] == '*') {
      width_from_arg = true;
      ++i;
    } else {
      while (format[i] >= '0' && format[i] <= '9') {
        ++i;
      }
    }
    if (format[i] == '.') {
      ++i;
      if (format[i] == '*') {
        precision_from_arg = true;
        ++i;
      } else {
        while (format[i] >= '0' && format[i] <= '9') {
          ++i;
        }
      }
    }
    while (format[i] == 'h' || format[i] == 'l' || format[i] == 'j' || format[i] == 't' ||
           format[i] == 'L' || format[i] == 'q') {
      const char lm = format[i++];
      if ((lm == 'h' || lm == 'l') && format[i] == lm) {
        ++i;
      }
    }

    const char conv = format[i];
    if (conv == '\0') {
      break;
    }
    ++i;
    const int width_arg = width_from_arg ? static_cast<int>(next_arg()) : 0;
    const int precision_arg = precision_from_arg ? static_cast<int>(next_arg()) : 0;

    auto print_signed = [&](const char* spec, long long value) {
      if (width_from_arg && precision_from_arg) {
        std::fprintf(stdout, spec, width_arg, precision_arg, value);
      } else if (width_from_arg) {
        std::fprintf(stdout, spec, width_arg, value);
      } else if (precision_from_arg) {
        std::fprintf(stdout, spec, precision_arg, value);
      } else {
        std::fprintf(stdout, spec, value);
      }
    };
    auto print_unsigned = [&](const char* spec, unsigned long long value) {
      if (width_from_arg && precision_from_arg) {
        std::fprintf(stdout, spec, width_arg, precision_arg, value);
      } else if (width_from_arg) {
        std::fprintf(stdout, spec, width_arg, value);
      } else if (precision_from_arg) {
        std::fprintf(stdout, spec, precision_arg, value);
      } else {
        std::fprintf(stdout, spec, value);
      }
    };
    auto print_int = [&](const char* spec, int value) {
      if (width_from_arg && precision_from_arg) {
        std::fprintf(stdout, spec, width_arg, precision_arg, value);
      } else if (width_from_arg) {
        std::fprintf(stdout, spec, width_arg, value);
      } else if (precision_from_arg) {
        std::fprintf(stdout, spec, precision_arg, value);
      } else {
        std::fprintf(stdout, spec, value);
      }
    };
    auto print_pointer = [&](const char* spec, const void* ptr) {
      if (width_from_arg && precision_from_arg) {
        std::fprintf(stdout, spec, width_arg, precision_arg, ptr);
      } else if (width_from_arg) {
        std::fprintf(stdout, spec, width_arg, ptr);
      } else if (precision_from_arg) {
        std::fprintf(stdout, spec, precision_arg, ptr);
      } else {
        std::fprintf(stdout, spec, ptr);
      }
    };
    auto print_cstr = [&](const char* spec, const char* text) {
      if (width_from_arg && precision_from_arg) {
        std::fprintf(stdout, spec, width_arg, precision_arg, text);
      } else if (width_from_arg) {
        std::fprintf(stdout, spec, width_arg, text);
      } else if (precision_from_arg) {
        std::fprintf(stdout, spec, precision_arg, text);
      } else {
        std::fprintf(stdout, spec, text);
      }
    };
    auto print_double = [&](const char* spec, double value) {
      if (width_from_arg && precision_from_arg) {
        std::fprintf(stdout, spec, width_arg, precision_arg, value);
      } else if (width_from_arg) {
        std::fprintf(stdout, spec, width_arg, value);
      } else if (precision_from_arg) {
        std::fprintf(stdout, spec, precision_arg, value);
      } else {
        std::fprintf(stdout, spec, value);
      }
    };
    auto unpack_double = [](std::int64_t raw) {
      const std::uint64_t bits = static_cast<std::uint64_t>(raw);
      double value = 0.0;
      static_assert(sizeof(value) == sizeof(bits), "double payload size mismatch");
      std::memcpy(&value, &bits, sizeof(value));
      return value;
    };

    if (conv == 'z') {
      const std::int64_t idx = next_arg();
      const char* table =
          reinterpret_cast<const char*>(static_cast<std::uintptr_t>(next_arg()));
      std::fputs(LookupZString(table, idx), stdout);
      continue;
    }
    if (conv == 'b') {
      PrintBinary(static_cast<std::uint64_t>(next_arg()));
      continue;
    }

    char spec[64];
    std::size_t spec_len = i - spec_begin;
    if (spec_len >= sizeof(spec)) {
      spec_len = sizeof(spec) - 1;
    }
    std::memcpy(spec, format + spec_begin, spec_len);
    spec[spec_len] = '\0';
    switch (conv) {
      case 'd':
      case 'i':
        print_signed(spec, static_cast<long long>(next_arg()));
        break;
      case 'u':
      case 'x':
      case 'X':
      case 'o':
        print_unsigned(spec, static_cast<unsigned long long>(next_arg()));
        break;
      case 'p': {
        char pointer_spec[64];
        std::memcpy(pointer_spec, spec, spec_len + 1);
        pointer_spec[spec_len - 1] = 'p';
        void* ptr = reinterpret_cast<void*>(static_cast<std::uintptr_t>(next_arg()));
        print_pointer(pointer_spec, ptr);
        break;
      }
      case 'P': {
        const std::uintptr_t raw = static_cast<std::uintptr_t>(next_arg());
        if (raw == 0) {
          std::fputs("0x0", stdout);
          break;
        }
        char pointer_spec[64];
        std::memcpy(pointer_spec, spec, spec_len + 1);
        pointer_spec[spec_len - 1] = 'p';
        void* ptr = reinterpret_cast<void*>(raw);
        print_pointer(pointer_spec, ptr);
        break;
      }
      case 'c':
        print_int(spec, static_cast<int>(next_arg() & 0xff));
        break;
      case 's': {
        const char* text =
            reinterpret_cast<const char*>(static_cast<std::uintptr_t>(next_arg()));
        print_cstr(spec, text == nullptr ? "(null)" : text);
        break;
      }
      case 'f':
      case 'F':
      case 'e':
      case 'E':
      case 'g':
      case 'G':
        print_double(spec, unpack_double(next_arg()));
        break;
      default:
        std::fwrite(format + spec_begin, 1, i - spec_begin, stdout);
        break;
    }
  }
}

void hc_try_push(hc_try_frame* frame) {
  if (frame == nullptr) {
    return;
  }
  frame->prev = g_try_stack;
  g_try_stack = frame;
}

void hc_try_pop(hc_try_frame* frame) {
  if (frame == nullptr || g_try_stack == nullptr) {
    return;
  }
  if (g_try_stack == frame) {
    g_try_stack = frame->prev;
    return;
  }

  hc_try_frame* prev = g_try_stack;
  hc_try_frame* cur = g_try_stack->prev;
  while (cur != nullptr) {
    if (cur == frame) {
      prev->prev = cur->prev;
      return;
    }
    prev = cur;
    cur = cur->prev;
  }
}

[[noreturn]] void hc_throw_i64(std::int64_t payload) {
  g_exception_payload = payload;
  if (g_try_stack != nullptr) {
    hc_try_frame* frame = g_try_stack;
    g_try_stack = frame->prev;
    longjmp(frame->env, 1);
  }

  std::fprintf(stderr, "fatal runtime error: uncaught HolyC exception payload=%lld\n",
               static_cast<long long>(payload));
  std::abort();
}

std::int64_t hc_exception_payload() {
  return g_exception_payload;
}

std::int64_t hc_exception_active() {
  return g_try_stack != nullptr ? 1 : 0;
}

std::int64_t hc_try_depth() {
  std::int64_t depth = 0;
  for (const hc_try_frame* cur = g_try_stack; cur != nullptr; cur = cur->prev) {
    ++depth;
  }
  return depth;
}

void hc_register_reflection_table(const hc_reflection_field* fields, std::size_t field_count) {
  g_reflection_fields = fields;
  g_reflection_field_count = field_count;
  ClearReflectionCache();
}

std::size_t hc_reflection_field_count() {
  return g_reflection_field_count;
}

const hc_reflection_field* hc_reflection_fields() {
  return g_reflection_fields;
}

void* hc_malloc(std::size_t size) {
  return std::malloc(size);
}

void hc_free(void* ptr) {
  std::free(ptr);
}

void* hc_memcpy(void* dst, const void* src, std::size_t size) {
  return std::memcpy(dst, src, size);
}

void* hc_memset(void* dst, int value, std::size_t size) {
  return std::memset(dst, value, size);
}

std::int64_t CallStkGrow(std::int64_t stack_min, std::int64_t stack_max, const char* fn,
                         std::int64_t a0, std::int64_t a1, std::int64_t a2) {
  (void)stack_min;
  (void)stack_max;
  if (fn == nullptr) {
    return 0;
  }
  using StkGrowFn = std::int64_t (*)(std::int64_t, std::int64_t, std::int64_t);
  StkGrowFn callee = reinterpret_cast<StkGrowFn>(reinterpret_cast<std::uintptr_t>(fn));
  return callee(a0, a1, a2);
}

CTask* Spawn(const char* fn, const char* data, const char* task_name, std::int64_t target_cpu,
             CTask* parent, std::int64_t stk_size, std::int64_t flags) {
  (void)task_name;
  (void)target_cpu;
  (void)parent;
  (void)flags;
  if (fn == nullptr) {
    return nullptr;
  }

  HcSpawnRequest* req = static_cast<HcSpawnRequest*>(std::calloc(1, sizeof(HcSpawnRequest)));
  if (req == nullptr) {
    return nullptr;
  }
  req->fn = fn;
  req->data = data;

  pthread_attr_t attr;
  pthread_attr_t* attr_ptr = nullptr;
  bool attr_initialized = false;
  if (pthread_attr_init(&attr) == 0) {
    attr_initialized = true;
    const std::size_t normalized_stack_size = NormalizeStackSize(stk_size);
    if (normalized_stack_size > 0 &&
        pthread_attr_setstacksize(&attr, normalized_stack_size) != 0) {
      pthread_attr_destroy(&attr);
      std::free(req);
      return nullptr;
    }
    attr_ptr = &attr;
  }

  MarkSpawnStart();
  pthread_t thread{};
  const int rc = pthread_create(&thread, attr_ptr, SpawnThreadMain, req);
  if (attr_initialized) {
    pthread_attr_destroy(&attr);
  }
  if (rc != 0) {
    std::free(req);
    MarkSpawnDone();
    return nullptr;
  }
  pthread_detach(thread);

  const std::uintptr_t task_id = static_cast<std::uintptr_t>(
      g_next_task_id.fetch_add(1, std::memory_order_relaxed));
  return reinterpret_cast<CTask*>(task_id);
}

CJob* JobQue(const char* fn, const char* arg, std::int64_t cpu, std::int64_t flags) {
  (void)cpu;
  (void)flags;
  CJob* job = static_cast<CJob*>(std::calloc(1, sizeof(CJob)));
  if (job == nullptr) {
    return nullptr;
  }
  job->fn = fn;
  job->arg = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(arg));
  job->result = 0;
  job->joined = 0;

  if (pthread_create(&job->thread, nullptr, JobThreadMain, job) != 0) {
    std::free(job);
    return nullptr;
  }
  return job;
}

std::int64_t JobResGet(CJob* job) {
  if (job == nullptr) {
    return 0;
  }
  if (!job->joined) {
    pthread_join(job->thread, nullptr);
    job->joined = 1;
  }
  const std::int64_t result = job->result;
  std::free(job);
  return result;
}

CHashClass* HashFind(const char* name, const char* table, std::int64_t kind) {
  (void)table;
  (void)kind;
  EnsureReflectionCache();
  return FindClassByName(name);
}

std::int64_t MemberMetaData(const char* key, const CMemberLst* member) {
  if (key == nullptr || member == nullptr) {
    return 0;
  }
  for (const HcMemberMeta* meta = member->meta; meta != nullptr; meta = meta->next) {
    if (meta->key != nullptr && std::strcmp(meta->key, key) == 0) {
      return meta->value;
    }
  }
  return 0;
}

std::int64_t MemberMetaFind(const char* key, const CMemberLst* member) {
  if (key == nullptr || member == nullptr) {
    return 0;
  }
  for (const HcMemberMeta* meta = member->meta; meta != nullptr; meta = meta->next) {
    if (meta->key != nullptr && std::strcmp(meta->key, key) == 0) {
      return static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(meta));
    }
  }
  return 0;
}

std::int64_t hc_task_spawn(const char* task_name) {
  if (task_name == nullptr || task_name[0] == '\0') {
    return -1;
  }

  char* command_copy = CopyCString(task_name);
  if (command_copy == nullptr) {
    return -1;
  }

  const CTask* task = Spawn(
      reinterpret_cast<const char*>(reinterpret_cast<std::uintptr_t>(&TaskSpawnCommandEntry)),
      command_copy, task_name, -1, nullptr, 0, 0);
  if (task == nullptr) {
    std::free(command_copy);
    return -1;
  }
  return static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(task));
}

void hc_spawn_wait_all() {
  pthread_mutex_lock(&g_spawn_mutex);
  while (g_spawn_inflight > 0) {
    pthread_cond_wait(&g_spawn_cond, &g_spawn_mutex);
  }
  pthread_mutex_unlock(&g_spawn_mutex);
}

}  // extern "C"

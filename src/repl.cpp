#include "repl.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

#include "frontend.h"
#include "llvm_backend.h"
#include "parser.h"
#include "preprocessor.h"

namespace holyc::repl {

namespace {

using frontend::internal::ParsedNode;

std::string TrimCopy(std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return std::string(value.substr(begin, end - begin));
}

bool HasModifier(std::string_view decl_text, std::string_view modifier) {
  std::istringstream stream(TrimCopy(decl_text));
  std::string token;
  while (stream >> token) {
    if (token == modifier) {
      return true;
    }
  }
  return false;
}

std::string FirstToken(std::string_view text) {
  std::istringstream stream(TrimCopy(text));
  std::string token;
  stream >> token;
  return token;
}

const ParsedNode* FindChildByKind(const ParsedNode& node, std::string_view kind) {
  const auto it = std::find_if(node.children.begin(), node.children.end(),
                               [kind](const ParsedNode& child) { return child.kind == kind; });
  if (it != node.children.end()) {
    return &(*it);
  }
  return nullptr;
}

std::string BuildFunctionPrototype(const ParsedNode& fn) {
  std::ostringstream out;
  out << TrimCopy(fn.text) << "(";

  const ParsedNode* params = FindChildByKind(fn, "ParamList");
  if (params != nullptr) {
    bool first_param = true;
    for (const ParsedNode& param : params->children) {
      if (param.kind != "Param") {
        continue;
      }
      if (!first_param) {
        out << ", ";
      }
      first_param = false;
      out << TrimCopy(param.text);
      if (const ParsedNode* default_expr = FindChildByKind(param, "Default")) {
        const std::string default_text = TrimCopy(default_expr->text);
        if (!default_text.empty()) {
          out << " = " << default_text;
        }
      }
    }
  }

  out << ");";
  return out.str();
}

std::string BuildExternVarDecl(const ParsedNode& var_decl) {
  const std::string decl = TrimCopy(var_decl.text);
  if (decl.empty()) {
    return "";
  }

  if (HasModifier(decl, "static")) {
    return "";
  }

  const std::string first = FirstToken(decl);
  if (first == "extern" || first == "import" || first == "_extern" || first == "_import" ||
      first == "export" || first == "_export") {
    return decl + ";";
  }

  return "extern " + decl + ";";
}

std::string BuildTypeAliasDecl(const ParsedNode& alias_decl) {
  const std::string text = TrimCopy(alias_decl.text);
  if (text.empty()) {
    return "";
  }
  return "typedef " + text + ";";
}

std::string BuildLinkageDecl(const ParsedNode& linkage_decl) {
  if (linkage_decl.children.empty()) {
    return "";
  }
  const std::string payload = TrimCopy(linkage_decl.children[0].text);
  if (payload.empty()) {
    return "";
  }
  return TrimCopy(linkage_decl.text) + " " + payload + ";";
}

std::string RenderClassDecl(const ParsedNode& class_decl, int indent = 0) {
  const std::string base_indent(static_cast<std::size_t>(indent), ' ');
  bool has_body_items = false;
  for (const ParsedNode& child : class_decl.children) {
    if (child.kind == "VarDecl") {
      continue;
    }
    has_body_items = true;
    break;
  }

  if (!has_body_items) {
    return base_indent + TrimCopy(class_decl.text) + ";";
  }

  std::ostringstream out;
  out << base_indent << TrimCopy(class_decl.text) << " {\n";
  for (const ParsedNode& child : class_decl.children) {
    if (child.kind == "VarDecl") {
      continue;
    }
    if (child.kind == "FieldDecl") {
      out << base_indent << "  " << TrimCopy(child.text) << ";\n";
      continue;
    }
    if (child.kind == "TypeAliasDecl") {
      out << base_indent << "  " << BuildTypeAliasDecl(child) << "\n";
      continue;
    }
    if (child.kind == "ClassDecl") {
      out << RenderClassDecl(child, indent + 2) << "\n";
    }
  }
  out << base_indent << "};";
  return out.str();
}

bool IsDeclarationTopLevelKind(std::string_view kind) {
  return kind == "FunctionDecl" || kind == "VarDecl" || kind == "VarDeclList" ||
         kind == "TypeAliasDecl" || kind == "ClassDecl" || kind == "LinkageDecl" ||
         kind == "StartLabel" || kind == "EndLabel";
}

enum class CellKind {
  kEmpty,
  kDeclaration,
  kExecutable,
  kMixed,
};

CellKind ClassifyCell(const ParsedNode& program) {
  bool has_declaration = false;
  bool has_executable = false;

  for (const ParsedNode& child : program.children) {
    if (child.kind == "EmptyStmt") {
      continue;
    }

    if (IsDeclarationTopLevelKind(child.kind)) {
      has_declaration = true;
    } else {
      has_executable = true;
    }
  }

  if (!has_declaration && !has_executable) {
    return CellKind::kEmpty;
  }
  if (has_declaration && has_executable) {
    return CellKind::kMixed;
  }
  return has_declaration ? CellKind::kDeclaration : CellKind::kExecutable;
}

bool IsSingleExpressionCell(const ParsedNode& program) {
  const ParsedNode* expression_stmt = nullptr;
  for (const ParsedNode& child : program.children) {
    if (child.kind == "EmptyStmt") {
      continue;
    }
    if (expression_stmt != nullptr || child.kind != "ExprStmt") {
      return false;
    }
    expression_stmt = &child;
  }
  return expression_stmt != nullptr;
}

enum class InputReadiness {
  kEmpty,
  kIncomplete,
  kComplete,
  kInvalid,
};

std::size_t CountLines(std::string_view text) {
  if (text.empty()) {
    return 0;
  }
  return 1 + static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
}

std::size_t LineLength(std::string_view text, std::size_t one_based_line) {
  if (one_based_line == 0) {
    return 0;
  }
  std::size_t current = 1;
  std::size_t start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (current == one_based_line) {
      start = i;
      break;
    }
    if (text[i] == '\n') {
      ++current;
    }
  }
  if (current != one_based_line) {
    return 0;
  }
  std::size_t end = start;
  while (end < text.size() && text[end] != '\n') {
    ++end;
  }
  return end - start;
}

bool ParseDiagnosticLocation(std::string_view message, std::size_t* out_line, std::size_t* out_col) {
  std::size_t best_line = 0;
  std::size_t best_col = 0;
  bool found = false;

  for (std::size_t i = 0; i + 4 < message.size(); ++i) {
    if (message[i] != ':') {
      continue;
    }
    std::size_t line_begin = i + 1;
    std::size_t line_end = line_begin;
    while (line_end < message.size() &&
           std::isdigit(static_cast<unsigned char>(message[line_end])) != 0) {
      ++line_end;
    }
    if (line_end == line_begin || line_end >= message.size() || message[line_end] != ':') {
      continue;
    }
    std::size_t col_begin = line_end + 1;
    std::size_t col_end = col_begin;
    while (col_end < message.size() &&
           std::isdigit(static_cast<unsigned char>(message[col_end])) != 0) {
      ++col_end;
    }
    if (col_end == col_begin || col_end >= message.size() || message[col_end] != ':') {
      continue;
    }

    best_line = static_cast<std::size_t>(
        std::strtoull(std::string(message.substr(line_begin, line_end - line_begin)).c_str(),
                      nullptr, 10));
    best_col = static_cast<std::size_t>(
        std::strtoull(std::string(message.substr(col_begin, col_end - col_begin)).c_str(),
                      nullptr, 10));
    found = true;
  }

  if (!found) {
    return false;
  }
  *out_line = best_line;
  *out_col = best_col;
  return true;
}

bool HasUnclosedLexicalScope(std::string_view text) {
  int paren_depth = 0;
  int brace_depth = 0;
  int bracket_depth = 0;
  bool in_string = false;
  char string_quote = '\0';
  bool in_line_comment = false;
  bool in_block_comment = false;
  bool escaped = false;

  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    const char next = (i + 1 < text.size()) ? text[i + 1] : '\0';

    if (in_line_comment) {
      if (c == '\n') {
        in_line_comment = false;
      }
      continue;
    }

    if (in_block_comment) {
      if (c == '*' && next == '/') {
        in_block_comment = false;
        ++i;
      }
      continue;
    }

    if (in_string) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (c == '\\') {
        escaped = true;
        continue;
      }
      if (c == string_quote) {
        in_string = false;
        string_quote = '\0';
      }
      continue;
    }

    if (c == '/' && next == '/') {
      in_line_comment = true;
      ++i;
      continue;
    }
    if (c == '/' && next == '*') {
      in_block_comment = true;
      ++i;
      continue;
    }

    if (c == '"' || c == '\'') {
      in_string = true;
      string_quote = c;
      escaped = false;
      continue;
    }

    if (c == '(') {
      ++paren_depth;
    } else if (c == ')') {
      --paren_depth;
    } else if (c == '{') {
      ++brace_depth;
    } else if (c == '}') {
      --brace_depth;
    } else if (c == '[') {
      ++bracket_depth;
    } else if (c == ']') {
      --bracket_depth;
    }
  }

  if (in_string || in_block_comment) {
    return true;
  }
  return paren_depth > 0 || brace_depth > 0 || bracket_depth > 0;
}

bool LooksLikeIncompleteDiagnostic(std::string_view source, std::string_view diagnostic) {
  const std::size_t line_count = CountLines(source);
  const std::string trimmed_source = TrimCopy(source);
  const char last_char = trimmed_source.empty() ? '\0' : trimmed_source.back();
  std::size_t line = 0;
  std::size_t col = 0;
  const bool have_location = ParseDiagnosticLocation(diagnostic, &line, &col);

  auto location_is_eof = [&]() -> bool {
    if (!have_location) {
      return true;
    }
    if (line == 0) {
      return true;
    }
    if (line > line_count) {
      return true;
    }
    if (line == line_count) {
      const std::size_t line_len = LineLength(source, line_count);
      return col > line_len;
    }
    return false;
  };

  auto last_char_suggests_continuation = [&]() -> bool {
    switch (last_char) {
      case ',':
      case '(':
      case '[':
      case '{':
      case '=':
      case '+':
      case '-':
      case '*':
      case '/':
      case '%':
      case '&':
      case '|':
      case '^':
      case '!':
      case '<':
      case '>':
      case '?':
      case ':':
        return true;
      default:
        return false;
    }
  };

  if (diagnostic.find("unterminated block comment") != std::string_view::npos ||
      diagnostic.find("unterminated string") != std::string_view::npos) {
    return true;
  }

  if (diagnostic.find("unexpected token in expression") != std::string_view::npos) {
    return location_is_eof() || last_char_suggests_continuation();
  }

  if (diagnostic.find("expected ") != std::string_view::npos) {
    if (diagnostic.find("expected ';'") != std::string_view::npos) {
      if (last_char != ';' && last_char != '}') {
        return true;
      }
    }
    if (diagnostic.find("expected '}'") != std::string_view::npos && last_char != '}') {
      return true;
    }
    if (diagnostic.find("expected ')'") != std::string_view::npos && last_char != ')') {
      return true;
    }
    if (diagnostic.find("expected ']'") != std::string_view::npos && last_char != ']') {
      return true;
    }
    return location_is_eof();
  }

  return false;
}

InputReadiness AnalyzeInputReadiness(const std::string& source) {
  const std::string trimmed = TrimCopy(source);
  if (trimmed.empty()) {
    return InputReadiness::kEmpty;
  }

  if (HasUnclosedLexicalScope(source)) {
    return InputReadiness::kIncomplete;
  }

  if (trimmed[0] != '#') {
    const char last = trimmed.back();
    if (last != ';' && last != '}') {
      return InputReadiness::kIncomplete;
    }
  }

  try {
    const std::string preprocessed =
        frontend::internal::RunPreprocessor(source, "<repl>", frontend::ExecutionMode::kJit);
    (void)frontend::internal::ParseAst(preprocessed, "<repl>");
    return InputReadiness::kComplete;
  } catch (const std::exception& ex) {
    if (LooksLikeIncompleteDiagnostic(source, ex.what())) {
      return InputReadiness::kIncomplete;
    }
    return InputReadiness::kInvalid;
  }
}

bool StripTrailingSemicolon(std::string* text) {
  if (text == nullptr) {
    return false;
  }
  std::string trimmed = TrimCopy(*text);
  if (trimmed.empty() || trimmed.back() != ';') {
    return false;
  }
  trimmed.pop_back();
  *text = TrimCopy(trimmed);
  return !text->empty();
}

bool IsInteractiveStdio() {
#if defined(__unix__) || defined(__APPLE__)
  return isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0;
#else
  return false;
#endif
}

bool StdinHasBufferedInput() {
#if defined(__unix__) || defined(__APPLE__)
  int available = 0;
  if (ioctl(STDIN_FILENO, FIONREAD, &available) != 0) {
    return false;
  }
  return available > 0;
#else
  return false;
#endif
}

#if defined(__unix__) || defined(__APPLE__)

class InteractiveLineEditor {
 public:
  ~InteractiveLineEditor() { RestoreTerminal(); }

  bool StartSession() {
    return EnableRawMode();
  }

  void EndSession() {
    RestoreTerminal();
  }

  bool ReadLine(std::string_view prompt, std::string* out_line) {
    out_line->clear();
    if (!StartSession()) {
      return false;
    }

    std::string line;
    std::size_t cursor = 0;
    std::size_t history_index = history_.size();
    std::string draft_line;

    if (!WriteAll(prompt)) {
      return false;
    }

    auto redraw = [&](std::string_view active_prompt) -> bool {
      std::ostringstream render;
      render << '\r' << active_prompt << line << "\x1b[K";
      render << '\r';
      const std::size_t target_column = active_prompt.size() + cursor;
      if (target_column > 0) {
        render << "\x1b[" << target_column << 'C';
      }
      return WriteAll(render.str());
    };

    auto read_byte = [](char* out) -> bool {
      while (true) {
        const ssize_t n = ::read(STDIN_FILENO, out, 1);
        if (n == 1) {
          return true;
        }
        if (n == 0) {
          return false;
        }
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
    };

    while (true) {
      char c = '\0';
      if (!read_byte(&c)) {
        return false;
      }

      if (c == '\r' || c == '\n') {
        if (!WriteAll("\r\n")) {
          return false;
        }
        *out_line = std::move(line);
        return true;
      }

      if (c == 4) {
        if (line.empty()) {
          return false;
        }
        continue;
      }

      if (c == 127 || c == 8) {
        if (cursor > 0) {
          if (cursor == line.size()) {
            line.pop_back();
            --cursor;
            if (!WriteAll("\b \b")) {
              return false;
            }
          } else {
            line.erase(cursor - 1, 1);
            --cursor;
            if (!redraw(prompt)) {
              return false;
            }
          }
        }
        continue;
      }

      if (c == '\x1b') {
        char seq1 = '\0';
        char seq2 = '\0';
        if (!read_byte(&seq1)) {
          return false;
        }
        if (!read_byte(&seq2)) {
          return false;
        }
        if (seq1 == '[') {
          if (seq2 == 'A') {
            if (!history_.empty() && history_index > 0) {
              if (history_index == history_.size()) {
                draft_line = line;
              }
              --history_index;
              line = history_[history_index];
              cursor = line.size();
              if (!redraw(prompt)) {
                return false;
              }
            }
            continue;
          }
          if (seq2 == 'B') {
            if (history_index < history_.size()) {
              ++history_index;
              if (history_index == history_.size()) {
                line = draft_line;
              } else {
                line = history_[history_index];
              }
              cursor = line.size();
              if (!redraw(prompt)) {
                return false;
              }
            }
            continue;
          }
          if (seq2 == 'C') {
            if (cursor < line.size()) {
              ++cursor;
              if (!redraw(prompt)) {
                return false;
              }
            }
            continue;
          }
          if (seq2 == 'D') {
            if (cursor > 0) {
              --cursor;
              if (!redraw(prompt)) {
                return false;
              }
            }
            continue;
          }

          // Consume bracketed-paste markers (`ESC [ 200~` / `ESC [ 201~`) and
          // other CSI tails without altering line state.
          if ((seq2 >= '0' && seq2 <= '9') || seq2 == ';') {
            char tail = '\0';
            do {
              if (!read_byte(&tail)) {
                return false;
              }
            } while (tail != '~' && !std::isalpha(static_cast<unsigned char>(tail)));
          }
        }
        continue;
      }

      if (std::isprint(static_cast<unsigned char>(c)) != 0 || c == '\t') {
        if (cursor == line.size()) {
          line.push_back(c);
          ++cursor;
          const std::string out(1, c);
          if (!WriteAll(out)) {
            return false;
          }
        } else {
          line.insert(cursor, 1, c);
          ++cursor;
          if (!redraw(prompt)) {
            return false;
          }
        }
      }
    }
  }

  void AddHistory(std::string_view line) {
    const std::string trimmed = TrimCopy(line);
    if (trimmed.empty()) {
      return;
    }
    if (!history_.empty() && history_.back() == std::string(line)) {
      return;
    }
    history_.push_back(std::string(line));
  }

 private:
  bool EnableRawMode() {
    if (raw_mode_enabled_) {
      return true;
    }
    if (tcgetattr(STDIN_FILENO, &original_termios_) != 0) {
      return false;
    }
    struct termios raw = original_termios_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON));
    raw.c_lflag |= ISIG;
    raw.c_iflag &= static_cast<tcflag_t>(~IXON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
      return false;
    }
    raw_mode_enabled_ = true;
    return true;
  }

  void RestoreTerminal() {
    if (!raw_mode_enabled_) {
      return;
    }
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &original_termios_);
    raw_mode_enabled_ = false;
  }

  static bool WriteAll(std::string_view text) {
    const char* data = text.data();
    std::size_t remaining = text.size();
    while (remaining > 0) {
      const ssize_t wrote = ::write(STDOUT_FILENO, data, remaining);
      if (wrote < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      data += wrote;
      remaining -= static_cast<std::size_t>(wrote);
    }
    return true;
  }

  std::vector<std::string> history_;
  bool raw_mode_enabled_ = false;
  struct termios original_termios_ {};
};

#endif

bool ReadFile(std::string_view path, std::string* out) {
  std::ifstream input{std::string(path)};
  if (!input.is_open()) {
    return false;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  *out = buffer.str();
  return true;
}

struct DeclCatalog {
  void AddTypeDecl(std::string decl) {
    if (decl.empty()) {
      return;
    }
    if (type_decl_set_.insert(decl).second) {
      type_decls_.push_back(std::move(decl));
    }
  }

  void AddClassDecl(std::string decl) {
    if (decl.empty()) {
      return;
    }
    if (class_decl_set_.insert(decl).second) {
      class_decls_.push_back(std::move(decl));
    }
  }

  void AddLinkageDecl(std::string decl) {
    if (decl.empty()) {
      return;
    }
    if (linkage_decl_set_.insert(decl).second) {
      linkage_decls_.push_back(std::move(decl));
    }
  }

  void AddFunctionProto(std::string decl) {
    if (decl.empty()) {
      return;
    }
    if (function_proto_set_.insert(decl).second) {
      function_protos_.push_back(std::move(decl));
    }
  }

  void AddGlobalDecl(std::string decl) {
    if (decl.empty()) {
      return;
    }
    if (global_decl_set_.insert(decl).second) {
      global_decls_.push_back(std::move(decl));
    }
  }

  std::string BuildPrelude() const {
    std::ostringstream out;

    for (const std::string& decl : type_decls_) {
      out << decl << "\n";
    }
    for (const std::string& decl : class_decls_) {
      out << decl << "\n";
    }
    for (const std::string& decl : linkage_decls_) {
      out << decl << "\n";
    }
    for (const std::string& decl : function_protos_) {
      out << decl << "\n";
    }
    for (const std::string& decl : global_decls_) {
      out << decl << "\n";
    }

    return out.str();
  }

  void Clear() {
    type_decls_.clear();
    class_decls_.clear();
    linkage_decls_.clear();
    function_protos_.clear();
    global_decls_.clear();

    type_decl_set_.clear();
    class_decl_set_.clear();
    linkage_decl_set_.clear();
    function_proto_set_.clear();
    global_decl_set_.clear();
  }

 private:
  std::vector<std::string> type_decls_;
  std::vector<std::string> class_decls_;
  std::vector<std::string> linkage_decls_;
  std::vector<std::string> function_protos_;
  std::vector<std::string> global_decls_;

  std::unordered_set<std::string> type_decl_set_;
  std::unordered_set<std::string> class_decl_set_;
  std::unordered_set<std::string> linkage_decl_set_;
  std::unordered_set<std::string> function_proto_set_;
  std::unordered_set<std::string> global_decl_set_;
};

class ReplEngine {
 public:
  ReplEngine(bool strict_mode, std::string_view jit_session, llvm_backend::OptLevel opt_level)
      : strict_mode_(strict_mode), jit_session_(jit_session), opt_level_(opt_level) {}

  void SetStrictMode(bool strict_mode) { strict_mode_ = strict_mode; }

  bool StrictMode() const { return strict_mode_; }

  const std::string& SessionName() const { return jit_session_; }

  bool Reset() {
    const llvm_backend::Result reset = llvm_backend::ResetJitSession(jit_session_);
    if (!reset.ok) {
      std::cerr << reset.output << "\n";
      return false;
    }
    catalog_.Clear();
    cell_id_ = 0;
    return true;
  }

  static void PrintHelp() {
    std::cout
        << "REPL commands:\n"
        << "  :help           Show this help\n"
        << "  :quit | :q      Exit REPL\n"
        << "  :reset          Clear JIT session and declaration context\n"
        << "  :strict         Enable strict semantic mode\n"
        << "  :permissive     Enable permissive semantic mode\n"
        << "  :load <file>    Load and execute a HolyC file as one REPL cell\n"
        << "  (auto)          Incomplete input continues on ...> prompt\n"
        << "  :{ ... :}       Enter/exit multiline input mode\n";
  }

  bool ProcessCell(const std::string& cell_text, std::string_view origin) {
    const std::string trimmed = TrimCopy(cell_text);
    if (trimmed.empty()) {
      return true;
    }

    ParsedNode parsed;
    {
      try {
        const std::string preprocessed =
            frontend::internal::RunPreprocessor(cell_text, origin, frontend::ExecutionMode::kJit);
        parsed = frontend::internal::ParseAst(preprocessed, origin);
      } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return false;
      }
    }

    const CellKind kind = ClassifyCell(parsed);
    if (kind == CellKind::kEmpty) {
      return true;
    }
    if (kind == CellKind::kMixed) {
      std::cerr << "error: REPL cell cannot mix top-level declarations and executable statements; "
                << "split into separate inputs\n";
      return false;
    }

    const std::string prelude = catalog_.BuildPrelude();

    if (kind == CellKind::kDeclaration) {
      std::ostringstream unit;
      unit << prelude;
      if (!prelude.empty()) {
        unit << "\n";
      }
      unit << cell_text << "\n";

      const std::string filename = "<repl-decl-" + std::to_string(cell_id_ + 1) + ">";
      const frontend::ParseResult ir_result =
          frontend::EmitLlvmIr(unit.str(), filename, frontend::ExecutionMode::kJit, strict_mode_);
      if (!ir_result.ok) {
        std::cerr << ir_result.output << "\n";
        return false;
      }

      const llvm_backend::Result load_result =
          llvm_backend::LoadIrJit(ir_result.output, jit_session_, opt_level_);
      if (!load_result.ok) {
        std::cerr << load_result.output << "\n";
        return false;
      }

      IndexDeclarations(parsed);
      ++cell_id_;
      return true;
    }

    bool expression_mode = IsSingleExpressionCell(parsed);
    std::string wrapped_source;
    std::string entry_function_name;
    {
      entry_function_name = "__holyc_repl_exec_" + std::to_string(cell_id_ + 1);
      std::ostringstream unit;
      unit << prelude;
      if (!prelude.empty()) {
        unit << "\n";
      }
      unit << "I64 " << entry_function_name << "()\n{\n";

      if (expression_mode) {
        std::string expression = trimmed;
        if (!StripTrailingSemicolon(&expression)) {
          expression_mode = false;
        } else {
          unit << "  return " << expression << ";\n";
        }
      }

      if (!expression_mode) {
        unit << cell_text;
        if (!cell_text.empty() && cell_text.back() != '\n') {
          unit << "\n";
        }
        unit << "  return 0;\n";
      }

      unit << "}\n";
      wrapped_source = unit.str();
    }

    const std::string filename = "<repl-exec-" + std::to_string(cell_id_ + 1) + ">";
    const frontend::ParseResult ir_result =
        frontend::EmitLlvmIr(wrapped_source, filename, frontend::ExecutionMode::kJit, strict_mode_);
    if (!ir_result.ok) {
      std::cerr << ir_result.output << "\n";
      return false;
    }

    const llvm_backend::Result jit_result = llvm_backend::ExecuteIrJit(
        ir_result.output, jit_session_, false, entry_function_name, opt_level_);
    if (!jit_result.ok) {
      std::cerr << jit_result.output << "\n";
      return false;
    }

    if (expression_mode) {
      std::cout << jit_result.output;
    }

    ++cell_id_;
    return true;
  }

 private:
  void IndexDeclarations(const ParsedNode& program) {
    for (const ParsedNode& child : program.children) {
      if (child.kind == "TypeAliasDecl") {
        catalog_.AddTypeDecl(BuildTypeAliasDecl(child));
        continue;
      }

      if (child.kind == "ClassDecl") {
        catalog_.AddClassDecl(RenderClassDecl(child));
        for (const ParsedNode& class_child : child.children) {
          if (class_child.kind == "VarDecl") {
            catalog_.AddGlobalDecl(BuildExternVarDecl(class_child));
          }
        }
        continue;
      }

      if (child.kind == "LinkageDecl") {
        catalog_.AddLinkageDecl(BuildLinkageDecl(child));
        continue;
      }

      if (child.kind == "FunctionDecl") {
        if (!HasModifier(child.text, "static")) {
          catalog_.AddFunctionProto(BuildFunctionPrototype(child));
        }
        continue;
      }

      if (child.kind == "VarDecl") {
        catalog_.AddGlobalDecl(BuildExternVarDecl(child));
        continue;
      }

      if (child.kind == "VarDeclList") {
        for (const ParsedNode& var_child : child.children) {
          if (var_child.kind == "VarDecl") {
            catalog_.AddGlobalDecl(BuildExternVarDecl(var_child));
          }
        }
      }
    }
  }

  bool strict_mode_ = true;
  std::string jit_session_;
  llvm_backend::OptLevel opt_level_ = llvm_backend::OptLevel::kO1;
  std::uint64_t cell_id_ = 0;
  DeclCatalog catalog_;
};

}  // namespace

int RunRepl(bool strict_mode, std::string_view jit_session, bool jit_reset,
            llvm_backend::OptLevel opt_level) {
  ReplEngine engine(strict_mode, jit_session.empty() ? "__repl__" : jit_session, opt_level);
  if (jit_reset && !engine.Reset()) {
    return 1;
  }

#if defined(__unix__) || defined(__APPLE__)
  const bool interactive = IsInteractiveStdio();
  InteractiveLineEditor line_editor;
  if (interactive && !line_editor.StartSession()) {
    std::cerr << "error: failed to initialize interactive terminal mode\n";
    return 1;
  }
#endif
  bool explicit_multiline = false;
  std::string explicit_multiline_buffer;
  std::string pending_input;

  for (std::string line; ; ) {
#if defined(__unix__) || defined(__APPLE__)
    if (interactive) {
      const bool continuation_prompt = explicit_multiline || !TrimCopy(pending_input).empty();
      std::string prompt = continuation_prompt ? "...> " : "holyc> ";
      if (StdinHasBufferedInput()) {
        prompt.clear();
      }
      if (!line_editor.ReadLine(prompt, &line)) {
        break;
      }
    } else {
      if (!std::getline(std::cin, line)) {
        break;
      }
    }
    if (interactive) {
      line_editor.AddHistory(line);
    }
#else
    if (!std::getline(std::cin, line)) {
      break;
    }
#endif

    const std::string trimmed = TrimCopy(line);
    if (explicit_multiline) {
      if (trimmed == ":}") {
        engine.ProcessCell(explicit_multiline_buffer, "<repl-multiline>");
        explicit_multiline_buffer.clear();
        explicit_multiline = false;
        continue;
      }
      explicit_multiline_buffer.append(line);
      explicit_multiline_buffer.push_back('\n');
      continue;
    }

    if (pending_input.empty() && trimmed == ":{") {
      explicit_multiline = true;
      explicit_multiline_buffer.clear();
      continue;
    }

    if (pending_input.empty() && !trimmed.empty() && trimmed.front() == ':') {
      if (trimmed == ":quit" || trimmed == ":q") {
        break;
      }
      if (trimmed == ":help") {
        ReplEngine::PrintHelp();
        continue;
      }
      if (trimmed == ":reset") {
        engine.Reset();
        continue;
      }
      if (trimmed == ":strict") {
        engine.SetStrictMode(true);
        continue;
      }
      if (trimmed == ":permissive") {
        engine.SetStrictMode(false);
        continue;
      }
      if (trimmed.rfind(":load", 0) == 0) {
        std::string path = TrimCopy(trimmed.substr(5));
        if (path.empty()) {
          std::cerr << "error: :load requires a file path\n";
          continue;
        }
        std::string file_contents;
        if (!ReadFile(path, &file_contents)) {
          std::cerr << "error: cannot read file: " << path << "\n";
          continue;
        }
        engine.ProcessCell(file_contents, path);
        continue;
      }

      std::cerr << "error: unknown REPL command: " << trimmed << "\n";
      continue;
    }

    if (trimmed.empty() && pending_input.empty()) {
      continue;
    }

    pending_input.append(line);
    pending_input.push_back('\n');
    const InputReadiness readiness = AnalyzeInputReadiness(pending_input);
    if (readiness == InputReadiness::kEmpty) {
      pending_input.clear();
      continue;
    }
    if (readiness == InputReadiness::kIncomplete) {
      continue;
    }

    engine.ProcessCell(pending_input, "<repl>");
    pending_input.clear();
  }

#if defined(__unix__) || defined(__APPLE__)
  if (interactive) {
    line_editor.EndSession();
  }
#endif

  if (explicit_multiline && !TrimCopy(explicit_multiline_buffer).empty()) {
    std::cerr << "error: unterminated multiline input; use :} to execute the cell\n";
    return 1;
  }

  if (!TrimCopy(pending_input).empty()) {
    const InputReadiness readiness = AnalyzeInputReadiness(pending_input);
    if (readiness == InputReadiness::kIncomplete) {
      std::cerr << "error: unterminated input; keep typing or terminate constructs before EOF\n";
      return 1;
    }
    engine.ProcessCell(pending_input, "<repl-eof>");
  }

  return 0;
}

}  // namespace holyc::repl

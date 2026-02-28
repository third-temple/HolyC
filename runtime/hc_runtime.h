#pragma once

#include <cstddef>
#include <cstdint>
#include <setjmp.h>

#define HC_RUNTIME_ABI_VERSION_MAJOR 1
#define HC_RUNTIME_ABI_VERSION_MINOR 0

extern "C" {

std::int64_t hc_runtime_abi_version();

void hc_print_str(const char* text);
void hc_put_char(std::int64_t ch);
void hc_print_fmt(const char* format, const std::int64_t* args, std::size_t arg_count);

typedef struct hc_try_frame {
  jmp_buf env;
  struct hc_try_frame* prev;
} hc_try_frame;

void hc_try_push(hc_try_frame* frame);
void hc_try_pop(hc_try_frame* frame);
[[noreturn]] void hc_throw_i64(std::int64_t payload);
std::int64_t hc_exception_payload();
std::int64_t hc_exception_active();
std::int64_t hc_try_depth();

typedef struct hc_reflection_field {
  const char* aggregate_name;
  const char* field_name;
  const char* field_type;
  const char* annotations;
} hc_reflection_field;

void hc_register_reflection_table(const hc_reflection_field* fields, std::size_t field_count);
std::size_t hc_reflection_field_count();
const hc_reflection_field* hc_reflection_fields();

void* hc_malloc(std::size_t size);
void hc_free(void* ptr);
void* hc_memcpy(void* dst, const void* src, std::size_t size);
void* hc_memset(void* dst, int value, std::size_t size);

typedef struct CJob CJob;
typedef struct CTask CTask;
typedef struct CHashClass CHashClass;
typedef struct CMemberLst CMemberLst;

std::int64_t CallStkGrow(std::int64_t stack_min, std::int64_t stack_max, const char* fn,
                         std::int64_t a0, std::int64_t a1, std::int64_t a2);
CTask* Spawn(const char* fn, const char* data, const char* task_name, std::int64_t target_cpu,
             CTask* parent, std::int64_t stk_size, std::int64_t flags);
CJob* JobQue(const char* fn, const char* arg, std::int64_t cpu, std::int64_t flags);
std::int64_t JobResGet(CJob* job);
CHashClass* HashFind(const char* name, const char* table, std::int64_t kind);
std::int64_t MemberMetaData(const char* key, const CMemberLst* member);
std::int64_t MemberMetaFind(const char* key, const CMemberLst* member);
std::int64_t hc_task_spawn(const char* task_name);
void hc_spawn_wait_all();

}

#define hc_try_begin(frame_ptr) (hc_try_push((frame_ptr)), setjmp((frame_ptr)->env))
#define hc_try_end(frame_ptr) hc_try_pop((frame_ptr))

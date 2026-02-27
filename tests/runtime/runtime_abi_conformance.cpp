#include "hc_runtime.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

namespace {

volatile std::int64_t g_job_seen = 0;
std::atomic<std::int64_t> g_spawn_seen{0};

extern "C" std::int64_t AbiConformanceFn(std::int64_t a0, std::int64_t a1, std::int64_t a2) {
  return a0 + a1 + a2;
}

extern "C" void AbiConformanceJob(std::int64_t arg) {
  g_job_seen = arg;
}

extern "C" void AbiConformanceSpawn(const char* arg) {
  g_spawn_seen.store(static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(arg)),
                     std::memory_order_release);
}

struct CHashClassView {
  CMemberLst* member_lst_and_root;
};

}  // namespace

int main() {
  const std::int64_t abi = hc_runtime_abi_version();
  if ((abi >> 32) != HC_RUNTIME_ABI_VERSION_MAJOR) {
    return 1;
  }

  char src[8] = {};
  char dst[8] = {};
  hc_memset(src, 'A', 3);
  src[3] = '\0';
  hc_memcpy(dst, src, 4);
  if (std::strcmp(dst, "AAA") != 0) {
    return 2;
  }

  const hc_reflection_field fields[] = {
      {"Pair", "a", "I64", "visible"},
      {"Demo", "age", "I64", "dft_val 9 print_str \"%d\""},
  };
  hc_register_reflection_table(fields, 2);
  if (hc_reflection_field_count() != 2) {
    return 3;
  }
  const hc_reflection_field* reflected = hc_reflection_fields();
  if (reflected == nullptr || std::strcmp(reflected[0].field_name, "a") != 0) {
    return 4;
  }

  hc_try_frame frame{};
  const int state = hc_try_begin(&frame);
  if (state == 0) {
    if (hc_exception_active() == 0 || hc_try_depth() != 1) {
      return 5;
    }
    hc_throw_i64(42);
    return 6;
  }
  if (hc_exception_payload() != 42) {
    return 7;
  }
  hc_try_end(&frame);
  if (hc_exception_active() != 0 || hc_try_depth() != 0) {
    return 8;
  }

  void* ptr = hc_malloc(16);
  if (ptr == nullptr) {
    return 9;
  }
  hc_free(ptr);

  const std::int64_t stkgrow = CallStkGrow(
      0x100, 0x1000, reinterpret_cast<const char*>(reinterpret_cast<std::uintptr_t>(&AbiConformanceFn)),
      1, 2, 3);
  if (stkgrow != 6) {
    return 10;
  }

  CJob* job = JobQue(reinterpret_cast<const char*>(reinterpret_cast<std::uintptr_t>(&AbiConformanceJob)),
                     reinterpret_cast<const char*>(static_cast<std::uintptr_t>(17)), 0, 0);
  if (job == nullptr) {
    return 11;
  }
  (void)JobResGet(job);
  if (g_job_seen != 17) {
    return 12;
  }

  CTask* task = Spawn(
      reinterpret_cast<const char*>(reinterpret_cast<std::uintptr_t>(&AbiConformanceSpawn)),
      reinterpret_cast<const char*>(static_cast<std::uintptr_t>(23)), "abi-spawn", -1, nullptr, 0,
      0);
  if (task == nullptr) {
    return 13;
  }
  for (int i = 0; i < 2000; ++i) {
    if (g_spawn_seen.load(std::memory_order_acquire) == 23) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (g_spawn_seen.load(std::memory_order_acquire) != 23) {
    return 14;
  }

  CHashClass* demo = HashFind("Demo", nullptr, 0);
  if (demo == nullptr) {
    return 15;
  }
  CMemberLst* member = reinterpret_cast<CHashClassView*>(demo)->member_lst_and_root;
  if (member == nullptr) {
    return 16;
  }
  if (MemberMetaFind("dft_val", member) == 0) {
    return 17;
  }
  if (MemberMetaData("dft_val", member) != 9) {
    return 18;
  }

  if (hc_task_spawn(":") <= 0) {
    return 19;
  }

  return 0;
}

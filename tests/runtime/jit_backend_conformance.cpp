#include "llvm_backend.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using holyc::llvm_backend::Result;

bool ParseReturnCode(const std::string& output, int* value_out) {
  if (value_out == nullptr) {
    return false;
  }
  try {
    *value_out = std::stoi(output);
    return true;
  } catch (...) {
    return false;
  }
}

bool ExpectOk(std::string_view name, const Result& result, int expected_rc) {
  if (!result.ok) {
    std::cerr << "FAIL(" << name << "): expected success, got error: " << result.output << "\n";
    return false;
  }
  int rc = 0;
  if (!ParseReturnCode(result.output, &rc)) {
    std::cerr << "FAIL(" << name << "): expected numeric output, got: " << result.output << "\n";
    return false;
  }
  if (rc != expected_rc) {
    std::cerr << "FAIL(" << name << "): expected rc=" << expected_rc << ", got " << rc << "\n";
    return false;
  }
  return true;
}

bool ExpectFail(std::string_view name, const Result& result) {
  if (result.ok) {
    std::cerr << "FAIL(" << name << "): expected failure, got success output: " << result.output
              << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  constexpr std::string_view kSession = "runtime-jit-backend-conformance";

  // Keep the same session alive across checks to validate shadowing/inter-op.
  const Result reset_before = holyc::llvm_backend::ResetJitSession(kSession);
  if (!reset_before.ok) {
    std::cerr << "FAIL(reset-before): " << reset_before.output << "\n";
    return 1;
  }

  const std::string ir_v1 = R"(
define i32 @Foo() {
entry:
  ret i32 1
}

define i32 @main() {
entry:
  %v = call i32 @Foo()
  ret i32 %v
}
)";
  if (!ExpectOk("v1", holyc::llvm_backend::ExecuteIrJit(ir_v1, kSession, false), 1)) {
    return 1;
  }

  const std::string ir_v2 = R"(
define i32 @Foo() {
entry:
  ret i32 2
}

define i32 @main() {
entry:
  %v = call i32 @Foo()
  ret i32 %v
}
)";
  if (!ExpectOk("v2-shadow", holyc::llvm_backend::ExecuteIrJit(ir_v2, kSession, false), 2)) {
    return 1;
  }

  // New module resolves Foo from the previous module in the same session.
  const std::string ir_v3 = R"(
declare i32 @Foo()

define i32 @main() {
entry:
  %v = call i32 @Foo()
  %sum = add i32 %v, 40
  ret i32 %sum
}
)";
  if (!ExpectOk("v3-cross-module-lookup",
                holyc::llvm_backend::ExecuteIrJit(ir_v3, kSession, false), 42)) {
    return 1;
  }

  // Runtime resolver: explicit ABI symbol registration should be available.
  const std::string ir_runtime = R"(
declare i64 @hc_runtime_abi_version()

define i32 @main() {
entry:
  %abi = call i64 @hc_runtime_abi_version()
  %major = lshr i64 %abi, 32
  %major32 = trunc i64 %major to i32
  ret i32 %major32
}
)";
  if (!ExpectOk("runtime-symbol", holyc::llvm_backend::ExecuteIrJit(ir_runtime, kSession, false),
                1)) {
    return 1;
  }

  // Host resolver policy should block arbitrary host symbols like puts.
  const std::string ir_forbidden_host_symbol = R"(
@msg = private unnamed_addr constant [2 x i8] c"X\00"
declare i32 @puts(ptr)

define i32 @main() {
entry:
  %p = getelementptr [2 x i8], ptr @msg, i32 0, i32 0
  %v = call i32 @puts(ptr %p)
  ret i32 %v
}
)";
  if (!ExpectFail("forbidden-host-symbol",
                  holyc::llvm_backend::ExecuteIrJit(ir_forbidden_host_symbol, kSession, false))) {
    return 1;
  }

  const Result reset_after = holyc::llvm_backend::ResetJitSession(kSession);
  if (!reset_after.ok) {
    std::cerr << "FAIL(reset-after): " << reset_after.output << "\n";
    return 1;
  }

  return 0;
}

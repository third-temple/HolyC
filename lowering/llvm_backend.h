#pragma once

#include <string>
#include <string_view>

namespace holyc::llvm_backend {

struct Result {
  bool ok;
  std::string output;
};

enum class OptLevel {
  kO0,
  kO1,
  kO2,
  kO3,
  kOs,
  kOz,
};

Result NormalizeIr(std::string_view ir_text);
Result BuildExecutableFromIr(std::string_view ir_text, std::string_view output_path,
                             std::string_view artifact_dir = "",
                             std::string_view target_triple = "",
                             OptLevel opt_level = OptLevel::kO2);
Result LoadIrJit(std::string_view ir_text, std::string_view session_name = "",
                 OptLevel opt_level = OptLevel::kO2);
Result ExecuteIrJit(std::string_view ir_text, std::string_view session_name = "",
                    bool reset_after_run = true,
                    std::string_view entry_symbol_name = "main",
                    OptLevel opt_level = OptLevel::kO2);
Result ResetJitSession(std::string_view session_name = "");

}  // namespace holyc::llvm_backend

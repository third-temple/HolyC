#pragma once

#include <string>
#include <string_view>

namespace holyc::llvm_backend {

struct Result {
  bool ok;
  std::string output;
};

Result NormalizeIr(std::string_view ir_text);
Result BuildExecutableFromIr(std::string_view ir_text, std::string_view output_path,
                             std::string_view artifact_dir = "",
                             std::string_view target_triple = "");
Result LoadIrJit(std::string_view ir_text, std::string_view session_name = "");
Result ExecuteIrJit(std::string_view ir_text, std::string_view session_name = "",
                    bool reset_after_run = true,
                    std::string_view entry_symbol_name = "main");
Result ResetJitSession(std::string_view session_name = "");

}  // namespace holyc::llvm_backend

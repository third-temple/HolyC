#pragma once

#include <string>
#include <string_view>

namespace holyc::frontend {

struct ParseResult {
  bool ok;
  std::string output;
};

enum class ExecutionMode {
  kJit,
  kAot,
};

ParseResult PreprocessSource(std::string_view source, std::string_view filename,
                             ExecutionMode mode = ExecutionMode::kJit,
                             bool strict_mode = true);
ParseResult ParseAndDumpAst(std::string_view source, std::string_view filename,
                            ExecutionMode mode = ExecutionMode::kJit,
                            bool strict_mode = true);
ParseResult CheckSource(std::string_view source, std::string_view filename,
                        ExecutionMode mode = ExecutionMode::kJit,
                        bool strict_mode = true);
ParseResult EmitHir(std::string_view source, std::string_view filename,
                    ExecutionMode mode = ExecutionMode::kJit,
                    bool strict_mode = true);
ParseResult EmitLlvmIr(std::string_view source, std::string_view filename,
                       ExecutionMode mode = ExecutionMode::kAot,
                       bool strict_mode = true);

}  // namespace holyc::frontend

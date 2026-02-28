#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace holyc::frontend {

struct ParseResult {
  bool ok;
  std::string output;
};

struct PhaseTiming {
  std::string name;
  double seconds = 0.0;
};

enum class ExecutionMode {
  kJit,
  kAot,
};

ParseResult PreprocessSource(std::string_view source, std::string_view filename,
                             ExecutionMode mode = ExecutionMode::kJit,
                             bool strict_mode = true,
                             std::vector<PhaseTiming>* phase_timings = nullptr);
ParseResult ParseAndDumpAst(std::string_view source, std::string_view filename,
                            ExecutionMode mode = ExecutionMode::kJit,
                            bool strict_mode = true,
                            std::vector<PhaseTiming>* phase_timings = nullptr);
ParseResult CheckSource(std::string_view source, std::string_view filename,
                        ExecutionMode mode = ExecutionMode::kJit,
                        bool strict_mode = true,
                        std::vector<PhaseTiming>* phase_timings = nullptr);
ParseResult EmitHir(std::string_view source, std::string_view filename,
                    ExecutionMode mode = ExecutionMode::kJit,
                    bool strict_mode = true,
                    std::vector<PhaseTiming>* phase_timings = nullptr);
ParseResult EmitLlvmIr(std::string_view source, std::string_view filename,
                       ExecutionMode mode = ExecutionMode::kAot,
                       bool strict_mode = true,
                       std::vector<PhaseTiming>* phase_timings = nullptr);

}  // namespace holyc::frontend

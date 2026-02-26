#pragma once

#include <string>

namespace holyc::frontend::internal {

enum class DiagnosticSeverity {
  kError,
  kWarning,
  kNote,
};

struct Diagnostic {
  std::string code;
  DiagnosticSeverity severity = DiagnosticSeverity::kError;
  std::string file;
  int line = 0;
  int column = 0;
  std::string message;
  std::string remediation;
};

std::string FormatDiagnostic(const Diagnostic& diag);
[[noreturn]] void ThrowDiagnostic(const Diagnostic& diag);

}  // namespace holyc::frontend::internal

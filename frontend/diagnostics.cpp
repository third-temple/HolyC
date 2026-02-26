#include "diagnostics.h"

#include <sstream>
#include <stdexcept>
#include <string>

namespace holyc::frontend::internal {

std::string FormatDiagnostic(const Diagnostic& diag) {
  std::ostringstream oss;
  const char* sev = "error";
  if (diag.severity == DiagnosticSeverity::kWarning) {
    sev = "warning";
  } else if (diag.severity == DiagnosticSeverity::kNote) {
    sev = "note";
  }

  oss << sev;
  if (!diag.code.empty()) {
    oss << "[" << diag.code << "]";
  }
  oss << ": ";

  if (!diag.file.empty()) {
    oss << diag.file;
    if (diag.line > 0) {
      oss << ":" << diag.line;
      if (diag.column > 0) {
        oss << ":" << diag.column;
      }
    }
    oss << ": ";
  }

  oss << diag.message;
  if (!diag.remediation.empty()) {
    oss << "\nhelp: " << diag.remediation;
  }
  return oss.str();
}

[[noreturn]] void ThrowDiagnostic(const Diagnostic& diag) {
  throw std::runtime_error(FormatDiagnostic(diag));
}

}  // namespace holyc::frontend::internal

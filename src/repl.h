#pragma once

#include <string_view>

#include "llvm_backend.h"

namespace holyc::repl {

int RunRepl(bool strict_mode, std::string_view jit_session, bool jit_reset,
            llvm_backend::OptLevel opt_level);

}  // namespace holyc::repl

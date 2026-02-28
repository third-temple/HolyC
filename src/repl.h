#pragma once

#include <string_view>

namespace holyc::repl {

int RunRepl(bool strict_mode, std::string_view jit_session, bool jit_reset);

}  // namespace holyc::repl

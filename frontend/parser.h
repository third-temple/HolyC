#pragma once

#include <string_view>

#include "ast_internal.h"

namespace holyc::frontend::internal {

ParsedNode ParseAst(std::string_view source, std::string_view filename);

}  // namespace holyc::frontend::internal

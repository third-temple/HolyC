#pragma once

#include <string_view>

#include "ast_internal.h"

namespace holyc::frontend::internal {

TypedNode AnalyzeSemantics(const ParsedNode& program, std::string_view filename);

}  // namespace holyc::frontend::internal

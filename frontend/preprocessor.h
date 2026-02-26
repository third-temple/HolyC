#pragma once

#include <string>
#include <string_view>

#include "frontend.h"

namespace holyc::frontend::internal {

std::string RunPreprocessor(std::string_view source, std::string_view filename,
                            ExecutionMode mode);

}  // namespace holyc::frontend::internal

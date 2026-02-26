#pragma once

#include <string_view>

#include "hir.h"
#include "llvm_backend.h"

namespace holyc::llvm_irbuilder_backend {

llvm_backend::Result EmitIrFromHir(const frontend::internal::HIRModule& module,
                                   std::string_view module_name = "holyc",
                                   std::string_view target_triple = "");

}  // namespace holyc::llvm_irbuilder_backend

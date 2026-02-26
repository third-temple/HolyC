#include "llvm_irbuilder_backend.h"
#include "hc_runtime.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(HOLYC_HAS_LLVM) && __has_include(<llvm/IR/IRBuilder.h>)
#define HOLYC_LLVM_IRBUILDER_HEADERS_AVAILABLE 1
#endif

#ifdef HOLYC_LLVM_IRBUILDER_HEADERS_AVAILABLE
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
#endif

namespace holyc::llvm_irbuilder_backend {

namespace {

#ifdef HOLYC_LLVM_IRBUILDER_HEADERS_AVAILABLE

using frontend::internal::HIRExpr;
using frontend::internal::HIRFunction;
using frontend::internal::HIRFunctionDecl;
using frontend::internal::HIRModule;
using frontend::internal::HIRReflectionField;
using frontend::internal::HIRReflectionTable;
using frontend::internal::HIRStmt;

std::string TrimCopy(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

std::pair<std::string, std::string> ParseTypedName(std::string_view text) {
  const std::string input = TrimCopy(text);
  if (input.empty()) {
    return {"", ""};
  }

  std::smatch match;
  static const std::regex kFunctionPointerDecl(
      R"(^(.*)\(\s*[*&]\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*\(.*\)\s*$)");
  if (std::regex_match(input, match, kFunctionPointerDecl)) {
    return {TrimCopy(match[1].str()), match[2].str()};
  }

  static const std::regex kTrailingDeclarator(
      R"(^(.*?)([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\]\s*)*$)");
  if (std::regex_match(input, match, kTrailingDeclarator)) {
    return {TrimCopy(match[1].str()), match[2].str()};
  }

  static const std::regex kAnyIdentifier(R"(([A-Za-z_][A-Za-z0-9_]*))");
  std::string last_identifier;
  for (std::sregex_iterator it(input.begin(), input.end(), kAnyIdentifier), end_it;
       it != end_it; ++it) {
    last_identifier = (*it)[1].str();
  }
  if (last_identifier.empty()) {
    return {"", ""};
  }

  const std::size_t pos = input.rfind(last_identifier);
  if (pos == std::string::npos) {
    return {"", last_identifier};
  }
  return {TrimCopy(input.substr(0, pos)), last_identifier};
}

std::string NormalizeAggregateTypeName(std::string type_name) {
  type_name = TrimCopy(type_name);
  while (!type_name.empty() && type_name.back() == '*') {
    type_name.pop_back();
    type_name = TrimCopy(type_name);
  }
  if (type_name.rfind("class ", 0) == 0) {
    type_name = TrimCopy(type_name.substr(6));
  } else if (type_name.rfind("union ", 0) == 0) {
    type_name = TrimCopy(type_name.substr(6));
  }
  return type_name;
}

std::string JoinTokens(const std::vector<std::string>& tokens, std::string_view sep) {
  if (tokens.empty()) {
    return "";
  }
  std::string out;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (i > 0) {
      out.append(sep);
    }
    out.append(tokens[i]);
  }
  return out;
}

bool ParseIntegerLiteralText(std::string_view text, std::int64_t* value_out) {
  if (value_out == nullptr) {
    return false;
  }
  const std::string literal = TrimCopy(text);
  if (literal.empty()) {
    return false;
  }

  errno = 0;
  char* end = nullptr;
  const long long parsed_signed = std::strtoll(literal.c_str(), &end, 0);
  if (errno == 0 && end != literal.c_str() && *end == '\0') {
    *value_out = static_cast<std::int64_t>(parsed_signed);
    return true;
  }

  errno = 0;
  end = nullptr;
  const unsigned long long parsed_unsigned = std::strtoull(literal.c_str(), &end, 0);
  if (errno == 0 && end != literal.c_str() && *end == '\0') {
    *value_out = static_cast<std::int64_t>(parsed_unsigned);
    return true;
  }

  return false;
}

std::string StmtKindName(HIRStmt::Kind kind) {
  switch (kind) {
    case HIRStmt::Kind::kVarDecl:
      return "VarDecl";
    case HIRStmt::Kind::kAssign:
      return "Assign";
    case HIRStmt::Kind::kReturn:
      return "Return";
    case HIRStmt::Kind::kExpr:
      return "Expr";
    case HIRStmt::Kind::kNoParenCall:
      return "NoParenCall";
    case HIRStmt::Kind::kPrint:
      return "Print";
    case HIRStmt::Kind::kLock:
      return "Lock";
    case HIRStmt::Kind::kThrow:
      return "Throw";
    case HIRStmt::Kind::kTryCatch:
      return "TryCatch";
    case HIRStmt::Kind::kBreak:
      return "Break";
    case HIRStmt::Kind::kSwitch:
      return "Switch";
    case HIRStmt::Kind::kIf:
      return "If";
    case HIRStmt::Kind::kWhile:
      return "While";
    case HIRStmt::Kind::kDoWhile:
      return "DoWhile";
    case HIRStmt::Kind::kLabel:
      return "Label";
    case HIRStmt::Kind::kGoto:
      return "Goto";
    case HIRStmt::Kind::kInlineAsm:
      return "InlineAsm";
    case HIRStmt::Kind::kMetadataDecl:
      return "MetadataDecl";
    case HIRStmt::Kind::kLinkageDecl:
      return "LinkageDecl";
  }
  return "Unknown";
}

std::string ExprKindName(HIRExpr::Kind kind) {
  switch (kind) {
    case HIRExpr::Kind::kIntLiteral:
      return "int-literal";
    case HIRExpr::Kind::kStringLiteral:
      return "string-literal";
    case HIRExpr::Kind::kDollar:
      return "dollar";
    case HIRExpr::Kind::kVar:
      return "var";
    case HIRExpr::Kind::kAssign:
      return "assign";
    case HIRExpr::Kind::kUnary:
      return "unary";
    case HIRExpr::Kind::kBinary:
      return "binary";
    case HIRExpr::Kind::kCall:
      return "call";
    case HIRExpr::Kind::kCast:
      return "cast";
    case HIRExpr::Kind::kPostfix:
      return "postfix";
    case HIRExpr::Kind::kLane:
      return "lane";
    case HIRExpr::Kind::kMember:
      return "member";
    case HIRExpr::Kind::kIndex:
      return "index";
    case HIRExpr::Kind::kComma:
      return "comma";
  }
  return "unknown";
}

bool HasExpr(const HIRExpr& expr) {
  return !expr.text.empty() || !expr.children.empty();
}

class IrBuilderEmitter {
 public:
  IrBuilderEmitter(std::string_view module_name, std::string_view target_triple)
      : context_(std::make_unique<llvm::LLVMContext>()),
        module_(std::make_unique<llvm::Module>(std::string(module_name), *context_)),
        builder_(*context_) {
    const std::string triple = target_triple.empty() ? llvm::sys::getDefaultTargetTriple()
                                                     : std::string(target_triple);
    module_->setTargetTriple(llvm::Triple(triple));
  }

  llvm_backend::Result Emit(const HIRModule& hir_module) {
    const llvm_backend::Result layouts_result = BuildAggregateLayouts(hir_module);
    if (!layouts_result.ok) {
      return layouts_result;
    }

    const llvm_backend::Result globals_result = EmitTopLevelItems(hir_module.top_level_items);
    if (!globals_result.ok) {
      return globals_result;
    }

    const llvm_backend::Result reflection_result = EmitReflectionTable(hir_module.reflection);
    if (!reflection_result.ok) {
      return reflection_result;
    }

    for (const HIRFunctionDecl& fn : hir_module.function_decls) {
      if (!DeclareFunction(fn.name, fn.return_type, fn.params, fn.linkage_kind)) {
        return {false, "irbuilder emit: function redeclaration conflict: " + fn.name};
      }
    }

    for (const HIRFunction& fn : hir_module.functions) {
      const auto it = functions_.find(fn.name);
      if (it == functions_.end()) {
        return {false, "irbuilder emit: missing declared function: " + fn.name};
      }
      it->second->setLinkage(ToFunctionLinkage(fn.linkage_kind));
      const llvm_backend::Result build = BuildFunction(fn, it->second);
      if (!build.ok) {
        return build;
      }
    }

    const llvm_backend::Result wrapper = EmitHostMainWrapper();
    if (!wrapper.ok) {
      return wrapper;
    }

    std::string verify_err;
    llvm::raw_string_ostream verify_os(verify_err);
    if (llvm::verifyModule(*module_, &verify_os)) {
      verify_os.flush();
      return {false, verify_err};
    }

    std::string out;
    llvm::raw_string_ostream os(out);
    module_->print(os, nullptr);
    os.flush();
    return {true, out};
  }

 private:
  struct FunctionFrame {
    llvm::Function* function = nullptr;
    std::unordered_map<std::string, llvm::AllocaInst*> locals;
    std::unordered_map<std::string, llvm::BasicBlock*> label_blocks;
    std::vector<llvm::BasicBlock*> break_targets;
  };

  struct ExprResult {
    bool ok = false;
    llvm::Value* value = nullptr;
    std::string message;
  };

  struct BinaryResult {
    bool ok = false;
    llvm::Value* value = nullptr;
    std::string message;
  };

  struct ConstIntResult {
    bool ok = false;
    std::int64_t value = 0;
    std::string message;
  };

  struct ConstValueResult {
    bool ok = false;
    llvm::Constant* value = nullptr;
    std::string message;
  };

  struct LValueResult {
    bool ok = false;
    llvm::Value* ptr = nullptr;
    llvm::Type* pointee_type = nullptr;
    std::string message;
  };

  struct AggregateMemberLayout {
    unsigned index = 0;
    llvm::Type* type = nullptr;
  };

  struct AggregateLayout {
    llvm::StructType* type = nullptr;
    bool is_union = false;
    std::unordered_map<std::string, AggregateMemberLayout> members;
  };

  struct LaneInfo {
    bool valid = false;
    unsigned bits = 0;
    bool is_signed = false;
  };

  struct PrintFormatSpec {
    char conv = '\0';
    bool width_from_arg = false;
    bool precision_from_arg = false;
  };

  static constexpr std::size_t kTryFrameStorageSize = sizeof(hc_try_frame);
  static constexpr unsigned kTryFrameStorageAlignment =
      static_cast<unsigned>(alignof(hc_try_frame));

  llvm_backend::Result EmitTopLevelItems(const std::vector<HIRStmt>& items) {
    for (const HIRStmt& item : items) {
      if (item.kind == HIRStmt::Kind::kVarDecl) {
        const llvm_backend::Result global_result = EmitGlobalVariable(item);
        if (!global_result.ok) {
          return global_result;
        }
        continue;
      }

      if (item.kind == HIRStmt::Kind::kLinkageDecl) {
        const llvm_backend::Result linkage_result = EmitLinkageDecl(item);
        if (!linkage_result.ok) {
          return linkage_result;
        }
        continue;
      }

      if (item.kind == HIRStmt::Kind::kMetadataDecl) {
        // Reflection/metadata declarations are represented separately in
        // HIRReflectionTable and emitted via EmitReflectionTable.
        continue;
      }

      return {false, "irbuilder emit: unsupported top-level statement kind: " +
                         StmtKindName(item.kind)};
    }
    return {true, ""};
  }

  static std::size_t ApproxTypeSize(llvm::Type* ty) {
    if (ty == nullptr) {
      return 0;
    }
    if (ty->isIntegerTy()) {
      return std::max<std::size_t>(1, ty->getIntegerBitWidth() / 8);
    }
    if (ty->isPointerTy()) {
      return 8;
    }
    if (auto* arr = llvm::dyn_cast<llvm::ArrayType>(ty)) {
      return static_cast<std::size_t>(arr->getNumElements()) * ApproxTypeSize(arr->getElementType());
    }
    if (auto* st = llvm::dyn_cast<llvm::StructType>(ty)) {
      if (st->isOpaque()) {
        return 8;
      }
      std::size_t total = 0;
      for (llvm::Type* element : st->elements()) {
        total += ApproxTypeSize(element);
      }
      return std::max<std::size_t>(1, total);
    }
    return 8;
  }

  llvm_backend::Result BuildAggregateLayouts(const HIRModule& hir_module) {
    aggregate_layouts_.clear();

    std::unordered_map<std::string, std::vector<HIRReflectionField>> fields_by_aggregate;
    fields_by_aggregate.reserve(hir_module.reflection.fields.size());
    for (const HIRReflectionField& field : hir_module.reflection.fields) {
      fields_by_aggregate[field.aggregate_name].push_back(field);
    }

    std::unordered_set<std::string> union_aggregates;
    for (const HIRStmt& item : hir_module.top_level_items) {
      if (item.kind != HIRStmt::Kind::kMetadataDecl) {
        continue;
      }
      if (item.metadata_name.rfind("union ", 0) == 0) {
        const std::string union_name = NormalizeAggregateTypeName(item.metadata_name);
        if (!union_name.empty()) {
          union_aggregates.insert(union_name);
        }
      }
    }

    for (const auto& [aggregate_name, _] : fields_by_aggregate) {
      AggregateLayout layout;
      layout.type = llvm::StructType::create(*context_, "hc." + aggregate_name);
      layout.is_union = union_aggregates.find(aggregate_name) != union_aggregates.end();
      aggregate_layouts_[aggregate_name] = std::move(layout);
    }

    for (const auto& [aggregate_name, fields] : fields_by_aggregate) {
      auto layout_it = aggregate_layouts_.find(aggregate_name);
      if (layout_it == aggregate_layouts_.end() || layout_it->second.type == nullptr) {
        return {false, "irbuilder emit: missing aggregate layout for " + aggregate_name};
      }

      AggregateLayout& layout = layout_it->second;
      if (fields.empty()) {
        layout.type->setBody({llvm::Type::getInt8Ty(*context_)}, false);
        continue;
      }

      if (layout.is_union) {
        llvm::Type* storage_type = ToLlvmType(fields.front().field_type);
        std::size_t storage_size = ApproxTypeSize(storage_type);
        for (const HIRReflectionField& field : fields) {
          llvm::Type* field_ty = ToLlvmType(field.field_type);
          const std::size_t field_size = ApproxTypeSize(field_ty);
          if (field_size > storage_size) {
            storage_type = field_ty;
            storage_size = field_size;
          }
          layout.members[field.field_name] = AggregateMemberLayout{0, field_ty};
        }
        layout.type->setBody({storage_type}, false);
        continue;
      }

      std::vector<llvm::Type*> llvm_fields;
      llvm_fields.reserve(fields.size());
      for (std::size_t i = 0; i < fields.size(); ++i) {
        llvm::Type* field_ty = ToLlvmType(fields[i].field_type);
        llvm_fields.push_back(field_ty);
        layout.members[fields[i].field_name] =
            AggregateMemberLayout{static_cast<unsigned>(i), field_ty};
      }
      layout.type->setBody(llvm_fields, false);
    }

    return {true, ""};
  }

  llvm_backend::Result EmitGlobalVariable(const HIRStmt& st) {
    llvm::Type* ty = ToLlvmType(st.type);
    if (!ty->isIntegerTy() && !ty->isPointerTy() && !ty->isStructTy() &&
        !ty->isArrayTy()) {
      return {false, "irbuilder emit: unsupported global type for " + st.name};
    }

    llvm::Constant* initializer = llvm::Constant::getNullValue(ty);
    if (HasExpr(st.expr)) {
      const ConstValueResult constant = EvalGlobalConstExpr(st.expr, ty);
      if (!constant.ok || constant.value == nullptr) {
        return {false, "irbuilder emit: global initializer for " + st.name + " is not constant: " +
                           constant.message};
      }
      initializer = constant.value;
    }

    const bool is_static = st.decl_storage == "static-global";
    llvm::GlobalValue::LinkageTypes linkage =
        is_static ? llvm::GlobalValue::InternalLinkage : llvm::GlobalValue::ExternalLinkage;
    llvm::GlobalVariable* global = module_->getGlobalVariable(st.name, true);
    if (global != nullptr) {
      if (global->getValueType() != ty) {
        return {false, "irbuilder emit: conflicting global declaration type for " + st.name};
      }
      if (!global->isDeclaration()) {
        return {false, "irbuilder emit: duplicate global definition: " + st.name};
      }
      if (is_static && global->getLinkage() != llvm::GlobalValue::InternalLinkage) {
        return {false, "irbuilder emit: conflicting global linkage for " + st.name};
      }
      global->setInitializer(initializer);
      global->setLinkage(linkage);
    } else {
      global = new llvm::GlobalVariable(*module_, ty, false, linkage, initializer, st.name);
    }
    globals_[st.name] = global;
    global_constants_[st.name] = initializer;
    return {true, ""};
  }

  llvm_backend::Result EmitLinkageDecl(const HIRStmt& st) {
    if (st.linkage_kind != "extern" && st.linkage_kind != "import" &&
        st.linkage_kind != "_extern" && st.linkage_kind != "_import" &&
        st.linkage_kind != "export" && st.linkage_kind != "_export") {
      return {false, "irbuilder emit: unsupported linkage directive: " + st.linkage_kind};
    }

    if (st.linkage_symbol.empty()) {
      return {false, "irbuilder emit: linkage declaration missing symbol payload"};
    }

    const auto [decl_ty, decl_name] = ParseTypedName(st.linkage_symbol);
    if (decl_name.empty()) {
      return {false, "irbuilder emit: invalid linkage declaration payload: " + st.linkage_symbol};
    }

    llvm::Type* ty = ToLlvmType(decl_ty.empty() ? "I64" : decl_ty);
    if (!ty->isIntegerTy() && !ty->isPointerTy()) {
      return {false, "irbuilder emit: unsupported linkage type for " + decl_name};
    }

    llvm::GlobalVariable* global = module_->getGlobalVariable(decl_name, true);
    if (global == nullptr) {
      global = new llvm::GlobalVariable(*module_, ty, false, llvm::GlobalValue::ExternalLinkage,
                                        nullptr, decl_name);
    } else if (global->getValueType() != ty) {
      return {false, "irbuilder emit: conflicting linkage declaration type for " + decl_name};
    }
    globals_[decl_name] = global;
    return {true, ""};
  }

  llvm_backend::Result EmitReflectionTable(const HIRReflectionTable& table) {
    reflection_table_ptr_ = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(TypePtr()));
    reflection_table_count_ = 0;

    if (table.fields.empty()) {
      return {true, ""};
    }

    llvm::StructType* field_ty =
        llvm::StructType::get(TypePtr(), TypePtr(), TypePtr(), TypePtr());
    std::vector<llvm::Constant*> rows;
    rows.reserve(table.fields.size());

    for (const HIRReflectionField& field : table.fields) {
      llvm::Constant* aggregate_name =
          llvm::cast<llvm::Constant>(GetOrCreateStringLiteral(field.aggregate_name));
      llvm::Constant* field_name =
          llvm::cast<llvm::Constant>(GetOrCreateStringLiteral(field.field_name));
      llvm::Constant* field_type =
          llvm::cast<llvm::Constant>(GetOrCreateStringLiteral(field.field_type));
      llvm::Constant* annotations =
          llvm::cast<llvm::Constant>(GetOrCreateStringLiteral(JoinTokens(field.annotations, " ")));
      rows.push_back(
          llvm::ConstantStruct::get(field_ty, {aggregate_name, field_name, field_type, annotations}));
    }

    llvm::ArrayType* table_ty = llvm::ArrayType::get(field_ty, rows.size());
    llvm::Constant* table_init = llvm::ConstantArray::get(table_ty, rows);
    auto* table_global = new llvm::GlobalVariable(
        *module_, table_ty, true, llvm::GlobalValue::PrivateLinkage, table_init, ".hc.reflection");
    table_global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

    llvm::Constant* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
    llvm::Constant* indices[] = {zero, zero};
    reflection_table_ptr_ =
        llvm::ConstantExpr::getGetElementPtr(table_ty, table_global, indices);
    reflection_table_count_ = static_cast<std::uint64_t>(rows.size());
    return {true, ""};
  }

  static llvm::Function::LinkageTypes ToFunctionLinkage(std::string_view linkage_kind) {
    if (linkage_kind == "internal") {
      return llvm::Function::InternalLinkage;
    }
    return llvm::Function::ExternalLinkage;
  }

  bool DeclareFunction(std::string_view name, std::string_view return_type,
                       const std::vector<std::pair<std::string, std::string>>& params,
                       std::string_view linkage_kind) {
    std::vector<llvm::Type*> arg_types;
    arg_types.reserve(params.size());
    for (const auto& param : params) {
      arg_types.push_back(ToLlvmType(param.first));
    }

    const std::string name_str(name);
    llvm::Type* ret_ty = ToLlvmType(std::string(return_type));
    llvm::FunctionType* fnty = llvm::FunctionType::get(ret_ty, arg_types, false);
    const llvm::Function::LinkageTypes llvm_linkage = ToFunctionLinkage(linkage_kind);
    if (const auto it = functions_.find(name_str); it != functions_.end()) {
      llvm::Function* existing = it->second;
      if (existing->getFunctionType() != fnty) {
        return false;
      }
      if (existing->getLinkage() != llvm_linkage &&
          (existing->hasLocalLinkage() || llvm_linkage == llvm::Function::InternalLinkage)) {
        return false;
      }
      existing->setLinkage(llvm_linkage);
      return true;
    }

    llvm::Function* fn_value = module_->getFunction(name_str);
    if (fn_value != nullptr) {
      if (fn_value->getFunctionType() != fnty) {
        return false;
      }
      fn_value->setLinkage(llvm_linkage);
    } else {
      fn_value = llvm::Function::Create(fnty, llvm_linkage, name_str, module_.get());
    }

    std::size_t i = 0;
    for (llvm::Argument& arg : fn_value->args()) {
      arg.setName(params[i].second);
      ++i;
    }

    functions_[name_str] = fn_value;
    return true;
  }

  llvm_backend::Result BuildFunction(const HIRFunction& fn, llvm::Function* fn_value) {
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context_, "entry", fn_value);
    builder_.SetInsertPoint(entry);

    FunctionFrame frame;
    frame.function = fn_value;

    for (llvm::Argument& arg : fn_value->args()) {
      llvm::AllocaInst* slot =
          CreateEntryAlloca(fn_value, std::string(arg.getName()), arg.getType());
      builder_.CreateStore(&arg, slot);
      frame.locals[std::string(arg.getName())] = slot;
    }

    const llvm_backend::Result body_result = EmitStmtList(fn.body, &frame);
    if (!body_result.ok) {
      return body_result;
    }

    if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
      if (fn_value->getReturnType()->isVoidTy()) {
        builder_.CreateRetVoid();
      } else if (fn_value->getReturnType()->isIntegerTy()) {
        builder_.CreateRet(llvm::ConstantInt::get(fn_value->getReturnType(), 0));
      } else if (fn_value->getReturnType()->isPointerTy()) {
        builder_.CreateRet(
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(fn_value->getReturnType())));
      } else {
        builder_.CreateRet(llvm::UndefValue::get(fn_value->getReturnType()));
      }
    }

    return {true, ""};
  }

  llvm_backend::Result EmitStmtList(const std::vector<HIRStmt>& stmts, FunctionFrame* frame) {
    for (const HIRStmt& st : stmts) {
      if (builder_.GetInsertBlock()->getTerminator() != nullptr &&
          st.kind != HIRStmt::Kind::kLabel) {
        continue;
      }

      const llvm_backend::Result result = EmitStmt(st, frame);
      if (!result.ok) {
        return result;
      }
    }
    return {true, ""};
  }

  llvm_backend::Result EmitStmt(const HIRStmt& st, FunctionFrame* frame) {
    switch (st.kind) {
      case HIRStmt::Kind::kVarDecl: {
        llvm::Type* ty = ToLlvmType(st.type);
        llvm::AllocaInst* slot = CreateEntryAlloca(frame->function, st.name, ty);
        frame->locals[st.name] = slot;
        if (HasExpr(st.expr)) {
          const ExprResult value = EmitExpr(st.expr, frame);
          if (!value.ok) {
            return {false, value.message};
          }
          llvm::Value* casted = CastIfNeeded(value.value, ty);
          if (casted == nullptr) {
            return {false, "irbuilder emit: variable initializer type mismatch for " + st.name};
          }
          builder_.CreateStore(casted, slot);
        }
        return {true, ""};
      }

      case HIRStmt::Kind::kAssign: {
        llvm::Value* ptr = nullptr;
        llvm::Type* dst_ty = nullptr;

        const auto local_it = frame->locals.find(st.name);
        if (local_it != frame->locals.end()) {
          ptr = local_it->second;
          dst_ty = local_it->second->getAllocatedType();
        } else {
          const auto global_it = globals_.find(st.name);
          if (global_it != globals_.end()) {
            ptr = global_it->second;
            dst_ty = global_it->second->getValueType();
          }
        }

        if (ptr == nullptr || dst_ty == nullptr) {
          return {false, "irbuilder emit: assignment to unknown variable " + st.name};
        }

        const ExprResult rhs = EmitExpr(st.expr, frame);
        if (!rhs.ok) {
          return {false, rhs.message};
        }

        llvm::Value* to_store = rhs.value;
        if (st.assign_op != "=") {
          llvm::Value* current = builder_.CreateLoad(dst_ty, ptr);
          const BinaryResult combined =
              EmitBinaryOp(AssignOpToBinary(st.assign_op), current, rhs.value);
          if (!combined.ok) {
            return {false, combined.message};
          }
          to_store = combined.value;
        }

        llvm::Value* casted = CastIfNeeded(to_store, dst_ty);
        if (casted == nullptr) {
          return {false, "irbuilder emit: assignment type mismatch for " + st.name};
        }
        builder_.CreateStore(casted, ptr);
        return {true, ""};
      }

      case HIRStmt::Kind::kExpr: {
        const ExprResult value = EmitExpr(st.expr, frame);
        if (!value.ok) {
          return {false, value.message};
        }
        (void)value;
        return {true, ""};
      }

      case HIRStmt::Kind::kNoParenCall: {
        llvm::Function* callee = module_->getFunction(st.name);
        if (callee == nullptr) {
          return {false, "irbuilder emit: unknown function " + st.name};
        }
        if (callee->arg_size() != 0) {
          return {false, "irbuilder emit: no-paren call requires zero-arg callee: " + st.name};
        }
        builder_.CreateCall(callee, {});
        return {true, ""};
      }

      case HIRStmt::Kind::kPrint:
        return EmitPrint(st, frame);

      case HIRStmt::Kind::kLock:
        return EmitLockStmt(st, frame);

      case HIRStmt::Kind::kIf:
        return EmitIfStmt(st, frame);

      case HIRStmt::Kind::kWhile:
        return EmitWhileStmt(st, frame);

      case HIRStmt::Kind::kDoWhile:
        return EmitDoWhileStmt(st, frame);

      case HIRStmt::Kind::kSwitch:
        return EmitSwitchStmt(st, frame);

      case HIRStmt::Kind::kBreak: {
        if (frame->break_targets.empty()) {
          return {false, "irbuilder emit: break used outside switch/loop"};
        }
        builder_.CreateBr(frame->break_targets.back());
        return {true, ""};
      }

      case HIRStmt::Kind::kReturn: {
        llvm::Type* ret_ty = frame->function->getReturnType();
        if (ret_ty->isVoidTy()) {
          builder_.CreateRetVoid();
          return {true, ""};
        }

        llvm::Value* ret_value = ret_ty->isIntegerTy()
                                     ? llvm::ConstantInt::get(ret_ty, 0)
                                     : static_cast<llvm::Value*>(
                                           llvm::UndefValue::get(ret_ty));
        if (HasExpr(st.expr)) {
          const ExprResult value = EmitExpr(st.expr, frame);
          if (!value.ok) {
            return {false, value.message};
          }
          ret_value = value.value;
        }

        llvm::Value* casted = CastIfNeeded(ret_value, ret_ty);
        if (casted == nullptr) {
          return {false, "irbuilder emit: return type mismatch"};
        }
        builder_.CreateRet(casted);
        return {true, ""};
      }

      case HIRStmt::Kind::kThrow:
        return EmitThrowStmt(st, frame);

      case HIRStmt::Kind::kTryCatch:
        return EmitTryCatchStmt(st, frame);

      case HIRStmt::Kind::kLabel:
        return EmitLabelStmt(st, frame);

      case HIRStmt::Kind::kGoto:
        return EmitGotoStmt(st, frame);

      case HIRStmt::Kind::kInlineAsm:
        return EmitInlineAsmStmt(st, frame);

      case HIRStmt::Kind::kMetadataDecl:
      case HIRStmt::Kind::kLinkageDecl:
        return {false, "irbuilder emit: unsupported statement kind in primary backend: " +
                           StmtKindName(st.kind)};
    }

    return {false, "irbuilder emit: invalid statement kind"};
  }

  llvm_backend::Result EmitIfStmt(const HIRStmt& st, FunctionFrame* frame) {
    const ExprResult cond_value = EmitExpr(st.flow_cond, frame);
    if (!cond_value.ok) {
      return {false, cond_value.message};
    }

    llvm::Value* cond = ToBool(cond_value.value);
    if (cond == nullptr) {
      return {false, "irbuilder emit: if condition is not bool-convertible"};
    }

    llvm::Function* fn = frame->function;
    llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(*context_, "if.then", fn);
    llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(*context_, "if.else", fn);
    llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(*context_, "if.end", fn);

    builder_.CreateCondBr(cond, then_bb, else_bb);

    builder_.SetInsertPoint(then_bb);
    const llvm_backend::Result then_result = EmitStmtList(st.flow_then, frame);
    if (!then_result.ok) {
      return then_result;
    }
    if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
      builder_.CreateBr(end_bb);
    }

    builder_.SetInsertPoint(else_bb);
    const llvm_backend::Result else_result = EmitStmtList(st.flow_else, frame);
    if (!else_result.ok) {
      return else_result;
    }
    if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
      builder_.CreateBr(end_bb);
    }

    builder_.SetInsertPoint(end_bb);
    return {true, ""};
  }

  llvm_backend::Result EmitWhileStmt(const HIRStmt& st, FunctionFrame* frame) {
    llvm::Function* fn = frame->function;
    llvm::BasicBlock* cond_bb = llvm::BasicBlock::Create(*context_, "while.cond", fn);
    llvm::BasicBlock* body_bb = llvm::BasicBlock::Create(*context_, "while.body", fn);
    llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(*context_, "while.end", fn);

    builder_.CreateBr(cond_bb);

    builder_.SetInsertPoint(cond_bb);
    const ExprResult cond_value = EmitExpr(st.flow_cond, frame);
    if (!cond_value.ok) {
      return {false, cond_value.message};
    }
    llvm::Value* cond = ToBool(cond_value.value);
    if (cond == nullptr) {
      return {false, "irbuilder emit: while condition is not bool-convertible"};
    }
    builder_.CreateCondBr(cond, body_bb, end_bb);

    builder_.SetInsertPoint(body_bb);
    frame->break_targets.push_back(end_bb);
    const llvm_backend::Result body_result = EmitStmtList(st.flow_then, frame);
    frame->break_targets.pop_back();
    if (!body_result.ok) {
      return body_result;
    }
    if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
      builder_.CreateBr(cond_bb);
    }

    builder_.SetInsertPoint(end_bb);
    return {true, ""};
  }

  llvm_backend::Result EmitDoWhileStmt(const HIRStmt& st, FunctionFrame* frame) {
    llvm::Function* fn = frame->function;
    llvm::BasicBlock* body_bb = llvm::BasicBlock::Create(*context_, "do.body", fn);
    llvm::BasicBlock* cond_bb = llvm::BasicBlock::Create(*context_, "do.cond", fn);
    llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(*context_, "do.end", fn);

    builder_.CreateBr(body_bb);

    builder_.SetInsertPoint(body_bb);
    frame->break_targets.push_back(end_bb);
    const llvm_backend::Result body_result = EmitStmtList(st.flow_then, frame);
    frame->break_targets.pop_back();
    if (!body_result.ok) {
      return body_result;
    }
    if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
      builder_.CreateBr(cond_bb);
    }

    builder_.SetInsertPoint(cond_bb);
    const ExprResult cond_value = EmitExpr(st.flow_cond, frame);
    if (!cond_value.ok) {
      return {false, cond_value.message};
    }
    llvm::Value* cond = ToBool(cond_value.value);
    if (cond == nullptr) {
      return {false, "irbuilder emit: do-while condition is not bool-convertible"};
    }
    builder_.CreateCondBr(cond, body_bb, end_bb);

    builder_.SetInsertPoint(end_bb);
    return {true, ""};
  }

  llvm_backend::Result EmitSwitchStmt(const HIRStmt& st, FunctionFrame* frame) {
    const ExprResult cond_result = EmitExpr(st.switch_cond, frame);
    if (!cond_result.ok) {
      return {false, cond_result.message};
    }

    llvm::Value* cond_value = CoerceInt64(cond_result.value);
    if (cond_value == nullptr || !cond_value->getType()->isIntegerTy(64)) {
      return {false, "irbuilder emit: switch condition must be integer-convertible"};
    }

    llvm::Function* fn = frame->function;
    llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(*context_, "sw.end", fn);

    std::vector<llvm::BasicBlock*> case_bbs;
    case_bbs.reserve(st.switch_case_bodies.size());
    for (std::size_t i = 0; i < st.switch_case_bodies.size(); ++i) {
      case_bbs.push_back(llvm::BasicBlock::Create(*context_, "sw.case." + std::to_string(i), fn));
    }

    llvm::BasicBlock* default_bb = end_bb;
    if (!st.switch_default.empty()) {
      default_bb = llvm::BasicBlock::Create(*context_, "sw.default", fn);
    }

    if (case_bbs.empty()) {
      builder_.CreateBr(default_bb);
    } else {
      llvm::BasicBlock* current_test = llvm::BasicBlock::Create(*context_, "sw.test", fn);
      builder_.CreateBr(current_test);

      std::int64_t last_case_end = -1;
      for (std::size_t i = 0; i < case_bbs.size(); ++i) {
        builder_.SetInsertPoint(current_test);

        int flags = st.switch_case_flags[i];
        std::int64_t begin = st.switch_case_begin[i];
        std::int64_t end = st.switch_case_end[i];

        if ((flags & 1) != 0) {
          begin = last_case_end + 1;
          end = begin;
        }
        if ((flags & 2) == 0) {
          end = begin;
        }
        last_case_end = end;

        llvm::BasicBlock* false_target = default_bb;
        if (i + 1 < case_bbs.size()) {
          false_target =
              llvm::BasicBlock::Create(*context_, "sw.test." + std::to_string(i + 1), fn);
        }

        llvm::Value* match = nullptr;
        if (begin == end) {
          match = builder_.CreateICmpEQ(
              cond_value,
              llvm::ConstantInt::get(TypeI64(), static_cast<std::uint64_t>(begin), true));
        } else {
          llvm::Value* ge =
              builder_.CreateICmpSGE(
                  cond_value,
                  llvm::ConstantInt::get(TypeI64(), static_cast<std::uint64_t>(begin), true));
          llvm::Value* le =
              builder_.CreateICmpSLE(
                  cond_value,
                  llvm::ConstantInt::get(TypeI64(), static_cast<std::uint64_t>(end), true));
          match = builder_.CreateAnd(ge, le);
        }

        builder_.CreateCondBr(match, case_bbs[i], false_target);

        if (i + 1 < case_bbs.size()) {
          current_test = false_target;
        }
      }
    }

    if (!st.switch_default.empty()) {
      builder_.SetInsertPoint(default_bb);
      frame->break_targets.push_back(end_bb);
      const llvm_backend::Result default_result = EmitStmtList(st.switch_default, frame);
      frame->break_targets.pop_back();
      if (!default_result.ok) {
        return default_result;
      }
      if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
        builder_.CreateBr(end_bb);
      }
    }

    frame->break_targets.push_back(end_bb);
    for (std::size_t i = 0; i < case_bbs.size(); ++i) {
      builder_.SetInsertPoint(case_bbs[i]);
      const llvm_backend::Result case_result = EmitStmtList(st.switch_case_bodies[i], frame);
      if (!case_result.ok) {
        frame->break_targets.pop_back();
        return case_result;
      }

      if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
        if (i + 1 < case_bbs.size()) {
          builder_.CreateBr(case_bbs[i + 1]);
        } else if (!st.switch_default.empty()) {
          builder_.CreateBr(default_bb);
        } else {
          builder_.CreateBr(end_bb);
        }
      }
    }
    frame->break_targets.pop_back();

    builder_.SetInsertPoint(end_bb);
    return {true, ""};
  }

  llvm::BasicBlock* GetOrCreateLabelBlock(FunctionFrame* frame, const std::string& label) {
    const auto it = frame->label_blocks.find(label);
    if (it != frame->label_blocks.end()) {
      return it->second;
    }

    llvm::BasicBlock* created =
        llvm::BasicBlock::Create(*context_, "label." + label, frame->function);
    frame->label_blocks.emplace(label, created);
    return created;
  }

  llvm_backend::Result EmitLabelStmt(const HIRStmt& st, FunctionFrame* frame) {
    if (st.label_name.empty()) {
      return {false, "irbuilder emit: invalid empty label"};
    }
    llvm::BasicBlock* label_bb = GetOrCreateLabelBlock(frame, st.label_name);
    if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
      builder_.CreateBr(label_bb);
    }
    builder_.SetInsertPoint(label_bb);
    return {true, ""};
  }

  llvm_backend::Result EmitGotoStmt(const HIRStmt& st, FunctionFrame* frame) {
    if (st.goto_target.empty()) {
      return {false, "irbuilder emit: invalid goto target"};
    }
    llvm::BasicBlock* target_bb = GetOrCreateLabelBlock(frame, st.goto_target);
    builder_.CreateBr(target_bb);
    return {true, ""};
  }

  llvm_backend::Result EmitThrowStmt(const HIRStmt& st, FunctionFrame* frame) {
    if (!HasExpr(st.expr)) {
      return {false, "irbuilder emit: throw requires payload expression"};
    }
    const ExprResult payload_value = EmitExpr(st.expr, frame);
    if (!payload_value.ok) {
      return {false, payload_value.message};
    }
    llvm::Value* payload = CoerceInt64(payload_value.value);
    if (payload == nullptr) {
      return {false, "irbuilder emit: throw payload must be integer-convertible"};
    }

    llvm::FunctionCallee throw_fn =
        module_->getOrInsertFunction("hc_throw_i64",
                                     llvm::FunctionType::get(
                                         llvm::Type::getVoidTy(*context_), {TypeI64()}, false));
    builder_.CreateCall(throw_fn, {payload});
    builder_.CreateUnreachable();
    return {true, ""};
  }

  llvm_backend::Result EmitTryCatchStmt(const HIRStmt& st, FunctionFrame* frame) {
    llvm::ArrayType* storage_ty =
        llvm::ArrayType::get(llvm::Type::getInt8Ty(*context_), kTryFrameStorageSize);
    llvm::AllocaInst* storage = CreateEntryAlloca(frame->function, "hc.try.frame", storage_ty);
    storage->setAlignment(llvm::Align(kTryFrameStorageAlignment));

    llvm::Value* frame_ptr = builder_.CreateBitCast(storage, TypePtr());

    llvm::FunctionCallee push_fn = module_->getOrInsertFunction(
        "hc_try_push", llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {TypePtr()},
                                                false));
    llvm::FunctionCallee pop_fn = module_->getOrInsertFunction(
        "hc_try_pop",
        llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {TypePtr()}, false));
    llvm::FunctionCallee setjmp_fn = module_->getOrInsertFunction(
        "_setjmp",
        llvm::FunctionType::get(llvm::Type::getInt32Ty(*context_), {TypePtr()}, false));

    builder_.CreateCall(push_fn, {frame_ptr});
    llvm::Value* sj_value = builder_.CreateCall(setjmp_fn, {frame_ptr});
    llvm::Value* run_try = builder_.CreateICmpEQ(
        sj_value, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0));

    llvm::BasicBlock* try_bb = llvm::BasicBlock::Create(*context_, "try.body", frame->function);
    llvm::BasicBlock* catch_bb =
        llvm::BasicBlock::Create(*context_, "catch.body", frame->function);
    llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(*context_, "try.end", frame->function);

    builder_.CreateCondBr(run_try, try_bb, catch_bb);

    builder_.SetInsertPoint(try_bb);
    const llvm_backend::Result try_result = EmitStmtList(st.try_body, frame);
    if (!try_result.ok) {
      return try_result;
    }
    if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
      builder_.CreateCall(pop_fn, {frame_ptr});
      builder_.CreateBr(end_bb);
    }

    builder_.SetInsertPoint(catch_bb);
    const llvm_backend::Result catch_result = EmitStmtList(st.catch_body, frame);
    if (!catch_result.ok) {
      return catch_result;
    }
    if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
      builder_.CreateBr(end_bb);
    }

    builder_.SetInsertPoint(end_bb);
    return {true, ""};
  }

  llvm_backend::Result EmitInlineAsmStmt(const HIRStmt& st, FunctionFrame* frame) {
    std::string asm_template = st.asm_template;
    if (asm_template.empty()) {
      return {false, "irbuilder emit: inline asm requires non-empty body/template"};
    }

    if (asm_template.size() >= 2 && asm_template.front() == '"' && asm_template.back() == '"') {
      asm_template = DecodeQuotedString(asm_template);
    }

    std::vector<std::string> constraints;
    constraints.reserve(st.asm_constraints.size());
    unsigned output_count = 0;
    std::vector<llvm::Type*> output_types;
    output_types.reserve(st.asm_constraints.size());
    std::vector<llvm::Type*> arg_types;
    std::vector<llvm::Value*> args;

    if (!st.asm_operand_present.empty() &&
        st.asm_operand_present.size() != st.asm_constraints.size()) {
      return {false, "irbuilder emit: malformed inline asm operand flags"};
    }
    if (!st.asm_operands.empty() && st.asm_operands.size() != st.asm_constraints.size()) {
      return {false, "irbuilder emit: malformed inline asm operand list"};
    }

    for (std::size_t i = 0; i < st.asm_constraints.size(); ++i) {
      const std::string& raw_constraint = st.asm_constraints[i];
      std::string constraint = TrimCopy(raw_constraint);
      if (constraint.size() >= 2 && constraint.front() == '"' && constraint.back() == '"') {
        constraint = DecodeQuotedString(constraint);
      }
      if (constraint.empty()) {
        continue;
      }
      const bool has_operand =
          !st.asm_operand_present.empty() && i < st.asm_operand_present.size()
              ? st.asm_operand_present[i]
              : false;

      const bool is_output = !constraint.empty() && constraint.front() == '=';
      const bool is_clobber = !constraint.empty() && constraint.front() == '~';
      const bool is_legacy_register_only =
          constraint.size() >= 3 && constraint.front() == '{' && constraint.back() == '}';

      if (is_output) {
        if (has_operand) {
          return {false, "irbuilder emit: inline asm output constraints do not take operand expressions"};
        }
        constraints.push_back(constraint);
        ++output_count;
        output_types.push_back(TypeI64());
        continue;
      }

      if (is_clobber || is_legacy_register_only) {
        if (has_operand) {
          return {false, "irbuilder emit: inline asm clobber constraints do not take operand expressions"};
        }
        constraints.push_back(is_legacy_register_only ? "~" + constraint : constraint);
        continue;
      }

      constraints.push_back(constraint);

      if (!has_operand) {
        return {false, "irbuilder emit: inline asm input constraint requires operand expression: " +
                           constraint};
      }
      if (i >= st.asm_operands.size()) {
        return {false, "irbuilder emit: inline asm input constraint missing operand payload"};
      }

      const ExprResult operand = EmitExpr(st.asm_operands[i], frame);
      if (!operand.ok) {
        return {false, operand.message};
      }
      llvm::Value* coerced = CoerceInt64(operand.value);
      if (coerced == nullptr) {
        return {false, "irbuilder emit: inline asm operand must be integer/pointer-compatible"};
      }
      arg_types.push_back(coerced->getType());
      args.push_back(coerced);
    }

    const std::string constraint_string = JoinTokens(constraints, ",");
    llvm::Type* ret_ty = llvm::Type::getVoidTy(*context_);
    if (output_count == 1) {
      ret_ty = output_types.front();
    } else if (output_count > 1) {
      ret_ty = llvm::StructType::get(*context_, output_types);
    }
    llvm::FunctionType* asm_ty = llvm::FunctionType::get(ret_ty, arg_types, false);
    llvm::InlineAsm* asm_fn = llvm::InlineAsm::get(asm_ty, asm_template, constraint_string, true);
    llvm::CallInst* call = builder_.CreateCall(asm_fn, args);
    (void)call;
    return {true, ""};
  }

  static LaneInfo ParseLaneInfo(std::string_view lane_selector) {
    const std::string lane = TrimCopy(lane_selector);
    if (lane.empty()) {
      return {};
    }

    std::string lowered = lane;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });

    LaneInfo info;
    if (lowered == "i8" || lowered == "u8") {
      info.valid = true;
      info.bits = 8;
      info.is_signed = lowered[0] == 'i';
      return info;
    }
    if (lowered == "i16" || lowered == "u16") {
      info.valid = true;
      info.bits = 16;
      info.is_signed = lowered[0] == 'i';
      return info;
    }
    if (lowered == "i32" || lowered == "u32") {
      info.valid = true;
      info.bits = 32;
      info.is_signed = lowered[0] == 'i';
      return info;
    }
    if (lowered == "i64" || lowered == "u64") {
      info.valid = true;
      info.bits = 64;
      info.is_signed = lowered[0] == 'i';
      return info;
    }
    return {};
  }

  static unsigned ParseIntegralBitWidth(std::string_view holy_type) {
    std::string normalized = TrimCopy(holy_type);
    while (!normalized.empty() && normalized.back() == '*') {
      normalized.pop_back();
      normalized = TrimCopy(normalized);
    }

    if (normalized == "Bool" || normalized == "Bool(chained)") {
      return 1;
    }

    if (!normalized.empty() && (normalized.front() == 'I' || normalized.front() == 'U')) {
      std::size_t i = 1;
      while (i < normalized.size() &&
             std::isdigit(static_cast<unsigned char>(normalized[i])) != 0) {
        ++i;
      }
      if (i > 1) {
        try {
          const unsigned bits = static_cast<unsigned>(std::stoul(normalized.substr(1, i - 1)));
          if (bits > 0) {
            return bits;
          }
        } catch (...) {
        }
      }
    }

    return 64;
  }

  llvm::Value* CastIntegerWithSignedness(llvm::Value* value, llvm::Type* target_type,
                                         bool signed_extend) {
    if (value == nullptr || target_type == nullptr || !target_type->isIntegerTy()) {
      return nullptr;
    }
    if (value->getType() == target_type) {
      return value;
    }

    if (value->getType()->isPointerTy()) {
      return builder_.CreatePtrToInt(value, target_type);
    }
    if (!value->getType()->isIntegerTy()) {
      return nullptr;
    }

    const unsigned from_bits = value->getType()->getIntegerBitWidth();
    const unsigned to_bits = target_type->getIntegerBitWidth();
    if (from_bits == to_bits) {
      return value;
    }
    if (from_bits < to_bits) {
      return signed_extend ? builder_.CreateSExt(value, target_type)
                           : builder_.CreateZExt(value, target_type);
    }
    return builder_.CreateTrunc(value, target_type);
  }

  ExprResult EmitLaneLoad(const HIRExpr& expr, FunctionFrame* frame) {
    if (expr.children.size() != 2) {
      return {false, nullptr, "irbuilder emit: invalid lane expression"};
    }

    const LaneInfo lane = ParseLaneInfo(expr.text);
    if (!lane.valid) {
      return {false, nullptr, "irbuilder emit: unknown lane selector " + expr.text};
    }

    const HIRExpr& base_expr = expr.children[0];
    const HIRExpr& index_expr = expr.children[1];

    const ExprResult base_value = EmitExpr(base_expr, frame);
    if (!base_value.ok) {
      return base_value;
    }
    const ExprResult index_value = EmitExpr(index_expr, frame);
    if (!index_value.ok) {
      return index_value;
    }

    const unsigned parsed_base_bits = ParseIntegralBitWidth(base_expr.type);
    const unsigned base_bits = std::max<unsigned>(lane.bits, parsed_base_bits == 0 ? 64 : parsed_base_bits);
    if (base_bits == 0 || base_bits > 64) {
      return {false, nullptr, "irbuilder emit: invalid lane base width"};
    }

    llvm::Type* base_int_ty = llvm::Type::getIntNTy(*context_, base_bits);
    llvm::Value* base_int = CastIntegerWithSignedness(base_value.value, base_int_ty, true);
    if (base_int == nullptr) {
      return {false, nullptr, "irbuilder emit: lane base is not integer-convertible"};
    }

    llvm::Value* index_i64 = CoerceInt64(index_value.value);
    if (index_i64 == nullptr) {
      return {false, nullptr, "irbuilder emit: lane index must be integer-convertible"};
    }
    llvm::Value* index_int = CastIntegerWithSignedness(index_i64, base_int_ty, false);
    if (index_int == nullptr) {
      return {false, nullptr, "irbuilder emit: lane index type conversion failed"};
    }

    llvm::Value* lane_bits_const = llvm::ConstantInt::get(base_int_ty, lane.bits, false);
    llvm::Value* shift_amount = builder_.CreateMul(index_int, lane_bits_const);
    llvm::Value* shifted = builder_.CreateLShr(base_int, shift_amount);

    const std::uint64_t raw_mask =
        lane.bits >= 64 ? std::numeric_limits<std::uint64_t>::max() : ((1ULL << lane.bits) - 1ULL);
    llvm::Value* mask = llvm::ConstantInt::get(base_int_ty, raw_mask, false);
    llvm::Value* lane_bits_value = builder_.CreateAnd(shifted, mask);

    llvm::Type* lane_int_ty = llvm::Type::getIntNTy(*context_, lane.bits);
    if (lane.bits != base_bits) {
      lane_bits_value = builder_.CreateTrunc(lane_bits_value, lane_int_ty);
    }

    llvm::Value* lane_i64 = CastIntegerWithSignedness(lane_bits_value, TypeI64(), lane.is_signed);
    if (lane_i64 == nullptr) {
      return {false, nullptr, "irbuilder emit: lane result conversion failed"};
    }
    llvm::Type* target_ty = ToLlvmType(expr.type.empty() ? "I64" : expr.type);
    if (target_ty->isIntegerTy()) {
      // Keep lane values widened to i64 so unsigned lanes keep zero-extended
      // semantics in later arithmetic/comparisons.
      return {true, lane_i64, ""};
    }
    llvm::Value* casted = CastIfNeeded(lane_i64, target_ty);
    if (casted == nullptr) {
      return {false, nullptr, "irbuilder emit: lane result conversion failed"};
    }
    return {true, casted, ""};
  }

  ExprResult StoreAssignable(const HIRExpr& lhs_expr, llvm::Value* rhs_value, FunctionFrame* frame) {
    if (lhs_expr.kind != HIRExpr::Kind::kLane) {
      const LValueResult lhs = EmitLValue(lhs_expr, frame);
      if (!lhs.ok) {
        return {false, nullptr, lhs.message};
      }
      llvm::Value* casted = CastIfNeeded(rhs_value, lhs.pointee_type);
      if (casted == nullptr) {
        return {false, nullptr, "irbuilder emit: assignment expression type mismatch"};
      }
      builder_.CreateStore(casted, lhs.ptr);
      return {true, casted, ""};
    }

    if (lhs_expr.children.size() != 2) {
      return {false, nullptr, "irbuilder emit: invalid lane assignment target"};
    }

    const LaneInfo lane = ParseLaneInfo(lhs_expr.text);
    if (!lane.valid) {
      return {false, nullptr, "irbuilder emit: unknown lane selector " + lhs_expr.text};
    }

    const HIRExpr& base_expr = lhs_expr.children[0];
    const HIRExpr& index_expr = lhs_expr.children[1];

    const ExprResult base_value = EmitExpr(base_expr, frame);
    if (!base_value.ok) {
      return base_value;
    }
    const ExprResult index_value = EmitExpr(index_expr, frame);
    if (!index_value.ok) {
      return index_value;
    }

    const unsigned parsed_base_bits = ParseIntegralBitWidth(base_expr.type);
    const unsigned base_bits = std::max<unsigned>(lane.bits, parsed_base_bits == 0 ? 64 : parsed_base_bits);
    if (base_bits == 0 || base_bits > 64) {
      return {false, nullptr, "irbuilder emit: invalid lane base width"};
    }

    llvm::Type* base_int_ty = llvm::Type::getIntNTy(*context_, base_bits);
    llvm::Value* base_int = CastIntegerWithSignedness(base_value.value, base_int_ty, true);
    if (base_int == nullptr) {
      return {false, nullptr, "irbuilder emit: lane base is not integer-convertible"};
    }

    llvm::Value* index_i64 = CoerceInt64(index_value.value);
    if (index_i64 == nullptr) {
      return {false, nullptr, "irbuilder emit: lane index must be integer-convertible"};
    }
    llvm::Value* index_int = CastIntegerWithSignedness(index_i64, base_int_ty, false);
    if (index_int == nullptr) {
      return {false, nullptr, "irbuilder emit: lane index type conversion failed"};
    }

    llvm::Type* lane_int_ty = llvm::Type::getIntNTy(*context_, lane.bits);
    llvm::Value* rhs_lane = CastIntegerWithSignedness(rhs_value, lane_int_ty, lane.is_signed);
    if (rhs_lane == nullptr) {
      return {false, nullptr, "irbuilder emit: lane assignment rhs is not integer-convertible"};
    }

    llvm::Value* lane_bits_const = llvm::ConstantInt::get(base_int_ty, lane.bits, false);
    llvm::Value* shift_amount = builder_.CreateMul(index_int, lane_bits_const);
    const std::uint64_t raw_mask =
        lane.bits >= 64 ? std::numeric_limits<std::uint64_t>::max() : ((1ULL << lane.bits) - 1ULL);
    llvm::Value* base_mask = llvm::ConstantInt::get(base_int_ty, raw_mask, false);
    llvm::Value* shifted_mask = builder_.CreateShl(base_mask, shift_amount);
    llvm::Value* cleared_base = builder_.CreateAnd(base_int, builder_.CreateNot(shifted_mask));

    llvm::Value* rhs_base = CastIntegerWithSignedness(rhs_lane, base_int_ty, false);
    if (rhs_base == nullptr) {
      return {false, nullptr, "irbuilder emit: lane assignment rhs type conversion failed"};
    }
    llvm::Value* shifted_rhs = builder_.CreateShl(rhs_base, shift_amount);
    llvm::Value* masked_rhs = builder_.CreateAnd(shifted_rhs, shifted_mask);
    llvm::Value* updated_base = builder_.CreateOr(cleared_base, masked_rhs);

    const ExprResult base_store = StoreAssignable(base_expr, updated_base, frame);
    if (!base_store.ok) {
      return base_store;
    }

    llvm::Type* lane_result_ty = ToLlvmType(lhs_expr.type.empty() ? "I64" : lhs_expr.type);
    if (lane_result_ty->isIntegerTy()) {
      llvm::Value* casted = CastIntegerWithSignedness(rhs_lane, lane_result_ty, lane.is_signed);
      if (casted == nullptr) {
        return {false, nullptr, "irbuilder emit: lane assignment result conversion failed"};
      }
      return {true, casted, ""};
    }
    llvm::Value* rhs_i64 = CastIntegerWithSignedness(rhs_lane, TypeI64(), lane.is_signed);
    if (rhs_i64 == nullptr) {
      return {false, nullptr, "irbuilder emit: lane assignment result conversion failed"};
    }
    llvm::Value* casted = CastIfNeeded(rhs_i64, lane_result_ty);
    if (casted == nullptr) {
      return {false, nullptr, "irbuilder emit: lane assignment result conversion failed"};
    }
    return {true, casted, ""};
  }

  ExprResult EmitAssignExpr(const HIRExpr& lhs_expr, std::string_view assign_op,
                            const HIRExpr& rhs_expr, FunctionFrame* frame) {
    const ExprResult rhs = EmitExpr(rhs_expr, frame);
    if (!rhs.ok) {
      return rhs;
    }

    llvm::Value* to_store = rhs.value;
    if (assign_op != "=") {
      const ExprResult lhs_current = EmitExpr(lhs_expr, frame);
      if (!lhs_current.ok) {
        return lhs_current;
      }
      const BinaryResult combined =
          EmitBinaryOp(AssignOpToBinary(std::string(assign_op)), lhs_current.value, rhs.value);
      if (!combined.ok) {
        return {false, nullptr, combined.message};
      }
      to_store = combined.value;
    }

    return StoreAssignable(lhs_expr, to_store, frame);
  }

  LValueResult EmitLValue(const HIRExpr& expr, FunctionFrame* frame) {
    switch (expr.kind) {
      case HIRExpr::Kind::kVar: {
        const auto local_it = frame->locals.find(expr.text);
        if (local_it != frame->locals.end()) {
          llvm::AllocaInst* slot = local_it->second;
          return {true, slot, slot->getAllocatedType(), ""};
        }

        const auto global_it = globals_.find(expr.text);
        if (global_it != globals_.end()) {
          llvm::GlobalVariable* slot = global_it->second;
          return {true, slot, slot->getValueType(), ""};
        }

        if (expr.type.rfind("fn ", 0) == 0) {
          return {false, nullptr, nullptr, "irbuilder emit: unknown function symbol " + expr.text};
        }

        llvm::Type* guessed_ty = ToLlvmType(expr.type.empty() ? "I64" : expr.type);
        llvm::GlobalVariable* extern_global = module_->getGlobalVariable(expr.text, true);
        if (extern_global == nullptr) {
          extern_global = new llvm::GlobalVariable(*module_, guessed_ty, false,
                                                   llvm::GlobalValue::ExternalLinkage, nullptr,
                                                   expr.text);
        } else if (extern_global->getValueType() != guessed_ty) {
          return {false, nullptr, nullptr,
                  "irbuilder emit: conflicting external variable type for " + expr.text};
        }
        globals_[expr.text] = extern_global;
        return {true, extern_global, extern_global->getValueType(), ""};
      }

      case HIRExpr::Kind::kUnary: {
        if (expr.text != "*" || expr.children.size() != 1) {
          return {false, nullptr, nullptr, "irbuilder emit: unsupported unary lvalue operator"};
        }

        const ExprResult base = EmitExpr(expr.children[0], frame);
        if (!base.ok) {
          return {false, nullptr, nullptr, base.message};
        }

        llvm::Type* pointee_ty = ToLlvmType(expr.type.empty() ? "I64" : expr.type);
        llvm::Type* ptr_ty = TypePtr();
        llvm::Value* ptr = CastIfNeeded(base.value, ptr_ty);
        if (ptr == nullptr) {
          return {false, nullptr, nullptr, "irbuilder emit: unary '*' requires pointer operand"};
        }
        return {true, ptr, pointee_ty, ""};
      }

      case HIRExpr::Kind::kMember: {
        if (expr.children.size() != 1) {
          return {false, nullptr, nullptr, "irbuilder emit: invalid member expression"};
        }
        const HIRExpr& base_expr = expr.children[0];
        const std::string aggregate_name = NormalizeAggregateTypeName(base_expr.type);
        const auto aggregate_it = aggregate_layouts_.find(aggregate_name);
        if (aggregate_it == aggregate_layouts_.end() || aggregate_it->second.type == nullptr) {
          llvm::Type* member_ty = ToLlvmType(expr.type.empty() ? "I64" : expr.type);
          llvm::Type* member_ptr_ty = TypePtr();
          if (base_expr.type.find('*') != std::string::npos) {
            const ExprResult base_ptr = EmitExpr(base_expr, frame);
            if (!base_ptr.ok) {
              return {false, nullptr, nullptr, base_ptr.message};
            }
            llvm::Value* casted = CastIfNeeded(base_ptr.value, member_ptr_ty);
            if (casted == nullptr) {
              return {false, nullptr, nullptr, "irbuilder emit: invalid pointer member base"};
            }
            return {true, casted, member_ty, ""};
          }

          const LValueResult base = EmitLValue(base_expr, frame);
          if (!base.ok) {
            return base;
          }
          llvm::Value* casted = CastIfNeeded(base.ptr, member_ptr_ty);
          if (casted == nullptr) {
            return {false, nullptr, nullptr,
                    "irbuilder emit: unsupported member base layout in primary backend"};
          }
          return {true, casted, member_ty, ""};
        }

        const auto member_it = aggregate_it->second.members.find(expr.text);
        if (member_it == aggregate_it->second.members.end() || member_it->second.type == nullptr) {
          return {false, nullptr, nullptr, "irbuilder emit: unknown aggregate member " + expr.text};
        }

        llvm::StructType* aggregate_ty = aggregate_it->second.type;
        llvm::Type* aggregate_ptr_ty = TypePtr();
        llvm::Value* aggregate_ptr = nullptr;

        if (base_expr.type.find('*') != std::string::npos) {
          const ExprResult base_ptr = EmitExpr(base_expr, frame);
          if (!base_ptr.ok) {
            return {false, nullptr, nullptr, base_ptr.message};
          }
          aggregate_ptr = CastIfNeeded(base_ptr.value, aggregate_ptr_ty);
        } else {
          const LValueResult base = EmitLValue(base_expr, frame);
          if (!base.ok) {
            return base;
          }
          aggregate_ptr = CastIfNeeded(base.ptr, aggregate_ptr_ty);
        }

        if (aggregate_ptr == nullptr) {
          return {false, nullptr, nullptr, "irbuilder emit: invalid aggregate member base pointer"};
        }

        llvm::Value* field_ptr =
            builder_.CreateStructGEP(aggregate_ty, aggregate_ptr, member_it->second.index);
        return {true, field_ptr, member_it->second.type, ""};
      }

      case HIRExpr::Kind::kIndex: {
        if (expr.children.size() != 2) {
          return {false, nullptr, nullptr, "irbuilder emit: invalid index expression"};
        }
        const ExprResult base = EmitExpr(expr.children[0], frame);
        if (!base.ok) {
          return {false, nullptr, nullptr, base.message};
        }
        const ExprResult index = EmitExpr(expr.children[1], frame);
        if (!index.ok) {
          return {false, nullptr, nullptr, index.message};
        }
        llvm::Value* index_i64 = CoerceInt64(index.value);
        if (index_i64 == nullptr) {
          return {false, nullptr, nullptr, "irbuilder emit: index must be integer-convertible"};
        }

        llvm::Type* elem_ty = ToLlvmType(expr.type.empty() ? "I64" : expr.type);
        llvm::Type* elem_ptr_ty = TypePtr();
        llvm::Value* base_ptr = CastIfNeeded(base.value, elem_ptr_ty);
        if (base_ptr == nullptr) {
          return {false, nullptr, nullptr, "irbuilder emit: index base must be pointer"};
        }
        llvm::Value* elem_ptr = builder_.CreateGEP(elem_ty, base_ptr, index_i64);
        return {true, elem_ptr, elem_ty, ""};
      }

      default:
        return {false, nullptr, nullptr,
                "irbuilder emit: expression is not assignable: " + ExprKindName(expr.kind)};
    }
  }

  ExprResult EmitExpr(const HIRExpr& expr, FunctionFrame* frame) {
    switch (expr.kind) {
      case HIRExpr::Kind::kIntLiteral: {
        std::int64_t value = 0;
        if (!ParseIntegerLiteralText(expr.text, &value)) {
          return {false, nullptr, "irbuilder emit: invalid integer literal: " + expr.text};
        }
        llvm::Type* ty = ToLlvmType(expr.type.empty() ? "I64" : expr.type);
        if (!ty->isIntegerTy()) {
          ty = TypeI64();
        }
        return {true, llvm::ConstantInt::get(ty, static_cast<std::uint64_t>(value), true), ""};
      }

      case HIRExpr::Kind::kStringLiteral:
        return {true, GetOrCreateStringLiteral(expr.text), ""};

      case HIRExpr::Kind::kDollar:
        return {true, llvm::ConstantInt::get(TypeI64(), 0), ""};

      case HIRExpr::Kind::kVar: {
        const LValueResult lvalue = EmitLValue(expr, frame);
        if (!lvalue.ok) {
          return {false, nullptr, lvalue.message};
        }
        return {true, builder_.CreateLoad(lvalue.pointee_type, lvalue.ptr), ""};
      }

      case HIRExpr::Kind::kAssign:
        if (expr.children.size() != 2) {
          return {false, nullptr, "irbuilder emit: invalid assignment expression"};
        }
        return EmitAssignExpr(expr.children[0], expr.text, expr.children[1], frame);

      case HIRExpr::Kind::kUnary: {
        if (expr.children.size() != 1) {
          return {false, nullptr, "irbuilder emit: invalid unary expression"};
        }

        if (expr.text == "++" || expr.text == "--") {
          const LValueResult lvalue = EmitLValue(expr.children[0], frame);
          if (!lvalue.ok) {
            return {false, nullptr, lvalue.message};
          }
          llvm::Value* old_value = builder_.CreateLoad(lvalue.pointee_type, lvalue.ptr);
          llvm::Value* updated = nullptr;
          if (old_value->getType()->isPointerTy()) {
            llvm::Value* as_i64 = builder_.CreatePtrToInt(old_value, TypeI64());
            llvm::Value* one = llvm::ConstantInt::get(TypeI64(), 1);
            llvm::Value* next_i64 = (expr.text == "++")
                                        ? builder_.CreateAdd(as_i64, one)
                                        : builder_.CreateSub(as_i64, one);
            updated = builder_.CreateIntToPtr(next_i64, old_value->getType());
          } else {
            llvm::Value* as_i64 = CoerceInt64(old_value);
            if (as_i64 == nullptr) {
              return {false, nullptr, "irbuilder emit: unary inc/dec requires integer/pointer lvalue"};
            }
            llvm::Value* one = llvm::ConstantInt::get(TypeI64(), 1);
            llvm::Value* next_i64 = (expr.text == "++")
                                        ? builder_.CreateAdd(as_i64, one)
                                        : builder_.CreateSub(as_i64, one);
            updated = CastIfNeeded(next_i64, old_value->getType());
            if (updated == nullptr) {
              return {false, nullptr, "irbuilder emit: unary inc/dec type conversion failed"};
            }
          }
          builder_.CreateStore(updated, lvalue.ptr);
          return {true, updated, ""};
        }

        if (expr.text == "&") {
          if (expr.children[0].kind == HIRExpr::Kind::kVar) {
            llvm::Function* fn_value = module_->getFunction(expr.children[0].text);
            if (fn_value != nullptr) {
              llvm::Type* target_ty = ToLlvmType(expr.type.empty() ? "U8*" : expr.type);
              llvm::Value* casted = CastIfNeeded(fn_value, target_ty);
              if (casted == nullptr) {
                return {false, nullptr,
                        "irbuilder emit: unary '&' function address type conversion failed"};
              }
              return {true, casted, ""};
            }
          }

          const LValueResult lvalue = EmitLValue(expr.children[0], frame);
          if (!lvalue.ok) {
            return {false, nullptr, lvalue.message};
          }
          llvm::Type* target_ty = ToLlvmType(expr.type.empty() ? "U8*" : expr.type);
          llvm::Value* casted = CastIfNeeded(lvalue.ptr, target_ty);
          if (casted == nullptr) {
            return {false, nullptr, "irbuilder emit: unary '&' produced non-castable address"};
          }
          return {true, casted, ""};
        }

        if (expr.text == "*") {
          const LValueResult lvalue = EmitLValue(expr, frame);
          if (!lvalue.ok) {
            return {false, nullptr, lvalue.message};
          }
          return {true, builder_.CreateLoad(lvalue.pointee_type, lvalue.ptr), ""};
        }

        const ExprResult child = EmitExpr(expr.children[0], frame);
        if (!child.ok) {
          return child;
        }

        if (expr.text == "+") {
          return child;
        }
        if (expr.text == "-") {
          llvm::Value* operand = CoerceInt64(child.value);
          if (operand == nullptr) {
            return {false, nullptr, "irbuilder emit: unary '-' requires integer operand"};
          }
          return {true, builder_.CreateNeg(operand), ""};
        }
        if (expr.text == "~") {
          llvm::Value* operand = CoerceInt64(child.value);
          if (operand == nullptr) {
            return {false, nullptr, "irbuilder emit: unary '~' requires integer operand"};
          }
          return {true, builder_.CreateNot(operand), ""};
        }
        if (expr.text == "!") {
          llvm::Value* b = ToBool(child.value);
          if (b == nullptr) {
            return {false, nullptr, "irbuilder emit: unary '!' requires bool-convertible operand"};
          }
          llvm::Value* flipped = builder_.CreateXor(b, llvm::ConstantInt::get(TypeI1(), 1));
          return {true, builder_.CreateZExt(flipped, TypeI64()), ""};
        }

        return {false, nullptr, "irbuilder emit: unsupported unary operator " + expr.text};
      }

      case HIRExpr::Kind::kBinary: {
        if (expr.children.size() != 2) {
          return {false, nullptr, "irbuilder emit: invalid binary expression"};
        }
        const ExprResult lhs = EmitExpr(expr.children[0], frame);
        if (!lhs.ok) {
          return lhs;
        }
        const ExprResult rhs = EmitExpr(expr.children[1], frame);
        if (!rhs.ok) {
          return rhs;
        }

        const BinaryResult combined = EmitBinaryOp(expr.text, lhs.value, rhs.value);
        if (!combined.ok) {
          return {false, nullptr, combined.message};
        }
        return {true, combined.value, ""};
      }

      case HIRExpr::Kind::kCall: {
        if (!expr.text.empty()) {
          llvm::Function* callee = module_->getFunction(expr.text);
          if (callee == nullptr) {
            return {false, nullptr, "irbuilder emit: unknown function " + expr.text};
          }

          if (expr.children.size() != callee->arg_size()) {
            return {false, nullptr, "irbuilder emit: argument count mismatch for function " +
                                        expr.text};
          }

          std::vector<llvm::Value*> args;
          args.reserve(expr.children.size());
          llvm::FunctionType* fnty = callee->getFunctionType();
          for (std::size_t i = 0; i < expr.children.size(); ++i) {
            const ExprResult value = EmitExpr(expr.children[i], frame);
            if (!value.ok) {
              return value;
            }

            llvm::Type* param_ty = fnty->getParamType(static_cast<unsigned>(i));
            llvm::Value* casted = CastIfNeeded(value.value, param_ty);
            if (casted == nullptr) {
              return {false, nullptr, "irbuilder emit: call argument type mismatch for function " +
                                          expr.text};
            }
            args.push_back(casted);
          }

          llvm::CallInst* call = builder_.CreateCall(callee, args);
          if (callee->getReturnType()->isVoidTy()) {
            return {true, llvm::ConstantInt::get(TypeI64(), 0), ""};
          }
          return {true, call, ""};
        }

        if (expr.children.empty()) {
          return {false, nullptr, "irbuilder emit: invalid indirect call expression"};
        }

        const ExprResult callee_value = EmitExpr(expr.children[0], frame);
        if (!callee_value.ok) {
          return callee_value;
        }

        std::vector<llvm::Type*> param_types;
        std::vector<llvm::Value*> args;
        param_types.reserve(expr.children.size() - 1);
        args.reserve(expr.children.size() - 1);
        for (std::size_t i = 1; i < expr.children.size(); ++i) {
          llvm::Type* param_ty =
              ToLlvmType(expr.children[i].type.empty() ? "I64" : expr.children[i].type);
          const ExprResult arg_value = EmitExpr(expr.children[i], frame);
          if (!arg_value.ok) {
            return arg_value;
          }
          llvm::Value* casted = CastIfNeeded(arg_value.value, param_ty);
          if (casted == nullptr) {
            return {false, nullptr, "irbuilder emit: indirect call argument type mismatch"};
          }
          param_types.push_back(param_ty);
          args.push_back(casted);
        }

        llvm::Type* return_ty = ToLlvmType(expr.type.empty() ? "I64" : expr.type);
        llvm::FunctionType* call_ty = llvm::FunctionType::get(return_ty, param_types, false);
        llvm::Value* callee_ptr = CastIfNeeded(callee_value.value, TypePtr());
        if (callee_ptr == nullptr) {
          return {false, nullptr, "irbuilder emit: indirect call target is not callable"};
        }

        llvm::CallInst* call = builder_.CreateCall(call_ty, callee_ptr, args);
        if (return_ty->isVoidTy()) {
          return {true, llvm::ConstantInt::get(TypeI64(), 0), ""};
        }
        return {true, call, ""};
      }

      case HIRExpr::Kind::kCast: {
        if (expr.children.size() != 1) {
          return {false, nullptr, "irbuilder emit: invalid cast expression"};
        }
        const ExprResult source = EmitExpr(expr.children[0], frame);
        if (!source.ok) {
          return source;
        }
        llvm::Type* target_ty = ToLlvmType(expr.type.empty() ? "I64" : expr.type);
        llvm::Value* casted = CastIfNeeded(source.value, target_ty);
        if (casted == nullptr) {
          return {false, nullptr, "irbuilder emit: unsupported cast in primary backend"};
        }
        return {true, casted, ""};
      }

      case HIRExpr::Kind::kComma: {
        if (expr.children.empty()) {
          return {false, nullptr, "irbuilder emit: invalid empty comma expression"};
        }
        ExprResult value;
        for (const HIRExpr& child : expr.children) {
          value = EmitExpr(child, frame);
          if (!value.ok) {
            return value;
          }
        }
        return value;
      }

      case HIRExpr::Kind::kPostfix: {
        if (expr.children.size() != 1) {
          return {false, nullptr, "irbuilder emit: invalid postfix expression"};
        }
        if (expr.text != "++" && expr.text != "--") {
          return {false, nullptr, "irbuilder emit: unsupported postfix operator " + expr.text};
        }

        const LValueResult lvalue = EmitLValue(expr.children[0], frame);
        if (!lvalue.ok) {
          return {false, nullptr, lvalue.message};
        }

        llvm::Value* old_value = builder_.CreateLoad(lvalue.pointee_type, lvalue.ptr);
        llvm::Value* updated = nullptr;
        if (old_value->getType()->isPointerTy()) {
          llvm::Value* as_i64 = builder_.CreatePtrToInt(old_value, TypeI64());
          llvm::Value* one = llvm::ConstantInt::get(TypeI64(), 1);
          llvm::Value* next_i64 =
              (expr.text == "++") ? builder_.CreateAdd(as_i64, one) : builder_.CreateSub(as_i64, one);
          updated = builder_.CreateIntToPtr(next_i64, old_value->getType());
        } else {
          llvm::Value* as_i64 = CoerceInt64(old_value);
          if (as_i64 == nullptr) {
            return {false, nullptr, "irbuilder emit: postfix requires integer/pointer lvalue"};
          }
          llvm::Value* one = llvm::ConstantInt::get(TypeI64(), 1);
          llvm::Value* next_i64 =
              (expr.text == "++") ? builder_.CreateAdd(as_i64, one) : builder_.CreateSub(as_i64, one);
          updated = CastIfNeeded(next_i64, old_value->getType());
          if (updated == nullptr) {
            return {false, nullptr, "irbuilder emit: postfix type conversion failed"};
          }
        }
        builder_.CreateStore(updated, lvalue.ptr);
        return {true, old_value, ""};
      }

      case HIRExpr::Kind::kLane:
        return EmitLaneLoad(expr, frame);

      case HIRExpr::Kind::kMember:
      case HIRExpr::Kind::kIndex: {
        const LValueResult lvalue = EmitLValue(expr, frame);
        if (!lvalue.ok) {
          return {false, nullptr, lvalue.message};
        }
        return {true, builder_.CreateLoad(lvalue.pointee_type, lvalue.ptr), ""};
      }
    }

    return {false, nullptr, "irbuilder emit: invalid expression kind"};
  }

  llvm_backend::Result EmitAtomicAssignExpr(const HIRExpr& lhs_expr, const std::string& assign_op,
                                            const HIRExpr& rhs_expr, FunctionFrame* frame) {
    const LValueResult lhs = EmitLValue(lhs_expr, frame);
    if (!lhs.ok) {
      return {false, lhs.message};
    }
    if (lhs.pointee_type == nullptr || !lhs.pointee_type->isIntegerTy()) {
      return {false, "irbuilder emit: lock requires integer lvalue target"};
    }

    const ExprResult rhs = EmitExpr(rhs_expr, frame);
    if (!rhs.ok) {
      return {false, rhs.message};
    }

    llvm::Value* rhs_value = CastIfNeeded(rhs.value, lhs.pointee_type);
    if (rhs_value == nullptr) {
      return {false, "irbuilder emit: lock assignment rhs type mismatch"};
    }

    llvm::AtomicRMWInst::BinOp rmw_op = llvm::AtomicRMWInst::BinOp::BAD_BINOP;
    if (assign_op == "=") {
      rmw_op = llvm::AtomicRMWInst::BinOp::Xchg;
    } else if (assign_op == "+=") {
      rmw_op = llvm::AtomicRMWInst::BinOp::Add;
    } else if (assign_op == "-=") {
      rmw_op = llvm::AtomicRMWInst::BinOp::Sub;
    } else if (assign_op == "&=") {
      rmw_op = llvm::AtomicRMWInst::BinOp::And;
    } else if (assign_op == "|=") {
      rmw_op = llvm::AtomicRMWInst::BinOp::Or;
    } else if (assign_op == "^=") {
      rmw_op = llvm::AtomicRMWInst::BinOp::Xor;
    } else {
      return {false, "irbuilder emit: unsupported lock assignment operator " + assign_op};
    }

    builder_.CreateAtomicRMW(rmw_op, lhs.ptr, rhs_value, llvm::MaybeAlign(),
                             llvm::AtomicOrdering::SequentiallyConsistent);
    return {true, ""};
  }

  llvm_backend::Result EmitAtomicIncDec(const HIRExpr& lvalue_expr, bool increment,
                                        FunctionFrame* frame) {
    const LValueResult lhs = EmitLValue(lvalue_expr, frame);
    if (!lhs.ok) {
      return {false, lhs.message};
    }
    if (lhs.pointee_type == nullptr || !lhs.pointee_type->isIntegerTy()) {
      return {false, "irbuilder emit: lock inc/dec requires integer lvalue target"};
    }

    llvm::Value* one = llvm::ConstantInt::get(lhs.pointee_type, 1, true);
    builder_.CreateAtomicRMW(increment ? llvm::AtomicRMWInst::BinOp::Add
                                       : llvm::AtomicRMWInst::BinOp::Sub,
                             lhs.ptr, one, llvm::MaybeAlign(),
                             llvm::AtomicOrdering::SequentiallyConsistent);
    return {true, ""};
  }

  llvm_backend::Result EmitLockStmt(const HIRStmt& st, FunctionFrame* frame) {
    for (const HIRStmt& nested : st.flow_then) {
      if (nested.kind == HIRStmt::Kind::kAssign) {
        HIRExpr lhs;
        lhs.kind = HIRExpr::Kind::kVar;
        lhs.text = nested.name;
        lhs.type = nested.type.empty() ? "I64" : nested.type;
        const llvm_backend::Result result =
            EmitAtomicAssignExpr(lhs, nested.assign_op, nested.expr, frame);
        if (!result.ok) {
          return result;
        }
        continue;
      }

      if (nested.kind == HIRStmt::Kind::kExpr && nested.expr.kind == HIRExpr::Kind::kAssign &&
          nested.expr.children.size() == 2) {
        const llvm_backend::Result result = EmitAtomicAssignExpr(
            nested.expr.children[0], nested.expr.text, nested.expr.children[1], frame);
        if (!result.ok) {
          return result;
        }
        continue;
      }

      if (nested.kind == HIRStmt::Kind::kExpr && nested.expr.kind == HIRExpr::Kind::kPostfix &&
          nested.expr.children.size() == 1 &&
          (nested.expr.text == "++" || nested.expr.text == "--")) {
        const llvm_backend::Result result =
            EmitAtomicIncDec(nested.expr.children[0], nested.expr.text == "++", frame);
        if (!result.ok) {
          return result;
        }
        continue;
      }

      if (nested.kind == HIRStmt::Kind::kExpr && nested.expr.kind == HIRExpr::Kind::kUnary &&
          nested.expr.children.size() == 1 &&
          (nested.expr.text == "++" || nested.expr.text == "--")) {
        const llvm_backend::Result result =
            EmitAtomicIncDec(nested.expr.children[0], nested.expr.text == "++", frame);
        if (!result.ok) {
          return result;
        }
        continue;
      }

      return {false, "irbuilder emit: unsupported statement inside lock block: " +
                         StmtKindName(nested.kind)};
    }
    return {true, ""};
  }

  static bool IsFloatPrintConversion(char conv) {
    return conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' || conv == 'g' ||
           conv == 'G';
  }

  static std::vector<PrintFormatSpec> ParsePrintFormatSpecifiers(std::string_view format,
                                                                  std::string* error) {
    std::vector<PrintFormatSpec> specs;
    std::size_t i = 0;
    while (i < format.size()) {
      if (format[i] != '%') {
        ++i;
        continue;
      }
      if (i + 1 >= format.size()) {
        if (error != nullptr) {
          *error = "dangling '%' in print format string";
        }
        return {};
      }

      ++i;
      if (format[i] == '%') {
        ++i;
        continue;
      }

      while (i < format.size() &&
             (format[i] == '-' || format[i] == '+' || format[i] == ' ' || format[i] == '#' ||
              format[i] == '0' || format[i] == '\'')) {
        ++i;
      }

      PrintFormatSpec spec;
      if (i < format.size() && format[i] == '*') {
        spec.width_from_arg = true;
        ++i;
      }
      while (i < format.size() && std::isdigit(static_cast<unsigned char>(format[i])) != 0) {
        ++i;
      }

      if (i < format.size() && format[i] == '.') {
        ++i;
        if (i < format.size() && format[i] == '*') {
          spec.precision_from_arg = true;
          ++i;
        }
        while (i < format.size() && std::isdigit(static_cast<unsigned char>(format[i])) != 0) {
          ++i;
        }
      }

      while (i < format.size()) {
        const char lm = format[i];
        if (lm == 'h' || lm == 'l' || lm == 'j' || lm == 't' || lm == 'L' || lm == 'q') {
          ++i;
          if ((lm == 'h' || lm == 'l') && i < format.size() && format[i] == lm) {
            ++i;
          }
          continue;
        }
        break;
      }

      if (i >= format.size()) {
        if (error != nullptr) {
          *error = "incomplete print format conversion";
        }
        return {};
      }

      const char conv = format[i++];
      switch (conv) {
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X':
        case 'o':
        case 'b':
        case 'c':
        case 's':
        case 'p':
        case 'P':
        case 'z':
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
          spec.conv = conv;
          specs.push_back(spec);
          break;
        default:
          if (error != nullptr) {
            *error = std::string("unsupported print conversion '%") + conv + "'";
          }
          return {};
      }
    }
    return specs;
  }

  static std::vector<bool> BuildPrintArgFloatMask(const std::vector<PrintFormatSpec>& specs) {
    std::vector<bool> mask;
    for (const PrintFormatSpec& spec : specs) {
      if (spec.width_from_arg) {
        mask.push_back(false);
      }
      if (spec.precision_from_arg) {
        mask.push_back(false);
      }
      if (spec.conv == 'z') {
        mask.push_back(false);
        mask.push_back(false);
        continue;
      }
      mask.push_back(IsFloatPrintConversion(spec.conv));
    }
    return mask;
  }

  llvm_backend::Result EmitPrint(const HIRStmt& st, FunctionFrame* frame) {
    const std::string literal = !st.name.empty() ? st.name : st.print_format.text;
    if (!literal.empty() && literal.front() == '\'') {
      llvm::FunctionCallee callee = module_->getOrInsertFunction(
          "hc_put_char", llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {TypeI64()},
                                                  false));
      builder_.CreateCall(
          callee, {llvm::ConstantInt::get(TypeI64(),
                                          static_cast<std::uint64_t>(ParseCharLiteral(literal)),
                                          true)});
      return {true, ""};
    }

    llvm::Value* format_ptr = nullptr;
    if (!literal.empty() && literal.front() == '"') {
      format_ptr = GetOrCreateStringLiteral(literal);
    } else {
      const ExprResult fmt_expr = EmitExpr(st.print_format, frame);
      if (!fmt_expr.ok) {
        return {false, fmt_expr.message};
      }
      format_ptr = CastIfNeeded(fmt_expr.value, TypePtr());
    }

    if (format_ptr == nullptr) {
      return {false, "irbuilder emit: print format must be pointer-like"};
    }

    std::vector<bool> float_arg_mask(st.print_args.size(), false);
    if (!literal.empty() && literal.front() == '"') {
      std::string format_error;
      const std::vector<PrintFormatSpec> specs =
          ParsePrintFormatSpecifiers(DecodeQuotedString(literal), &format_error);
      if (!format_error.empty()) {
        return {false, "irbuilder emit: " + format_error};
      }
      float_arg_mask = BuildPrintArgFloatMask(specs);
      if (float_arg_mask.size() != st.print_args.size()) {
        return {false, "irbuilder emit: print argument count mismatch for format string"};
      }
    }

    std::vector<llvm::Value*> coerced_args;
    coerced_args.reserve(st.print_args.size());
    for (std::size_t i = 0; i < st.print_args.size(); ++i) {
      const HIRExpr& arg = st.print_args[i];
      const ExprResult value = EmitExpr(arg, frame);
      if (!value.ok) {
        return {false, value.message};
      }
      llvm::Value* as_i64 = PackPrintArg(value.value, float_arg_mask[i]);
      if (as_i64 == nullptr) {
        if (float_arg_mask[i]) {
          return {false, "irbuilder emit: print argument is not float-convertible"};
        }
        return {false, "irbuilder emit: print argument is not integer/pointer-convertible"};
      }
      coerced_args.push_back(as_i64);
    }

    llvm::Value* args_ptr = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(TypePtr()));
    if (!coerced_args.empty()) {
      llvm::ArrayType* arg_array_ty = llvm::ArrayType::get(TypeI64(), coerced_args.size());
      llvm::Value* arg_storage = builder_.CreateAlloca(arg_array_ty, nullptr, "print.args");
      for (std::size_t i = 0; i < coerced_args.size(); ++i) {
        llvm::Value* slot = builder_.CreateInBoundsGEP(
            arg_array_ty, arg_storage,
            {llvm::ConstantInt::get(TypeI64(), 0),
             llvm::ConstantInt::get(TypeI64(), static_cast<std::uint64_t>(i))});
        builder_.CreateStore(coerced_args[i], slot);
      }
      args_ptr = builder_.CreateInBoundsGEP(
          arg_array_ty, arg_storage,
          {llvm::ConstantInt::get(TypeI64(), 0), llvm::ConstantInt::get(TypeI64(), 0)});
    }

    llvm::FunctionCallee fmt_callee = module_->getOrInsertFunction(
        "hc_print_fmt", llvm::FunctionType::get(llvm::Type::getVoidTy(*context_),
                                                 {TypePtr(), TypePtr(), TypeI64()}, false));
    builder_.CreateCall(
        fmt_callee,
        {format_ptr, args_ptr,
         llvm::ConstantInt::get(TypeI64(), static_cast<std::uint64_t>(coerced_args.size()))});
    return {true, ""};
  }

  llvm_backend::Result EmitHostMainWrapper() {
    if (module_->getFunction("main") != nullptr) {
      return {true, ""};
    }

    llvm::Function* holy_main = module_->getFunction("Main");
    if (holy_main == nullptr) {
      return {true, ""};
    }

    llvm::Type* i32 = llvm::Type::getInt32Ty(*context_);
    llvm::FunctionType* main_ty = llvm::FunctionType::get(i32, {i32, TypePtr()}, false);
    llvm::Function* c_main =
        llvm::Function::Create(main_ty, llvm::Function::ExternalLinkage, "main", module_.get());
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context_, "entry", c_main);
    llvm::IRBuilder<> b(entry);
    auto main_arg_it = c_main->arg_begin();
    llvm::Argument* argc_arg = &*main_arg_it++;
    argc_arg->setName("argc");
    llvm::Argument* argv_arg = &*main_arg_it++;
    argv_arg->setName("argv");

    auto cast_for_main = [&](llvm::Value* value, llvm::Type* to_type) -> llvm::Value* {
      if (value == nullptr || to_type == nullptr) {
        return nullptr;
      }
      if (value->getType() == to_type) {
        return value;
      }
      if (value->getType()->isIntegerTy() && to_type->isIntegerTy()) {
        const unsigned from_bits = value->getType()->getIntegerBitWidth();
        const unsigned to_bits = to_type->getIntegerBitWidth();
        if (from_bits == to_bits) {
          return value;
        }
        if (from_bits < to_bits) {
          return b.CreateSExt(value, to_type);
        }
        return b.CreateTrunc(value, to_type);
      }
      if (value->getType()->isPointerTy() && to_type->isPointerTy()) {
        return b.CreateBitCast(value, to_type);
      }
      if (value->getType()->isPointerTy() && to_type->isIntegerTy()) {
        return b.CreatePtrToInt(value, to_type);
      }
      if (value->getType()->isIntegerTy() && to_type->isPointerTy()) {
        return b.CreateIntToPtr(value, to_type);
      }
      return nullptr;
    };

    if (reflection_table_count_ > 0 && reflection_table_ptr_ != nullptr) {
      llvm::FunctionCallee register_fn = module_->getOrInsertFunction(
          "hc_register_reflection_table",
          llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {TypePtr(), TypeI64()}, false));
      b.CreateCall(register_fn,
                   {reflection_table_ptr_,
                    llvm::ConstantInt::get(TypeI64(), reflection_table_count_)});
    }

    if (holy_main->arg_size() > 2) {
      return {false, "irbuilder emit: Main() supports at most two host parameters"};
    }

    std::vector<llvm::Value*> holy_args;
    holy_args.reserve(holy_main->arg_size());
    for (unsigned i = 0; i < holy_main->arg_size(); ++i) {
      llvm::Type* param_ty = holy_main->getFunctionType()->getParamType(i);
      llvm::Value* source = nullptr;
      if (holy_main->arg_size() == 1) {
        source = param_ty->isPointerTy() ? static_cast<llvm::Value*>(argv_arg)
                                         : static_cast<llvm::Value*>(argc_arg);
      } else if (i == 0) {
        source = argc_arg;
      } else {
        source = argv_arg;
      }
      llvm::Value* casted = cast_for_main(source, param_ty);
      if (casted == nullptr) {
        return {false, "irbuilder emit: Main parameter type is not host-call compatible"};
      }
      holy_args.push_back(casted);
    }

    if (holy_main->getReturnType()->isVoidTy()) {
      b.CreateCall(holy_main, holy_args);
      b.CreateRet(llvm::ConstantInt::get(i32, 0));
      return {true, ""};
    }

    if (!holy_main->getReturnType()->isIntegerTy()) {
      return {false, "irbuilder emit: Main return type is not integer/void"};
    }

    llvm::Value* value = b.CreateCall(holy_main, holy_args);
    if (value->getType()->getIntegerBitWidth() > i32->getIntegerBitWidth()) {
      value = b.CreateTrunc(value, i32);
    } else if (value->getType()->getIntegerBitWidth() < i32->getIntegerBitWidth()) {
      value = b.CreateSExt(value, i32);
    }
    b.CreateRet(value);
    return {true, ""};
  }

  llvm::AllocaInst* CreateEntryAlloca(llvm::Function* fn, const std::string& name,
                                      llvm::Type* ty) {
    llvm::IRBuilder<> tmp(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    return tmp.CreateAlloca(ty, nullptr, name);
  }

  static std::string AssignOpToBinary(const std::string& assign_op) {
    if (assign_op == "+=" || assign_op == "-=" || assign_op == "*=" || assign_op == "/=" ||
        assign_op == "%=" || assign_op == "&=" || assign_op == "|=" || assign_op == "^=") {
      return assign_op.substr(0, 1);
    }
    if (assign_op == "<<=") {
      return "<<";
    }
    if (assign_op == ">>=") {
      return ">>";
    }
    return assign_op;
  }

  BinaryResult EmitBinaryOp(const std::string& op, llvm::Value* lhs, llvm::Value* rhs) {
    lhs = CoerceInt64(lhs);
    rhs = CoerceInt64(rhs);
    if (lhs == nullptr || rhs == nullptr) {
      return {false, nullptr, "irbuilder emit: binary op requires integer-convertible operands"};
    }

    if (op == "+") {
      return {true, builder_.CreateAdd(lhs, rhs), ""};
    }
    if (op == "-") {
      return {true, builder_.CreateSub(lhs, rhs), ""};
    }
    if (op == "*") {
      return {true, builder_.CreateMul(lhs, rhs), ""};
    }
    if (op == "/") {
      return {true, builder_.CreateSDiv(lhs, rhs), ""};
    }
    if (op == "%") {
      return {true, builder_.CreateSRem(lhs, rhs), ""};
    }
    if (op == "&") {
      return {true, builder_.CreateAnd(lhs, rhs), ""};
    }
    if (op == "|") {
      return {true, builder_.CreateOr(lhs, rhs), ""};
    }
    if (op == "^") {
      return {true, builder_.CreateXor(lhs, rhs), ""};
    }
    if (op == "<<") {
      return {true, builder_.CreateShl(lhs, rhs), ""};
    }
    if (op == ">>") {
      return {true, builder_.CreateAShr(lhs, rhs), ""};
    }
    if (op == "==") {
      return {true, builder_.CreateZExt(builder_.CreateICmpEQ(lhs, rhs), TypeI64()), ""};
    }
    if (op == "!=") {
      return {true, builder_.CreateZExt(builder_.CreateICmpNE(lhs, rhs), TypeI64()), ""};
    }
    if (op == "<") {
      return {true, builder_.CreateZExt(builder_.CreateICmpSLT(lhs, rhs), TypeI64()), ""};
    }
    if (op == ">") {
      return {true, builder_.CreateZExt(builder_.CreateICmpSGT(lhs, rhs), TypeI64()), ""};
    }
    if (op == "<=") {
      return {true, builder_.CreateZExt(builder_.CreateICmpSLE(lhs, rhs), TypeI64()), ""};
    }
    if (op == ">=") {
      return {true, builder_.CreateZExt(builder_.CreateICmpSGE(lhs, rhs), TypeI64()), ""};
    }
    if (op == "&&") {
      llvm::Value* lhs_bool = ToBool(lhs);
      llvm::Value* rhs_bool = ToBool(rhs);
      if (lhs_bool == nullptr || rhs_bool == nullptr) {
        return {false, nullptr, "irbuilder emit: logical && requires bool-convertible operands"};
      }
      return {true, builder_.CreateZExt(builder_.CreateAnd(lhs_bool, rhs_bool), TypeI64()), ""};
    }
    if (op == "||") {
      llvm::Value* lhs_bool = ToBool(lhs);
      llvm::Value* rhs_bool = ToBool(rhs);
      if (lhs_bool == nullptr || rhs_bool == nullptr) {
        return {false, nullptr, "irbuilder emit: logical || requires bool-convertible operands"};
      }
      return {true, builder_.CreateZExt(builder_.CreateOr(lhs_bool, rhs_bool), TypeI64()), ""};
    }

    return {false, nullptr, "irbuilder emit: unsupported binary operator " + op};
  }

  ConstIntResult EvalConstIntExpr(const HIRExpr& expr) {
    switch (expr.kind) {
      case HIRExpr::Kind::kIntLiteral: {
        std::int64_t value = 0;
        if (!ParseIntegerLiteralText(expr.text, &value)) {
          return {false, 0, "invalid integer literal: " + expr.text};
        }
        return {true, value, ""};
      }

      case HIRExpr::Kind::kUnary: {
        if (expr.children.size() != 1) {
          return {false, 0, "invalid unary expression"};
        }
        ConstIntResult child = EvalConstIntExpr(expr.children[0]);
        if (!child.ok) {
          return child;
        }

        if (expr.text == "+") {
          return child;
        }
        if (expr.text == "-") {
          return {true, -child.value, ""};
        }
        if (expr.text == "~") {
          return {true, ~child.value, ""};
        }
        if (expr.text == "!") {
          return {true, child.value == 0 ? 1 : 0, ""};
        }
        return {false, 0, "unsupported unary operator: " + expr.text};
      }

      case HIRExpr::Kind::kBinary: {
        if (expr.children.size() != 2) {
          return {false, 0, "invalid binary expression"};
        }
        ConstIntResult lhs = EvalConstIntExpr(expr.children[0]);
        if (!lhs.ok) {
          return lhs;
        }
        ConstIntResult rhs = EvalConstIntExpr(expr.children[1]);
        if (!rhs.ok) {
          return rhs;
        }

        const std::string& op = expr.text;
        if (op == "+") {
          return {true, lhs.value + rhs.value, ""};
        }
        if (op == "-") {
          return {true, lhs.value - rhs.value, ""};
        }
        if (op == "*") {
          return {true, lhs.value * rhs.value, ""};
        }
        if (op == "/") {
          if (rhs.value == 0) {
            return {false, 0, "division by zero"};
          }
          return {true, lhs.value / rhs.value, ""};
        }
        if (op == "%") {
          if (rhs.value == 0) {
            return {false, 0, "modulo by zero"};
          }
          return {true, lhs.value % rhs.value, ""};
        }
        if (op == "&") {
          return {true, lhs.value & rhs.value, ""};
        }
        if (op == "|") {
          return {true, lhs.value | rhs.value, ""};
        }
        if (op == "^") {
          return {true, lhs.value ^ rhs.value, ""};
        }
        if (op == "<<") {
          return {true, lhs.value << rhs.value, ""};
        }
        if (op == ">>") {
          return {true, lhs.value >> rhs.value, ""};
        }
        if (op == "==") {
          return {true, lhs.value == rhs.value ? 1 : 0, ""};
        }
        if (op == "!=") {
          return {true, lhs.value != rhs.value ? 1 : 0, ""};
        }
        if (op == "<") {
          return {true, lhs.value < rhs.value ? 1 : 0, ""};
        }
        if (op == ">") {
          return {true, lhs.value > rhs.value ? 1 : 0, ""};
        }
        if (op == "<=") {
          return {true, lhs.value <= rhs.value ? 1 : 0, ""};
        }
        if (op == ">=") {
          return {true, lhs.value >= rhs.value ? 1 : 0, ""};
        }
        if (op == "&&") {
          return {true, (lhs.value != 0 && rhs.value != 0) ? 1 : 0, ""};
        }
        if (op == "||") {
          return {true, (lhs.value != 0 || rhs.value != 0) ? 1 : 0, ""};
        }

        return {false, 0, "unsupported binary operator: " + op};
      }

      case HIRExpr::Kind::kCast: {
        if (expr.children.size() != 1) {
          return {false, 0, "invalid cast expression"};
        }
        return EvalConstIntExpr(expr.children[0]);
      }

      case HIRExpr::Kind::kComma: {
        if (expr.children.empty()) {
          return {false, 0, "invalid empty comma expression"};
        }
        ConstIntResult value;
        for (const HIRExpr& child : expr.children) {
          value = EvalConstIntExpr(child);
          if (!value.ok) {
            return value;
          }
        }
        return value;
      }

      case HIRExpr::Kind::kVar: {
        const auto it = global_constants_.find(expr.text);
        if (it == global_constants_.end() || it->second == nullptr) {
          return {false, 0, "unknown constant variable: " + expr.text};
        }
        if (const auto* as_int = llvm::dyn_cast<llvm::ConstantInt>(it->second)) {
          return {true, as_int->getSExtValue(), ""};
        }
        if (llvm::isa<llvm::ConstantPointerNull>(it->second)) {
          return {true, 0, ""};
        }
        return {false, 0, "constant variable is not integer-like: " + expr.text};
      }

      case HIRExpr::Kind::kStringLiteral:
      case HIRExpr::Kind::kDollar:
      case HIRExpr::Kind::kAssign:
      case HIRExpr::Kind::kCall:
      case HIRExpr::Kind::kPostfix:
      case HIRExpr::Kind::kLane:
      case HIRExpr::Kind::kMember:
      case HIRExpr::Kind::kIndex:
        return {false, 0, "unsupported constant expression kind: " + ExprKindName(expr.kind)};
    }

    return {false, 0, "invalid constant expression"};
  }

  ConstValueResult EvalGlobalConstExpr(const HIRExpr& expr, llvm::Type* target_ty) {
    if (target_ty == nullptr) {
      return {false, nullptr, "missing target type"};
    }

    if (expr.kind == HIRExpr::Kind::kVar) {
      const auto constant_it = global_constants_.find(expr.text);
      if (constant_it != global_constants_.end() && constant_it->second != nullptr) {
        llvm::Constant* value = constant_it->second;
        if (value->getType() == target_ty) {
          return {true, value, ""};
        }
        if (target_ty->isIntegerTy() && value->getType()->isIntegerTy()) {
          if (const auto* as_int = llvm::dyn_cast<llvm::ConstantInt>(value)) {
            return {true,
                    llvm::ConstantInt::get(target_ty,
                                           static_cast<std::uint64_t>(as_int->getSExtValue()),
                                           /*isSigned=*/true),
                    ""};
          }
        }
      }

      const auto global_it = globals_.find(expr.text);
      if (global_it != globals_.end() && global_it->second != nullptr) {
        llvm::Constant* global_ptr = global_it->second;
        if (target_ty->isPointerTy()) {
          if (global_ptr->getType() == target_ty) {
            return {true, global_ptr, ""};
          }
          return {true,
                  llvm::ConstantExpr::getPointerCast(global_ptr,
                                                     llvm::cast<llvm::PointerType>(target_ty)),
                  ""};
        }
        if (target_ty->isIntegerTy()) {
          return {true, llvm::ConstantExpr::getPtrToInt(global_ptr, target_ty), ""};
        }
      }
    }

    if (expr.kind == HIRExpr::Kind::kUnary && expr.text == "&" && expr.children.size() == 1 &&
        expr.children[0].kind == HIRExpr::Kind::kVar) {
      const std::string& base_name = expr.children[0].text;
      const auto global_it = globals_.find(base_name);
      if (global_it == globals_.end() || global_it->second == nullptr) {
        return {false, nullptr, "unknown global in address-of constant expression: " + base_name};
      }
      llvm::Constant* global_ptr = global_it->second;
      if (target_ty->isPointerTy()) {
        if (global_ptr->getType() == target_ty) {
          return {true, global_ptr, ""};
        }
        return {true,
                llvm::ConstantExpr::getPointerCast(global_ptr,
                                                   llvm::cast<llvm::PointerType>(target_ty)),
                ""};
      }
      if (target_ty->isIntegerTy()) {
        return {true, llvm::ConstantExpr::getPtrToInt(global_ptr, target_ty), ""};
      }
      return {false, nullptr, "address-of initializer requires pointer/integer target type"};
    }

    if (expr.kind == HIRExpr::Kind::kStringLiteral) {
      llvm::Value* literal_ptr = GetOrCreateStringLiteral(expr.text);
      auto* literal_const = llvm::dyn_cast<llvm::Constant>(literal_ptr);
      if (literal_const == nullptr) {
        return {false, nullptr, "failed to materialize string literal constant"};
      }

      if (target_ty->isPointerTy()) {
        if (literal_const->getType() == target_ty) {
          return {true, literal_const, ""};
        }
        return {true, llvm::ConstantExpr::getPointerCast(
                          literal_const, llvm::cast<llvm::PointerType>(target_ty)),
                ""};
      }

      if (target_ty->isIntegerTy()) {
        return {true, llvm::ConstantExpr::getPtrToInt(literal_const, target_ty), ""};
      }

      return {false, nullptr, "string literal initializer requires pointer/integer target type"};
    }

    const ConstIntResult as_int = EvalConstIntExpr(expr);
    if (!as_int.ok) {
      return {false, nullptr, as_int.message};
    }

    if (target_ty->isIntegerTy()) {
      return {true,
              llvm::ConstantInt::get(target_ty, static_cast<std::uint64_t>(as_int.value), true),
              ""};
    }

    if (target_ty->isPointerTy()) {
      if (as_int.value == 0) {
        return {true, llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(target_ty)),
                ""};
      }
      llvm::Constant* as_i64 =
          llvm::ConstantInt::get(TypeI64(), static_cast<std::uint64_t>(as_int.value), true);
      return {true,
              llvm::ConstantExpr::getIntToPtr(as_i64, llvm::cast<llvm::PointerType>(target_ty)),
              ""};
    }

    return {false, nullptr, "unsupported constant target type"};
  }

  llvm::Value* CastIfNeeded(llvm::Value* value, llvm::Type* to_type) {
    if (value == nullptr || to_type == nullptr) {
      return nullptr;
    }

    if (value->getType() == to_type) {
      return value;
    }

    if (value->getType()->isIntegerTy() && to_type->isIntegerTy()) {
      const unsigned from_bits = value->getType()->getIntegerBitWidth();
      const unsigned to_bits = to_type->getIntegerBitWidth();
      if (from_bits == to_bits) {
        return value;
      }
      if (from_bits < to_bits) {
        return builder_.CreateSExt(value, to_type);
      }
      return builder_.CreateTrunc(value, to_type);
    }

    if (value->getType()->isPointerTy() && to_type->isPointerTy()) {
      return builder_.CreateBitCast(value, to_type);
    }

    if (value->getType()->isPointerTy() && to_type->isIntegerTy()) {
      return builder_.CreatePtrToInt(value, to_type);
    }

    if (value->getType()->isIntegerTy() && to_type->isPointerTy()) {
      return builder_.CreateIntToPtr(value, to_type);
    }

    return nullptr;
  }

  llvm::Value* CoerceFloat64(llvm::Value* value) {
    if (value == nullptr) {
      return nullptr;
    }

    llvm::Type* f64 = llvm::Type::getDoubleTy(*context_);
    if (value->getType()->isDoubleTy()) {
      return value;
    }
    if (value->getType()->isFloatingPointTy()) {
      return builder_.CreateFPCast(value, f64);
    }
    if (value->getType()->isIntegerTy()) {
      return builder_.CreateSIToFP(value, f64);
    }
    if (value->getType()->isPointerTy()) {
      llvm::Value* as_i64 = builder_.CreatePtrToInt(value, TypeI64());
      return builder_.CreateSIToFP(as_i64, f64);
    }
    return nullptr;
  }

  llvm::Value* CoerceInt64(llvm::Value* value) {
    if (value == nullptr) {
      return nullptr;
    }

    if (value->getType()->isIntegerTy(64)) {
      return value;
    }

    if (value->getType()->isIntegerTy()) {
      return CastIfNeeded(value, TypeI64());
    }

    if (value->getType()->isPointerTy()) {
      return builder_.CreatePtrToInt(value, TypeI64());
    }

    return nullptr;
  }

  llvm::Value* PackPrintArg(llvm::Value* value, bool expect_float) {
    if (expect_float) {
      llvm::Value* as_f64 = CoerceFloat64(value);
      if (as_f64 == nullptr) {
        return nullptr;
      }
      return builder_.CreateBitCast(as_f64, TypeI64());
    }
    return CoerceInt64(value);
  }

  llvm::Value* ToBool(llvm::Value* value) {
    if (value == nullptr) {
      return nullptr;
    }

    if (value->getType()->isIntegerTy(1)) {
      return value;
    }

    if (value->getType()->isIntegerTy()) {
      return builder_.CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0));
    }

    if (value->getType()->isPointerTy()) {
      return builder_.CreateICmpNE(value, llvm::ConstantPointerNull::get(
                                            llvm::cast<llvm::PointerType>(value->getType())));
    }

    return nullptr;
  }

  llvm::Type* TypeI1() { return llvm::Type::getInt1Ty(*context_); }
  llvm::Type* TypeI64() { return llvm::Type::getInt64Ty(*context_); }
  llvm::Type* TypePtr() { return llvm::PointerType::get(*context_, 0); }

  llvm::Type* ToLlvmType(const std::string& holy_type) {
    const std::string normalized = TrimCopy(holy_type);
    if (normalized == "U0") {
      return llvm::Type::getVoidTy(*context_);
    }
    if (normalized == "Bool" || normalized == "Bool(chained)") {
      return llvm::Type::getInt1Ty(*context_);
    }
    if (normalized == "I8" || normalized == "U8") {
      return llvm::Type::getInt8Ty(*context_);
    }
    if (normalized == "I16" || normalized == "U16") {
      return llvm::Type::getInt16Ty(*context_);
    }
    if (normalized == "I32" || normalized == "U32") {
      return llvm::Type::getInt32Ty(*context_);
    }
    if (normalized.find('*') != std::string::npos) {
      return TypePtr();
    }
    const std::string aggregate_name = NormalizeAggregateTypeName(normalized);
    const auto aggregate_it = aggregate_layouts_.find(aggregate_name);
    if (aggregate_it != aggregate_layouts_.end() && aggregate_it->second.type != nullptr) {
      return aggregate_it->second.type;
    }
    return llvm::Type::getInt64Ty(*context_);
  }

  static std::string DecodeQuotedString(const std::string& quoted) {
    if (quoted.size() < 2 || quoted.front() != '"' || quoted.back() != '"') {
      return quoted;
    }

    std::string decoded;
    decoded.reserve(quoted.size());
    for (std::size_t i = 1; i + 1 < quoted.size(); ++i) {
      const char c = quoted[i];
      if (c == '\\' && i + 1 < quoted.size()) {
        const char n = quoted[++i];
        switch (n) {
          case 'n':
            decoded.push_back('\n');
            break;
          case 't':
            decoded.push_back('\t');
            break;
          case 'r':
            decoded.push_back('\r');
            break;
          case '\\':
            decoded.push_back('\\');
            break;
          case '"':
            decoded.push_back('"');
            break;
          default:
            decoded.push_back(n);
            break;
        }
      } else {
        decoded.push_back(c);
      }
    }
    return decoded;
  }

  llvm::Value* GetOrCreateStringLiteral(const std::string& quoted) {
    const auto it = string_literals_.find(quoted);
    if (it != string_literals_.end()) {
      return it->second;
    }

    const std::string decoded = DecodeQuotedString(quoted);

    llvm::Constant* data = llvm::ConstantDataArray::getString(*context_, decoded, true);
    auto* gv = new llvm::GlobalVariable(*module_, data->getType(), true,
                                        llvm::GlobalValue::PrivateLinkage, data,
                                        ".str." + std::to_string(next_string_id_++));
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

    llvm::Constant* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
    llvm::Constant* indices[] = {zero, zero};
    llvm::Constant* ptr = llvm::ConstantExpr::getGetElementPtr(data->getType(), gv, indices);
    string_literals_[quoted] = ptr;
    return ptr;
  }

  static std::int64_t ParseCharLiteral(const std::string& text) {
    if (text.size() < 2 || text.front() != '\'' || text.back() != '\'') {
      return 0;
    }
    const std::string body = text.substr(1, text.size() - 2);
    if (body.empty()) {
      return 0;
    }
    if (body.size() >= 2 && body[0] == '\\') {
      switch (body[1]) {
        case 'n':
          return '\n';
        case 't':
          return '\t';
        case 'r':
          return '\r';
        case '\\':
          return '\\';
        case '\'':
          return '\'';
        default:
          return static_cast<unsigned char>(body[1]);
      }
    }
    return static_cast<unsigned char>(body[0]);
  }

  std::unique_ptr<llvm::LLVMContext> context_;
  std::unique_ptr<llvm::Module> module_;
  llvm::IRBuilder<> builder_;
  std::unordered_map<std::string, llvm::Function*> functions_;
  std::unordered_map<std::string, llvm::GlobalVariable*> globals_;
  std::unordered_map<std::string, llvm::Constant*> global_constants_;
  std::unordered_map<std::string, AggregateLayout> aggregate_layouts_;
  std::unordered_map<std::string, llvm::Constant*> string_literals_;
  llvm::Constant* reflection_table_ptr_ = nullptr;
  std::uint64_t reflection_table_count_ = 0;
  int next_string_id_ = 0;
};

#endif

}  // namespace

llvm_backend::Result EmitIrFromHir(const frontend::internal::HIRModule& module,
                                   std::string_view module_name,
                                   std::string_view target_triple) {
#ifdef HOLYC_LLVM_IRBUILDER_HEADERS_AVAILABLE
  IrBuilderEmitter emitter(module_name, target_triple);
  return emitter.Emit(module);
#else
  (void)module;
  (void)module_name;
  (void)target_triple;
  return {false, "LLVM IRBuilder backend not enabled at build time"};
#endif
}

}  // namespace holyc::llvm_irbuilder_backend

#include "frontend.h"

#include "ast_internal.h"
#include "hir.h"
#include "llvm_irbuilder_backend.h"
#include "parser.h"
#include "preprocessor.h"
#include "sema.h"

#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace holyc::frontend {

namespace {

using internal::HIRExpr;
using internal::HIRFunction;
using internal::HIRModule;
using internal::HIRStmt;
using internal::TypedNode;

std::string HIRExprKindName(HIRExpr::Kind kind) {
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

void DumpNode(const TypedNode& node, int depth, std::ostringstream& out) {
  out << std::string(static_cast<std::size_t>(depth) * 2, ' ') << node.kind;
  if (!node.text.empty()) {
    out << ": " << node.text;
  }
  if (!node.type.empty()) {
    out << " [type=" << node.type << "]";
  }
  out << "\n";
  for (const TypedNode& child : node.children) {
    DumpNode(child, depth + 1, out);
  }
}

std::string HIRStmtKindName(HIRStmt::Kind kind) {
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

void DumpHirExpr(const HIRExpr& expr, int depth, std::ostringstream& out) {
  out << std::string(static_cast<std::size_t>(depth) * 2, ' ') << "Expr("
      << HIRExprKindName(expr.kind) << ")";
  if (!expr.text.empty()) {
    out << ": " << expr.text;
  }
  if (!expr.type.empty()) {
    out << " [type=" << expr.type << "]";
  }
  out << "\n";
  for (const HIRExpr& child : expr.children) {
    DumpHirExpr(child, depth + 1, out);
  }
}

void DumpHirStmt(const HIRStmt& st, int depth, std::ostringstream& out) {
  out << std::string(static_cast<std::size_t>(depth) * 2, ' ') << HIRStmtKindName(st.kind);
  if (!st.name.empty()) {
    out << ": " << st.name;
  }
  if (!st.type.empty()) {
    out << " [type=" << st.type << "]";
  }
  if (st.kind == HIRStmt::Kind::kVarDecl) {
    out << " [storage=" << st.decl_storage << "]";
    if (st.decl_is_global) {
      out << " [global]";
    }
    if (st.decl_has_const_initializer) {
      out << " [const-init]";
    }
  }
  if (st.exception_region_id >= 0) {
    out << " [region=" << st.exception_region_id << "]";
  }
  if (st.exception_parent_region_id >= 0) {
    out << " [parent-region=" << st.exception_parent_region_id << "]";
  }
  out << "\n";

  if (!st.expr.type.empty() || !st.expr.text.empty() || !st.expr.children.empty()) {
    DumpHirExpr(st.expr, depth + 1, out);
  }
  if (!st.print_format.type.empty() || !st.print_format.text.empty() ||
      !st.print_format.children.empty()) {
    DumpHirExpr(st.print_format, depth + 1, out);
  }
  for (const HIRExpr& arg : st.print_args) {
    DumpHirExpr(arg, depth + 1, out);
  }
  if (!st.flow_cond.type.empty() || !st.flow_cond.text.empty() || !st.flow_cond.children.empty()) {
    DumpHirExpr(st.flow_cond, depth + 1, out);
  }
  if (!st.switch_cond.type.empty() || !st.switch_cond.text.empty() ||
      !st.switch_cond.children.empty()) {
    DumpHirExpr(st.switch_cond, depth + 1, out);
  }
  for (const HIRStmt& item : st.flow_then) {
    DumpHirStmt(item, depth + 1, out);
  }
  for (const HIRStmt& item : st.flow_else) {
    DumpHirStmt(item, depth + 1, out);
  }
  for (const HIRStmt& item : st.try_body) {
    DumpHirStmt(item, depth + 1, out);
  }
  for (const HIRStmt& item : st.catch_body) {
    DumpHirStmt(item, depth + 1, out);
  }
  for (const std::vector<HIRStmt>& body : st.switch_case_bodies) {
    for (const HIRStmt& item : body) {
      DumpHirStmt(item, depth + 1, out);
    }
  }
  for (const HIRStmt& item : st.switch_default) {
    DumpHirStmt(item, depth + 1, out);
  }
}

void DumpHirModule(const HIRModule& module, std::ostringstream& out) {
  out << "HIRModule\n";
  for (const HIRStmt& item : module.top_level_items) {
    DumpHirStmt(item, 1, out);
  }
  for (const HIRFunction& fn : module.functions) {
    out << "  Function: " << fn.name << " -> " << fn.return_type << "\n";
    for (const auto& param : fn.params) {
      out << "    Param: " << param.first << " " << param.second << "\n";
    }
    for (const HIRStmt& st : fn.body) {
      DumpHirStmt(st, 2, out);
    }
  }

  out << "  Reflection\n";
  for (const std::string& alias : module.reflection.type_aliases) {
    out << "    TypeAlias: " << alias << "\n";
  }
  for (const auto& field : module.reflection.fields) {
    out << "    Field: " << field.aggregate_name << "." << field.field_name << " : "
        << field.field_type;
    if (!field.annotations.empty()) {
      out << " [meta=";
      for (std::size_t i = 0; i < field.annotations.size(); ++i) {
        if (i > 0) {
          out << ",";
        }
        out << field.annotations[i];
      }
      out << "]";
    }
    out << "\n";
  }
}

template <typename Fn>
auto RunTimedPhase(std::string_view phase_name, std::vector<PhaseTiming>* phase_timings, Fn&& fn)
    -> decltype(fn()) {
  const auto start = std::chrono::steady_clock::now();
  auto result = fn();
  const auto end = std::chrono::steady_clock::now();
  if (phase_timings != nullptr) {
    phase_timings->push_back(PhaseTiming{
        std::string(phase_name),
        std::chrono::duration<double>(end - start).count(),
    });
  }
  return result;
}

}  // namespace

ParseResult ParseAndDumpAst(std::string_view source, std::string_view filename,
                            ExecutionMode mode, bool strict_mode,
                            std::vector<PhaseTiming>* phase_timings) {
  try {
    const std::string preprocessed = RunTimedPhase(
        "preprocess", phase_timings,
        [&]() { return internal::RunPreprocessor(source, filename, mode); });

    const internal::ParsedNode parsed =
        RunTimedPhase("parse", phase_timings,
                      [&]() { return internal::ParseAst(preprocessed, filename); });
    const TypedNode root =
        RunTimedPhase("sema", phase_timings, [&]() {
          return internal::AnalyzeSemantics(parsed, filename, strict_mode);
        });

    std::ostringstream out;
    RunTimedPhase("ast-dump", phase_timings, [&]() {
      DumpNode(root, 0, out);
      return 0;
    });
    return ParseResult{true, out.str()};
  } catch (const std::exception& ex) {
    return ParseResult{false, ex.what()};
  }
}

ParseResult PreprocessSource(std::string_view source, std::string_view filename,
                             ExecutionMode mode, bool /*strict_mode*/,
                             std::vector<PhaseTiming>* phase_timings) {
  try {
    const std::string preprocessed = RunTimedPhase(
        "preprocess", phase_timings,
        [&]() { return internal::RunPreprocessor(source, filename, mode); });
    return ParseResult{true, preprocessed};
  } catch (const std::exception& ex) {
    return ParseResult{false, ex.what()};
  }
}

ParseResult CheckSource(std::string_view source, std::string_view filename,
                        ExecutionMode mode, bool strict_mode,
                        std::vector<PhaseTiming>* phase_timings) {
  try {
    const std::string preprocessed = RunTimedPhase(
        "preprocess", phase_timings,
        [&]() { return internal::RunPreprocessor(source, filename, mode); });
    const internal::ParsedNode parsed =
        RunTimedPhase("parse", phase_timings,
                      [&]() { return internal::ParseAst(preprocessed, filename); });
    (void)RunTimedPhase("sema", phase_timings, [&]() {
      return internal::AnalyzeSemantics(parsed, filename, strict_mode);
    });
    return ParseResult{true, "ok\n"};
  } catch (const std::exception& ex) {
    return ParseResult{false, ex.what()};
  }
}

ParseResult EmitHir(std::string_view source, std::string_view filename, ExecutionMode mode,
                    bool strict_mode, std::vector<PhaseTiming>* phase_timings) {
  try {
    const std::string preprocessed = RunTimedPhase(
        "preprocess", phase_timings,
        [&]() { return internal::RunPreprocessor(source, filename, mode); });
    const internal::ParsedNode parsed =
        RunTimedPhase("parse", phase_timings,
                      [&]() { return internal::ParseAst(preprocessed, filename); });
    const TypedNode root =
        RunTimedPhase("sema", phase_timings, [&]() {
          return internal::AnalyzeSemantics(parsed, filename, strict_mode);
        });
    const HIRModule module =
        RunTimedPhase("hir-lower", phase_timings,
                      [&]() { return internal::LowerToHir(root, filename); });

    std::ostringstream out;
    RunTimedPhase("hir-dump", phase_timings, [&]() {
      DumpHirModule(module, out);
      return 0;
    });
    return ParseResult{true, out.str()};
  } catch (const std::exception& ex) {
    return ParseResult{false, ex.what()};
  }
}

ParseResult EmitLlvmIr(std::string_view source, std::string_view filename,
                       ExecutionMode mode, bool strict_mode,
                       std::vector<PhaseTiming>* phase_timings) {
  try {
    const std::string preprocessed = RunTimedPhase(
        "preprocess", phase_timings,
        [&]() { return internal::RunPreprocessor(source, filename, mode); });

    const internal::ParsedNode parsed =
        RunTimedPhase("parse", phase_timings,
                      [&]() { return internal::ParseAst(preprocessed, filename); });
    const TypedNode root =
        RunTimedPhase("sema", phase_timings, [&]() {
          return internal::AnalyzeSemantics(parsed, filename, strict_mode);
        });

    const HIRModule module =
        RunTimedPhase("hir-lower", phase_timings,
                      [&]() { return internal::LowerToHir(root, filename); });

    const llvm_backend::Result irbuilder = RunTimedPhase(
        "llvm-emit", phase_timings,
        [&]() { return llvm_irbuilder_backend::EmitIrFromHir(module, "holyc"); });
    if (irbuilder.ok) {
      return ParseResult{true, irbuilder.output};
    }
    return ParseResult{false, irbuilder.output};
  } catch (const std::exception& ex) {
    return ParseResult{false, ex.what()};
  }
}

}  // namespace holyc::frontend

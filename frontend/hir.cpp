#include "hir.h"

#include "diagnostics.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace holyc::frontend::internal {

namespace {
using Node = TypedNode;

struct ParamSig {
  std::string type;
  std::string name;
  bool has_default;
  Node default_expr;
};

struct FunctionSig {
  std::string return_type;
  std::string name;
  std::vector<ParamSig> params;
  std::string linkage_kind = "external";
  bool imported = false;
};

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

bool IsStringLiteralText(std::string_view text) {
  const std::string trimmed = TrimCopy(text);
  return trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"';
}

std::string InlineAsmConstraintText(std::string_view text) {
  std::string constraint = TrimCopy(text);
  if (constraint.size() >= 2 && constraint.front() == '"' && constraint.back() == '"') {
    constraint = constraint.substr(1, constraint.size() - 2);
  }
  return constraint;
}

bool InlineAsmConstraintNeedsOperand(std::string_view text) {
  const std::string constraint = InlineAsmConstraintText(text);
  if (constraint.empty()) {
    return false;
  }
  if (constraint.front() == '=' || constraint.front() == '~') {
    return false;
  }
  if (constraint.size() >= 3 && constraint.front() == '{' && constraint.back() == '}') {
    return false;
  }
  return true;
}

std::string NormalizeLastClassTypeName(std::string type_name) {
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
  return type_name.empty() ? "I64" : type_name;
}

bool IsLastClassDefaultExpr(const Node& expr) {
  return expr.kind == "Identifier" && expr.text == "lastclass";
}

std::string QuoteStringLiteral(std::string_view text) {
  std::string out = "\"";
  out.reserve(text.size() + 2);
  for (char c : text) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  out.push_back('"');
  return out;
}

std::string StripDeclModifiers(std::string_view decl_text) {
  static const std::unordered_set<std::string> kCompatModifiers = {
      "public", "interrupt", "noreg", "reg", "no_warn",
      "static", "extern", "import", "_extern", "_import", "export", "_export"};
  std::istringstream stream{std::string(decl_text)};
  std::string token;
  std::vector<std::string> kept;
  while (stream >> token) {
    if (kCompatModifiers.find(token) != kCompatModifiers.end()) {
      continue;
    }
    kept.push_back(token);
  }
  std::ostringstream out;
  for (std::size_t i = 0; i < kept.size(); ++i) {
    if (i > 0) {
      out << ' ';
    }
    out << kept[i];
  }
  return out.str();
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

std::pair<std::string, std::string> ParseTypedNameFromNode(const Node& node) {
  std::string decl_type;
  std::string decl_name;
  for (const Node& child : node.children) {
    if (child.kind == "DeclType") {
      decl_type = child.text;
    } else if (child.kind == "DeclName") {
      decl_name = child.text;
    }
  }
  if (!decl_name.empty()) {
    return {decl_type, decl_name};
  }
  return ParseTypedName(node.text);
}

const Node* FindChildByKind(const Node& node, std::string_view kind) {
  const auto it = std::find_if(node.children.begin(), node.children.end(),
                               [kind](const Node& child) { return child.kind == kind; });
  if (it != node.children.end()) {
    return &(*it);
  }
  return nullptr;
}

const Node* FindVarInitializer(const Node& node) {
  for (const Node& child : node.children) {
    if (child.kind == "DeclType" || child.kind == "DeclName") {
      continue;
    }
    return &child;
  }
  return nullptr;
}

bool HasDeclModifier(std::string_view decl_text, std::string_view modifier) {
  std::istringstream stream(TrimCopy(decl_text));
  std::string token;
  while (stream >> token) {
    if (token == modifier) {
      return true;
    }
  }
  return false;
}

std::string ResolveFunctionLinkageKind(std::string_view decl_text) {
  if (HasDeclModifier(decl_text, "static")) {
    return "internal";
  }
  return "external";
}

class HIRLowerer {
 public:
  explicit HIRLowerer(std::string_view filename) : filename_(filename) {}

  HIRModule LowerModule(const Node& program) {
    CollectFunctionSignatures(program);

    HIRModule module;

    for (const Node& child : program.children) {
      if (child.kind == "FunctionDecl") {
        if (FindChildByKind(child, "Block") == nullptr) {
          continue;
        }
        module.functions.push_back(LowerFunction(child));
        continue;
      }

      // Keep non-function top-level forms in HIR so unsupported backend
      // paths fail explicitly instead of silently dropping semantic input.
      if (child.kind == "ClassDecl") {
        CollectClassReflection(child, &module.reflection);
        HIRStmt hs;
        hs.kind = HIRStmt::Kind::kMetadataDecl;
        hs.metadata_name = child.text;
        for (const Node& meta : child.children) {
          if (meta.kind == "VarDecl") {
            continue;
          }
          hs.metadata_payload.push_back(meta.text);
        }
        module.top_level_items.push_back(std::move(hs));

        for (const Node& trailing : child.children) {
          if (trailing.kind == "VarDecl") {
            LowerStmt(trailing, &module.top_level_items, true);
          }
        }
        continue;
      }

      if (child.kind == "TypeAliasDecl") {
        module.reflection.type_aliases.push_back(child.text);
        HIRStmt hs;
        hs.kind = HIRStmt::Kind::kMetadataDecl;
        hs.metadata_name = "typedef";
        hs.metadata_payload.push_back(child.text);
        module.top_level_items.push_back(std::move(hs));
        continue;
      }

      if (child.kind == "LinkageDecl") {
        HIRStmt hs;
        hs.kind = HIRStmt::Kind::kLinkageDecl;
        hs.linkage_kind = child.text;
        if (!child.children.empty()) {
          hs.linkage_symbol = child.children[0].text;
        }
        module.top_level_items.push_back(std::move(hs));
        continue;
      }

      if (child.kind == "ExprStmt" && !child.children.empty() &&
          child.children[0].kind == "Identifier") {
        const std::string& directive = child.children[0].text;
        if (directive == "extern" || directive == "import" || directive == "_extern" ||
            directive == "_import" || directive == "export" || directive == "_export") {
          HIRStmt hs;
          hs.kind = HIRStmt::Kind::kLinkageDecl;
          hs.linkage_kind = directive;
          module.top_level_items.push_back(std::move(hs));
          continue;
        }
      }

      if (child.kind == "StartLabel" || child.kind == "EndLabel") {
        HIRStmt hs;
        hs.kind = HIRStmt::Kind::kMetadataDecl;
        hs.metadata_name = child.kind;
        module.top_level_items.push_back(std::move(hs));
        continue;
      }

      LowerStmt(child, &module.top_level_items, true);
    }

    module.function_decls.reserve(function_order_.size());
    for (const std::string& fn_name : function_order_) {
      const auto it = functions_.find(fn_name);
      if (it == functions_.end()) {
        continue;
      }
      HIRFunctionDecl decl;
      decl.name = it->second.name;
      decl.return_type = it->second.return_type;
      decl.linkage_kind = it->second.linkage_kind;
      decl.params.reserve(it->second.params.size());
      for (const ParamSig& param : it->second.params) {
        decl.params.emplace_back(param.type, param.name);
      }
      module.function_decls.push_back(std::move(decl));
    }
    return module;
  }

 private:
  void CollectFunctionSignatures(const Node& program) {
    functions_.clear();
    function_order_.clear();
    for (const Node& child : program.children) {
      if (child.kind != "FunctionDecl") {
        continue;
      }

      const auto [ret_ty, fn_name] = ParseTypedNameFromNode(child);
      if (fn_name.empty()) {
        Error("invalid function declaration in lowering: " + child.text);
      }

      FunctionSig sig;
      const std::string normalized_ret_ty = StripDeclModifiers(ret_ty);
      sig.return_type = normalized_ret_ty.empty() ? "I64" : normalized_ret_ty;
      sig.name = fn_name;
      sig.linkage_kind = ResolveFunctionLinkageKind(ret_ty);
      sig.imported = HasDeclModifier(ret_ty, "import") || HasDeclModifier(ret_ty, "_import");

      if (const Node* params = FindChildByKind(child, "ParamList"); params != nullptr) {
        for (const Node& p : params->children) {
          const auto [param_ty, param_name] = ParseTypedNameFromNode(p);
          if (param_name.empty()) {
            Error("invalid function parameter in lowering: " + p.text);
          }
          const Node* default_expr = FindChildByKind(p, "Default");
          const std::string normalized_param_ty = StripDeclModifiers(param_ty);
          Node lowered_default_expr;
          if (default_expr != nullptr) {
            if (default_expr->children.empty()) {
              Error("invalid default argument expression in lowering: " + p.text);
            }
            lowered_default_expr = default_expr->children[0];
          }
          sig.params.push_back(ParamSig{
              normalized_param_ty.empty() ? "I64" : normalized_param_ty,
              param_name,
              default_expr != nullptr,
              std::move(lowered_default_expr),
          });
        }
      }

      const auto it = functions_.find(sig.name);
      if (it == functions_.end()) {
        functions_[sig.name] = sig;
        function_order_.push_back(sig.name);
      } else {
        if (it->second.return_type != sig.return_type ||
            it->second.params.size() != sig.params.size()) {
          Error("conflicting function declaration in lowering: " + sig.name);
        }
        for (std::size_t i = 0; i < sig.params.size(); ++i) {
          if (it->second.params[i].type != sig.params[i].type ||
              it->second.params[i].name != sig.params[i].name) {
            Error("conflicting function declaration in lowering: " + sig.name);
          }
        }
        if (it->second.linkage_kind != sig.linkage_kind &&
            (it->second.linkage_kind == "internal" || sig.linkage_kind == "internal")) {
          Error("conflicting function linkage in lowering: " + sig.name);
        }
      }

      const bool has_body = FindChildByKind(child, "Block") != nullptr;
      if (has_body && sig.imported) {
        Error("import linkage function cannot have a definition in lowering: " + sig.name);
      }
    }
  }

  HIRFunction LowerFunction(const Node& fn) {
    const auto [ret_ty, fn_name] = ParseTypedNameFromNode(fn);
    if (fn_name.empty()) {
      Error("invalid function in HIR lowering: " + fn.text);
    }

    HIRFunction out;
    out.name = fn_name;
    const std::string normalized_ret_ty = StripDeclModifiers(ret_ty);
    out.return_type = normalized_ret_ty.empty() ? "I64" : normalized_ret_ty;
    const auto sig_it = functions_.find(fn_name);
    if (sig_it != functions_.end()) {
      out.linkage_kind = sig_it->second.linkage_kind;
    } else {
      out.linkage_kind = ResolveFunctionLinkageKind(ret_ty);
    }
    next_exception_region_id_ = 1;
    exception_region_stack_.clear();

    if (const Node* params = FindChildByKind(fn, "ParamList"); params != nullptr) {
      for (const Node& p : params->children) {
        const auto [p_ty, p_name] = ParseTypedNameFromNode(p);
        if (p_name.empty()) {
          Error("invalid parameter in HIR lowering: " + p.text);
        }
        const std::string normalized_param_ty = StripDeclModifiers(p_ty);
        out.params.emplace_back(normalized_param_ty.empty() ? "I64" : normalized_param_ty,
                                p_name);
      }
    }

    const Node* body = FindChildByKind(fn, "Block");
    if (body == nullptr) {
      Error("missing function body in HIR lowering: " + fn.text);
    }

    for (const Node& stmt : body->children) {
      LowerStmt(stmt, &out.body);
    }
    return out;
  }

  void LowerStmt(const Node& stmt, std::vector<HIRStmt>* out, bool top_level = false) {
    if (stmt.kind == "EmptyStmt") {
      return;
    }

    if (stmt.kind == "VarDeclList") {
      for (const Node& child : stmt.children) {
        if (child.kind == "VarDecl") {
          LowerStmt(child, out, top_level);
        }
      }
      return;
    }

    if (stmt.kind == "LockStmt") {
      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kLock;
      for (const Node& child : stmt.children) {
        LowerStmt(child, &hs.flow_then);
      }
      out->push_back(std::move(hs));
      return;
    }

    if (stmt.kind == "VarDecl") {
      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kVarDecl;
      const auto [ty, name] = ParseTypedNameFromNode(stmt);
      const std::string normalized_decl_ty = StripDeclModifiers(ty);
      hs.type = normalized_decl_ty.empty() ? "I64" : normalized_decl_ty;
      hs.name = name;
      hs.decl_is_global = top_level;
      const bool is_static = HasDeclModifier(stmt.text, "static");
      if (top_level) {
        hs.decl_storage = is_static ? "static-global" : "global";
      } else {
        hs.decl_storage = is_static ? "static-local" : "local";
      }
      const Node* init = FindVarInitializer(stmt);
      hs.decl_has_const_initializer = init != nullptr && IsConstInitializerExpr(*init);
      if (init != nullptr) {
        hs.expr = LowerExpr(*init);
      }
      out->push_back(std::move(hs));
      return;
    }

    if (stmt.kind == "ReturnStmt") {
      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kReturn;
      hs.type = stmt.type;
      if (!stmt.children.empty()) {
        hs.expr = LowerExpr(stmt.children[0]);
      }
      out->push_back(std::move(hs));
      return;
    }

    if (stmt.kind == "BreakStmt") {
      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kBreak;
      out->push_back(std::move(hs));
      return;
    }

    if (stmt.kind == "ThrowStmt") {
      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kThrow;
      hs.exception_region_id =
          exception_region_stack_.empty() ? -1 : exception_region_stack_.back();
      if (!stmt.children.empty()) {
        hs.expr = LowerExpr(stmt.children[0]);
      }
      out->push_back(std::move(hs));
      return;
    }

    if (stmt.kind == "AsmStmt") {
      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kInlineAsm;
      hs.asm_template = stmt.text;

      if (!stmt.children.empty()) {
        const Node& template_arg = stmt.children[0];
        if (!template_arg.children.empty()) {
          hs.asm_template = template_arg.children[0].text;
        }

        bool awaiting_operand = false;
        for (std::size_t i = 1; i < stmt.children.size(); ++i) {
          const Node& arg = stmt.children[i];
          if (arg.children.empty()) {
            Error("invalid inline asm argument in HIR lowering");
          }
          const Node& arg_expr = arg.children[0];
          if (IsStringLiteralText(arg_expr.text)) {
            if (awaiting_operand) {
              Error("inline asm input constraint requires operand in HIR lowering: " +
                    InlineAsmConstraintText(hs.asm_constraints.back()));
            }
            hs.asm_constraints.push_back(arg_expr.text);
            hs.asm_operands.push_back(HIRExpr{});
            hs.asm_operand_present.push_back(false);
            awaiting_operand = InlineAsmConstraintNeedsOperand(arg_expr.text);
            continue;
          }

          if (!awaiting_operand || hs.asm_constraints.empty()) {
            Error("inline asm operand must follow input constraint in HIR lowering");
          }
          const std::size_t idx = hs.asm_constraints.size() - 1;
          hs.asm_operands[idx] = LowerExpr(arg_expr);
          hs.asm_operand_present[idx] = true;
          awaiting_operand = false;
        }

        if (awaiting_operand) {
          Error("inline asm input constraint requires operand in HIR lowering: " +
                InlineAsmConstraintText(hs.asm_constraints.back()));
        }
      }
      out->push_back(std::move(hs));
      return;
    }

    if (stmt.kind == "TryStmt") {
      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kTryCatch;
      hs.exception_parent_region_id =
          exception_region_stack_.empty() ? -1 : exception_region_stack_.back();
      hs.exception_region_id = next_exception_region_id_++;
      exception_region_stack_.push_back(hs.exception_region_id);
      if (!stmt.children.empty()) {
        LowerStmt(stmt.children[0], &hs.try_body);
      }
      exception_region_stack_.pop_back();
      if (stmt.children.size() > 1) {
        if (hs.exception_parent_region_id >= 0) {
          exception_region_stack_.push_back(hs.exception_parent_region_id);
          LowerStmt(stmt.children[1], &hs.catch_body);
          exception_region_stack_.pop_back();
        } else {
          LowerStmt(stmt.children[1], &hs.catch_body);
        }
      }
      out->push_back(std::move(hs));
      return;
    }

    if (stmt.kind == "IfStmt") {
      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kIf;
      if (!stmt.children.empty()) {
        hs.flow_cond = LowerExpr(stmt.children[0]);
      }
      if (stmt.children.size() > 1) {
        LowerStmt(stmt.children[1], &hs.flow_then);
      }
      if (stmt.children.size() > 2) {
        LowerStmt(stmt.children[2], &hs.flow_else);
      }
      out->push_back(std::move(hs));
      return;
    }

    if (stmt.kind == "WhileStmt") {
      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kWhile;
      if (!stmt.children.empty()) {
        hs.flow_cond = LowerExpr(stmt.children[0]);
      }
      if (stmt.children.size() > 1) {
        LowerStmt(stmt.children[1], &hs.flow_then);
      }
      out->push_back(std::move(hs));
      return;
    }

    if (stmt.kind == "DoWhileStmt") {
      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kDoWhile;
      if (!stmt.children.empty()) {
        LowerStmt(stmt.children[0], &hs.flow_then);
      }
      if (stmt.children.size() > 1) {
        hs.flow_cond = LowerExpr(stmt.children[1]);
      }
      out->push_back(std::move(hs));
      return;
    }

    if (stmt.kind == "ForStmt") {
      if (!stmt.children.empty() && stmt.children[0].kind != "Init") {
        LowerExprAsStmt(stmt.children[0], out);
      }

      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kWhile;
      if (stmt.children.size() > 1 && stmt.children[1].kind != "Cond") {
        hs.flow_cond = LowerExpr(stmt.children[1]);
      } else {
        hs.flow_cond = HIRExpr{HIRExpr::Kind::kIntLiteral, "1", {}, "I64"};
      }

      if (stmt.children.size() > 3) {
        LowerStmt(stmt.children[3], &hs.flow_then);
      }

      if (stmt.children.size() > 2 && stmt.children[2].kind != "Inc") {
        LowerExprAsStmt(stmt.children[2], &hs.flow_then);
      }

      out->push_back(std::move(hs));
      return;
    }

    if (stmt.kind == "SwitchStmt") {
      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kSwitch;
      if (!stmt.children.empty()) {
        hs.switch_cond = LowerExpr(stmt.children[0]);
      }

      if (stmt.children.size() > 1 && stmt.children[1].kind == "Block") {
        int current_case = -1;
        for (const Node& item : stmt.children[1].children) {
          if (item.kind == "CaseClause") {
            int flags = 0;
            int64_t begin = 0;
            int64_t end = 0;

            if (item.text == "null-case") {
              flags |= 1;
            } else if (item.text == "range-case") {
              flags |= 2;
            }

            if (!item.children.empty()) {
              if ((flags & 1) == 0) {
                begin = ParseConstIntExpr(item.children[0]);
                end = begin;
              }
              if ((flags & 2) != 0 && item.children.size() > 1) {
                end = ParseConstIntExpr(item.children[1]);
              }
            }

            hs.switch_case_flags.push_back(flags);
            hs.switch_case_begin.push_back(begin);
            hs.switch_case_end.push_back(end);
            hs.switch_case_bodies.push_back({});
            current_case = static_cast<int>(hs.switch_case_bodies.size()) - 1;

            if (!item.children.empty()) {
              const Node& first_stmt = item.children.back();
              LowerStmt(first_stmt,
                        &hs.switch_case_bodies[static_cast<std::size_t>(current_case)]);
            }
            continue;
          }

          if (item.kind == "DefaultClause") {
            if (!item.children.empty()) {
              LowerStmt(item.children[0], &hs.switch_default);
            }
            continue;
          }

          if (current_case >= 0) {
            LowerStmt(item, &hs.switch_case_bodies[static_cast<std::size_t>(current_case)]);
          } else {
            LowerStmt(item, &hs.switch_default);
          }
        }
      }

      out->push_back(std::move(hs));
      return;
    }

    if (stmt.kind == "LabelStmt") {
      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kLabel;
      hs.label_name = stmt.text;
      out->push_back(std::move(hs));
      if (!stmt.children.empty()) {
        LowerStmt(stmt.children[0], out);
      }
      return;
    }

    if (stmt.kind == "GotoStmt") {
      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kGoto;
      hs.goto_target = stmt.text;
      out->push_back(std::move(hs));
      return;
    }

    if (stmt.kind == "ClassDecl") {
      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kMetadataDecl;
      hs.metadata_name = stmt.text;
      for (const Node& meta : stmt.children) {
        hs.metadata_payload.push_back(meta.text);
      }
      out->push_back(std::move(hs));
      return;
    }

    if (stmt.kind == "TypeAliasDecl") {
      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kMetadataDecl;
      hs.metadata_name = "typedef";
      hs.metadata_payload.push_back(stmt.text);
      out->push_back(std::move(hs));
      return;
    }

    if (stmt.kind == "LinkageDecl") {
      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kLinkageDecl;
      hs.linkage_kind = stmt.text;
      if (!stmt.children.empty()) {
        hs.linkage_symbol = stmt.children[0].text;
      }
      out->push_back(std::move(hs));
      return;
    }

    if (stmt.kind == "NoParenCallStmt") {
      if (stmt.children.empty() || stmt.children[0].kind != "Identifier") {
        Error("invalid no-paren call statement");
      }
      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kNoParenCall;
      hs.name = stmt.children[0].text;
      hs.type = stmt.type;
      out->push_back(std::move(hs));
      return;
    }

    if (stmt.kind == "PrintStmt") {
      if (stmt.children.empty()) {
        Error("invalid print statement in lowering");
      }
      std::size_t format_index = 0;
      std::size_t arg_begin = 1;
      if (stmt.children.size() > 1 && stmt.children[0].kind == "Literal" &&
          TrimCopy(stmt.children[0].text) == "\"\"") {
        // Normalize HolyC dynamic-format forwarding form: `"" fmt,*args`.
        format_index = 1;
        arg_begin = 2;
      }
      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kPrint;
      hs.print_format = LowerExpr(stmt.children[format_index]);
      if (stmt.children[format_index].kind == "Literal") {
        hs.name = stmt.children[format_index].text;
      }
      hs.print_args.reserve(stmt.children.size() > arg_begin ? stmt.children.size() - arg_begin
                                                              : 0);
      for (std::size_t i = arg_begin; i < stmt.children.size(); ++i) {
        hs.print_args.push_back(LowerExpr(stmt.children[i]));
      }
      out->push_back(std::move(hs));
      return;
    }

    if (stmt.kind == "ExprStmt") {
      if (stmt.children.empty()) {
        return;
      }

      const Node& expr = stmt.children[0];

      if (expr.kind == "AssignExpr" && expr.children.size() == 2 &&
          expr.children[0].kind == "Identifier") {
        HIRStmt hs;
        hs.kind = HIRStmt::Kind::kAssign;
        hs.name = expr.children[0].text;
        hs.assign_op = expr.text;
        hs.expr = LowerExpr(expr.children[1]);
        hs.type = expr.type;
        out->push_back(std::move(hs));
        return;
      }

      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kExpr;
      hs.expr = LowerExpr(expr);
      hs.type = stmt.type;
      out->push_back(std::move(hs));
      return;
    }

    if (stmt.kind == "Block") {
      for (const Node& child : stmt.children) {
        LowerStmt(child, out);
      }
      return;
    }

    if (stmt.kind == "StartLabel" || stmt.kind == "EndLabel") {
      // These are parser markers for HolyC switch compatibility; they are not
      // directly emitted as executable statements in the current lowering model.
      return;
    }

    Error("unsupported statement in lowering: " + stmt.kind);
  }

  HIRExpr LowerExpr(const Node& expr) {
    if (expr.kind == "Literal") {
      if (!expr.text.empty() && std::isdigit(static_cast<unsigned char>(expr.text[0])) != 0) {
        return HIRExpr{HIRExpr::Kind::kIntLiteral, expr.text, {}, "I64"};
      }
      if (!expr.text.empty() && expr.text.front() == '\'') {
        return HIRExpr{HIRExpr::Kind::kIntLiteral,
                       std::to_string(ParseCharLiteralToInt(expr.text)), {}, "I64"};
      }
      if (!expr.text.empty() && expr.text.front() == '"') {
        return HIRExpr{HIRExpr::Kind::kStringLiteral, expr.text, {}, "U8*"};
      }
      Error("unsupported literal in LLVM lowering: " + expr.text);
    }

    if (expr.kind == "DollarExpr") {
      return HIRExpr{HIRExpr::Kind::kDollar, expr.text.empty() ? "$" : expr.text, {}, "I64"};
    }

    if (expr.kind == "Identifier") {
      return HIRExpr{HIRExpr::Kind::kVar, expr.text, {}, expr.type.empty() ? "I64" : expr.type};
    }

    if (expr.kind == "AssignExpr") {
      if (expr.children.size() != 2) {
        Error("invalid assignment expression in lowering");
      }
      HIRExpr lhs = LowerExpr(expr.children[0]);
      HIRExpr rhs = LowerExpr(expr.children[1]);
      return HIRExpr{HIRExpr::Kind::kAssign, expr.text, {std::move(lhs), std::move(rhs)},
                     expr.type.empty() ? "I64" : expr.type};
    }

    if (expr.kind == "UnaryExpr") {
      if (expr.children.size() != 1) {
        Error("invalid unary expression in lowering");
      }
      HIRExpr operand = LowerExpr(expr.children[0]);
      return HIRExpr{HIRExpr::Kind::kUnary, expr.text, {std::move(operand)},
                     expr.type.empty() ? "I64" : expr.type};
    }

    if (expr.kind == "BinaryExpr") {
      if (expr.children.size() != 2) {
        Error("invalid binary expression in lowering");
      }
      HIRExpr lhs = LowerExpr(expr.children[0]);
      HIRExpr rhs = LowerExpr(expr.children[1]);
      return HIRExpr{HIRExpr::Kind::kBinary, expr.text, {std::move(lhs), std::move(rhs)},
                     expr.type.empty() ? "I64" : expr.type};
    }

    if (expr.kind == "CastExpr") {
      if (expr.children.size() != 1) {
        Error("invalid cast expression in lowering");
      }
      HIRExpr value = LowerExpr(expr.children[0]);
      return HIRExpr{HIRExpr::Kind::kCast, expr.text, {std::move(value)},
                     expr.type.empty() ? "I64" : expr.type};
    }

    if (expr.kind == "PostfixExpr") {
      if (expr.children.size() != 1) {
        Error("invalid postfix expression in lowering");
      }
      HIRExpr operand = LowerExpr(expr.children[0]);
      return HIRExpr{HIRExpr::Kind::kPostfix, expr.text, {std::move(operand)},
                     expr.type.empty() ? "I64" : expr.type};
    }

    if (expr.kind == "LaneExpr") {
      if (expr.children.size() != 2) {
        Error("invalid lane expression in lowering");
      }
      HIRExpr base = LowerExpr(expr.children[0]);
      HIRExpr index = LowerExpr(expr.children[1]);
      return HIRExpr{HIRExpr::Kind::kLane, expr.text, {std::move(base), std::move(index)},
                     expr.type.empty() ? "I64" : expr.type};
    }

    if (expr.kind == "MemberExpr") {
      if (expr.children.size() != 1) {
        Error("invalid member expression in lowering");
      }
      HIRExpr base = LowerExpr(expr.children[0]);
      return HIRExpr{HIRExpr::Kind::kMember, expr.text, {std::move(base)},
                     expr.type.empty() ? "I64" : expr.type};
    }

    if (expr.kind == "IndexExpr") {
      if (expr.children.size() != 2) {
        Error("invalid index expression in lowering");
      }
      HIRExpr base = LowerExpr(expr.children[0]);
      HIRExpr index = LowerExpr(expr.children[1]);
      return HIRExpr{HIRExpr::Kind::kIndex, expr.text, {std::move(base), std::move(index)},
                     expr.type.empty() ? "I64" : expr.type};
    }

    if (expr.kind == "CallExpr") {
      if (expr.children.size() < 2) {
        Error("invalid call expression in lowering");
      }
      if (expr.children[1].kind != "CallArgs") {
        Error("invalid call argument list in lowering");
      }

      const Node& callee_expr = expr.children[0];
      auto fn_it = functions_.end();
      bool direct_call = false;
      if (callee_expr.kind == "Identifier") {
        fn_it = functions_.find(callee_expr.text);
        if (fn_it != functions_.end()) {
          direct_call = true;
        } else {
          const std::string callee_ty = TrimCopy(callee_expr.type);
          const bool typed_callable_pointer =
              callee_ty.find('*') != std::string::npos || callee_ty.rfind("fn ", 0) == 0;
          if (!typed_callable_pointer) {
            FunctionSig synthesized;
            synthesized.name = callee_expr.text;
            synthesized.return_type = expr.type.empty() ? "I64" : expr.type;
            synthesized.linkage_kind = "external";
            std::size_t arg_idx = 0;
            for (const Node& arg : expr.children[1].children) {
              if (arg.kind == "EmptyArg") {
                Error("cannot synthesize signature for default-argument call: " +
                      callee_expr.text);
              }
              const std::string arg_ty = arg.type.empty() ? "I64" : StripDeclModifiers(arg.type);
              synthesized.params.push_back(
                  ParamSig{arg_ty.empty() ? "I64" : arg_ty, "p" + std::to_string(arg_idx++), false,
                           Node{}});
            }
            functions_[callee_expr.text] = std::move(synthesized);
            function_order_.push_back(callee_expr.text);
            fn_it = functions_.find(callee_expr.text);
            direct_call = true;
          }
        }
      }

      if (!direct_call) {
        HIRExpr call;
        call.kind = HIRExpr::Kind::kCall;
        call.text.clear();
        call.type = expr.type.empty() ? "I64" : expr.type;
        call.children.push_back(LowerExpr(callee_expr));
        for (const Node& arg : expr.children[1].children) {
          if (arg.kind == "EmptyArg") {
            Error("indirect call does not support sparse/default arguments");
          }
          call.children.push_back(LowerExpr(arg));
        }
        return call;
      }

      const std::string fn_name = callee_expr.text;
      const FunctionSig& sig = fn_it->second;

      HIRExpr call;
      call.kind = HIRExpr::Kind::kCall;
      call.text = fn_name;
      call.type = expr.type.empty() ? "I64" : expr.type;
      std::vector<std::string> resolved_arg_types;
      resolved_arg_types.reserve(sig.params.size());

      std::size_t param_idx = 0;
      for (const Node& arg : expr.children[1].children) {
        if (param_idx >= sig.params.size()) {
          Error("too many arguments in lowering call: " + call.text);
        }

        if (arg.kind == "EmptyArg") {
          if (!sig.params[param_idx].has_default) {
            Error("missing default argument during lowering for function: " + call.text);
          }

          if (IsLastClassDefaultExpr(sig.params[param_idx].default_expr)) {
            if (param_idx == 0 || resolved_arg_types.empty()) {
              Error("lastclass default requires a previous argument type: " + call.text);
            }
            const std::string lastclass =
                NormalizeLastClassTypeName(resolved_arg_types[param_idx - 1]);
            call.children.push_back(
                HIRExpr{HIRExpr::Kind::kStringLiteral, QuoteStringLiteral(lastclass), {}, "U8*"});
          } else {
            call.children.push_back(LowerExpr(sig.params[param_idx].default_expr));
          }

          resolved_arg_types.push_back(sig.params[param_idx].type);
          ++param_idx;
          continue;
        }

        call.children.push_back(LowerExpr(arg));
        resolved_arg_types.push_back(arg.type.empty() ? sig.params[param_idx].type : arg.type);
        ++param_idx;
      }

      while (param_idx < sig.params.size()) {
        if (!sig.params[param_idx].has_default) {
          Error("missing required trailing argument during lowering for function: " + call.text);
        }
        if (IsLastClassDefaultExpr(sig.params[param_idx].default_expr)) {
          if (param_idx == 0 || resolved_arg_types.empty()) {
            Error("lastclass default requires a previous argument type: " + call.text);
          }
          const std::string lastclass =
              NormalizeLastClassTypeName(resolved_arg_types[param_idx - 1]);
          call.children.push_back(
              HIRExpr{HIRExpr::Kind::kStringLiteral, QuoteStringLiteral(lastclass), {}, "U8*"});
        } else {
          call.children.push_back(LowerExpr(sig.params[param_idx].default_expr));
        }
        resolved_arg_types.push_back(sig.params[param_idx].type);
        ++param_idx;
      }
      return call;
    }

    if (expr.kind == "CommaExpr") {
      if (expr.children.empty()) {
        Error("invalid empty comma expression in lowering");
      }
      HIRExpr out;
      out.kind = HIRExpr::Kind::kComma;
      out.text = ",";
      out.type = expr.type.empty() ? "I64" : expr.type;
      out.children.reserve(expr.children.size());
      for (const Node& child : expr.children) {
        out.children.push_back(LowerExpr(child));
      }
      return out;
    }

    Error("unsupported expression in LLVM lowering: " + expr.kind);
  }

  [[noreturn]] void Error(const std::string& msg) const {
    ThrowDiagnostic(Diagnostic{
        "HC4001",
        DiagnosticSeverity::kError,
        filename_,
        0,
        0,
        msg,
        "",
    });
  }

  static int64_t ParseConstIntExpr(const Node& node) {
    if (node.kind != "Literal") {
      throw std::runtime_error("switch case requires literal constants in M8");
    }
    if (!node.text.empty() && node.text.front() == '\'') {
      return ParseCharLiteralToInt(node.text);
    }
    errno = 0;
    char* end = nullptr;
    const long long parsed_signed = std::strtoll(node.text.c_str(), &end, 0);
    if (errno == 0 && end != node.text.c_str() && *end == '\0') {
      return static_cast<int64_t>(parsed_signed);
    }

    errno = 0;
    end = nullptr;
    const unsigned long long parsed_unsigned = std::strtoull(node.text.c_str(), &end, 0);
    if (errno == 0 && end != node.text.c_str() && *end == '\0') {
      return static_cast<int64_t>(parsed_unsigned);
    }

    throw std::runtime_error("invalid integer literal: " + node.text);
  }

  static int64_t ParseCharLiteralToInt(const std::string& text) {
    if (text.size() < 2 || text.front() != '\'' || text.back() != '\'') {
      throw std::runtime_error("invalid char literal: " + text);
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

  static bool IsConstInitializerExpr(const Node& node) {
    if (node.kind == "Literal") {
      return true;
    }
    if (node.kind == "UnaryExpr") {
      return node.children.size() == 1 && IsConstInitializerExpr(node.children[0]);
    }
    if (node.kind == "BinaryExpr") {
      return node.children.size() == 2 && IsConstInitializerExpr(node.children[0]) &&
             IsConstInitializerExpr(node.children[1]);
    }
    if (node.kind == "CastExpr") {
      return node.children.size() == 1 && IsConstInitializerExpr(node.children[0]);
    }
    if (node.kind == "CommaExpr") {
      return !node.children.empty() &&
             std::all_of(node.children.begin(), node.children.end(),
                         [](const Node& child) { return IsConstInitializerExpr(child); });
    }
    return false;
  }

  static std::vector<std::string> SplitWhitespace(std::string_view text) {
    std::istringstream stream{std::string(text)};
    std::vector<std::string> out;
    std::string token;
    while (stream >> token) {
      out.push_back(token);
    }
    return out;
  }

  static void CollectClassReflection(const Node& class_node, HIRReflectionTable* table) {
    const auto [_, class_name] = ParseTypedName(class_node.text);
    if (class_name.empty()) {
      return;
    }

    for (const Node& field : class_node.children) {
      if (field.kind != "FieldDecl") {
        continue;
      }
      const auto [field_type, field_name] = ParseTypedNameFromNode(field);
      if (field_name.empty()) {
        continue;
      }

      HIRReflectionField entry;
      entry.aggregate_name = class_name;
      entry.field_name = field_name;
      const std::string normalized_field_ty = StripDeclModifiers(field_type);
      entry.field_type = normalized_field_ty.empty() ? "I64" : normalized_field_ty;
      for (const Node& child : field.children) {
        if (child.kind == "FieldMetaTokens") {
          entry.annotations = SplitWhitespace(child.text);
        }
      }
      table->fields.push_back(std::move(entry));
    }
  }

  void LowerExprAsStmt(const Node& expr, std::vector<HIRStmt>* out) {
    if (expr.kind == "AssignExpr" && expr.children.size() == 2 &&
        expr.children[0].kind == "Identifier") {
      HIRStmt hs;
      hs.kind = HIRStmt::Kind::kAssign;
      hs.name = expr.children[0].text;
      hs.assign_op = expr.text;
      hs.expr = LowerExpr(expr.children[1]);
      hs.type = expr.type;
      out->push_back(std::move(hs));
      return;
    }

    HIRStmt hs;
    hs.kind = HIRStmt::Kind::kExpr;
    hs.expr = LowerExpr(expr);
    hs.type = expr.type;
    out->push_back(std::move(hs));
  }

  std::string filename_;
  std::unordered_map<std::string, FunctionSig> functions_;
  std::vector<std::string> function_order_;
  int next_exception_region_id_ = 1;
  std::vector<int> exception_region_stack_;
};

}  // namespace

HIRModule LowerToHir(const TypedNode& program, std::string_view filename) {
  HIRLowerer lowerer(filename);
  return lowerer.LowerModule(program);
}

}  // namespace holyc::frontend::internal

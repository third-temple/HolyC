#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast_internal.h"

namespace holyc::frontend::internal {

struct HIRExpr {
  enum class Kind {
    kIntLiteral,
    kStringLiteral,
    kDollar,
    kVar,
    kAssign,
    kUnary,
    kBinary,
    kCall,
    kCast,
    kPostfix,
    kLane,
    kMember,
    kIndex,
    kComma,
  };

  Kind kind = Kind::kIntLiteral;
  std::string text;
  std::vector<HIRExpr> children;
  std::string type;
};

struct HIRStmt {
  enum class Kind {
    kVarDecl,
    kAssign,
    kReturn,
    kExpr,
    kNoParenCall,
    kPrint,
    kLock,
    kThrow,
    kTryCatch,
    kBreak,
    kSwitch,
    kIf,
    kWhile,
    kDoWhile,
    kLabel,
    kGoto,
    kInlineAsm,
    kMetadataDecl,
    kLinkageDecl,
  };

  Kind kind = Kind::kExpr;
  std::string name;
  std::string type;
  std::string decl_storage = "auto";
  bool decl_is_global = false;
  bool decl_has_const_initializer = false;
  std::string assign_op = "=";
  HIRExpr expr;
  HIRExpr print_format;
  std::vector<HIRExpr> print_args;
  std::vector<HIRStmt> try_body;
  std::vector<HIRStmt> catch_body;
  HIRExpr switch_cond;
  std::vector<int64_t> switch_case_begin;
  std::vector<int64_t> switch_case_end;
  std::vector<int> switch_case_flags;
  std::vector<std::vector<HIRStmt>> switch_case_bodies;
  std::vector<HIRStmt> switch_default;
  HIRExpr flow_cond;
  std::vector<HIRStmt> flow_then;
  std::vector<HIRStmt> flow_else;
  std::string label_name;
  std::string goto_target;
  std::string asm_template;
  std::vector<std::string> asm_constraints;
  std::vector<HIRExpr> asm_operands;
  std::vector<bool> asm_operand_present;
  std::string metadata_name;
  std::vector<std::string> metadata_payload;
  std::string linkage_kind;
  std::string linkage_symbol;
  int exception_region_id = -1;
  int exception_parent_region_id = -1;
};

struct HIRFunction {
  std::string name;
  std::string return_type;
  std::string linkage_kind = "external";
  std::vector<std::pair<std::string, std::string>> params;
  std::vector<HIRStmt> body;
};

struct HIRFunctionDecl {
  std::string name;
  std::string return_type;
  std::string linkage_kind = "external";
  std::vector<std::pair<std::string, std::string>> params;
};

struct HIRReflectionField {
  std::string aggregate_name;
  std::string field_name;
  std::string field_type;
  std::vector<std::string> annotations;
};

struct HIRReflectionTable {
  std::vector<std::string> type_aliases;
  std::vector<HIRReflectionField> fields;
};

struct HIRModule {
  std::vector<HIRStmt> top_level_items;
  std::vector<HIRFunction> functions;
  std::vector<HIRFunctionDecl> function_decls;
  HIRReflectionTable reflection;
};

HIRModule LowerToHir(const TypedNode& program, std::string_view filename);

}  // namespace holyc::frontend::internal

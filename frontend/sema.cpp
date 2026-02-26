#include "sema.h"

#include "diagnostics.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace holyc::frontend::internal {

namespace {
using Node = TypedNode;

Node ConvertToTyped(const ParsedNode& in) {
  Node out;
  out.kind = in.kind;
  out.text = in.text;
  out.line = in.line;
  out.column = in.column;
  out.children.reserve(in.children.size());
  for (const ParsedNode& child : in.children) {
    out.children.push_back(ConvertToTyped(child));
  }
  return out;
}

struct ParamSig {
  std::string type;
  std::string name;
  bool has_default;
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

bool IsRelationalOp(std::string_view op) {
  return op == "<" || op == ">" || op == "<=" || op == ">=";
}

class SemanticAnalyzer {
 public:
  explicit SemanticAnalyzer(std::string_view filename, bool strict_mode)
      : filename_(filename), strict_mode_(strict_mode) {}

  void Analyze(Node* program) {
    if (program->kind != "Program") {
      Error("internal semantic error: expected program node");
    }

    BootstrapTempleOsBuiltins();
    CollectFunctionSignatures(*program);
    CollectGlobalSymbols(*program);

    for (Node& child : program->children) {
      AnalyzeTopLevel(child);
    }
  }

 private:
  enum class ValueKind {
    kUnknown,
    kBool,
    kInt,
    kUInt,
    kFloat,
    kPointer,
  };

  struct TypeInfo {
    ValueKind kind = ValueKind::kUnknown;
    int bits = 0;
  };

  struct LabelInfo {
    int index = 0;
    int depth = 0;
  };

  struct GotoInfo {
    std::string target;
    int index = 0;
    int depth = 0;
  };

  struct InitDeclInfo {
    std::string name;
    int index = 0;
    int depth = 0;
  };

  [[noreturn]] void Error(std::string_view msg) const {
    ThrowDiagnostic(Diagnostic{
        "HC3001",
        DiagnosticSeverity::kError,
        filename_,
        0,
        0,
        std::string("semantic error: ") + std::string(msg),
        "",
    });
  }

  static std::string TrimSpaces(const std::string& in) {
    std::size_t begin = 0;
    std::size_t end = in.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(in[begin])) != 0) {
      ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(in[end - 1])) != 0) {
      --end;
    }
    return in.substr(begin, end - begin);
  }

  static bool HasPointerMarker(const std::string& type_name) {
    return type_name.find('*') != std::string::npos;
  }

  static std::string AddPointerLevel(std::string base_type) {
    base_type = TrimSpaces(base_type);
    if (base_type.empty()) {
      return "I64*";
    }
    if (base_type.back() == '*') {
      return base_type + "*";
    }
    return base_type + " *";
  }

  static std::string RemovePointerLevel(std::string pointer_type) {
    pointer_type = TrimSpaces(pointer_type);
    const std::size_t star = pointer_type.rfind('*');
    if (star == std::string::npos) {
      return pointer_type;
    }
    return TrimSpaces(pointer_type.substr(0, star));
  }

  static std::size_t EstimateTypeSize(std::string_view type_name) {
    const TypeInfo info = ParseTypeInfo(type_name);
    if (info.kind == ValueKind::kPointer || info.kind == ValueKind::kUnknown) {
      return 8;
    }
    if (info.kind == ValueKind::kFloat) {
      return 8;
    }
    if (info.bits <= 8) {
      return 1;
    }
    if (info.bits <= 16) {
      return 2;
    }
    if (info.bits <= 32) {
      return 4;
    }
    return 8;
  }

  static std::string NormalizeAggregateTypeName(std::string type_name) {
    type_name = TrimSpaces(type_name);
    while (!type_name.empty() && type_name.back() == '*') {
      type_name = TrimSpaces(type_name.substr(0, type_name.size() - 1));
    }
    if (type_name.rfind("class ", 0) == 0) {
      type_name = TrimSpaces(type_name.substr(6));
    } else if (type_name.rfind("union ", 0) == 0) {
      type_name = TrimSpaces(type_name.substr(6));
    }
    return type_name;
  }

  static TypeInfo ParseTypeInfo(std::string_view type_name) {
    const std::string ty = TrimSpaces(std::string(type_name));
    if (ty.empty()) {
      return TypeInfo{};
    }

    if (HasPointerMarker(ty)) {
      return TypeInfo{ValueKind::kPointer, 64};
    }
    if (ty == "Bool" || ty == "Bool(chained)") {
      return TypeInfo{ValueKind::kBool, 1};
    }
    if (ty == "F64") {
      return TypeInfo{ValueKind::kFloat, 64};
    }
    if (ty == "I8") {
      return TypeInfo{ValueKind::kInt, 8};
    }
    if (ty == "U8") {
      return TypeInfo{ValueKind::kUInt, 8};
    }
    if (ty == "I16") {
      return TypeInfo{ValueKind::kInt, 16};
    }
    if (ty == "U16") {
      return TypeInfo{ValueKind::kUInt, 16};
    }
    if (ty == "I32") {
      return TypeInfo{ValueKind::kInt, 32};
    }
    if (ty == "U32") {
      return TypeInfo{ValueKind::kUInt, 32};
    }
    if (ty == "I64") {
      return TypeInfo{ValueKind::kInt, 64};
    }
    if (ty == "U64") {
      return TypeInfo{ValueKind::kUInt, 64};
    }
    return TypeInfo{};
  }

  static std::string LaneElementType(std::string_view lane_name) {
    const std::string lane = TrimSpaces(std::string(lane_name));
    if (lane == "i8" || lane == "I8") {
      return "I8";
    }
    if (lane == "u8" || lane == "U8") {
      return "U8";
    }
    if (lane == "i16" || lane == "I16") {
      return "I16";
    }
    if (lane == "u16" || lane == "U16") {
      return "U16";
    }
    if (lane == "i32" || lane == "I32") {
      return "I32";
    }
    if (lane == "u32" || lane == "U32") {
      return "U32";
    }
    if (lane == "i64" || lane == "I64") {
      return "I64";
    }
    if (lane == "u64" || lane == "U64") {
      return "U64";
    }
    return "";
  }

  static int LaneElementBits(std::string_view lane_name) {
    const std::string lane = TrimSpaces(std::string(lane_name));
    if (lane == "i8" || lane == "I8" || lane == "u8" || lane == "U8") {
      return 8;
    }
    if (lane == "i16" || lane == "I16" || lane == "u16" || lane == "U16") {
      return 16;
    }
    if (lane == "i32" || lane == "I32" || lane == "u32" || lane == "U32") {
      return 32;
    }
    if (lane == "i64" || lane == "I64" || lane == "u64" || lane == "U64") {
      return 64;
    }
    return 0;
  }

  static bool TryParseIntLiteral(const Node& node, std::int64_t* out) {
    if (out == nullptr || node.kind != "Literal") {
      return false;
    }
    if (node.text.empty() || node.text.front() == '"' || node.text.front() == '\'') {
      return false;
    }
    try {
      *out = std::stoll(node.text, nullptr, 0);
      return true;
    } catch (...) {
      return false;
    }
  }

  static bool IsNumeric(TypeInfo info) {
    return info.kind == ValueKind::kBool || info.kind == ValueKind::kInt ||
           info.kind == ValueKind::kUInt || info.kind == ValueKind::kFloat;
  }

  static bool IsIntegralLike(TypeInfo info) {
    return info.kind == ValueKind::kBool || info.kind == ValueKind::kInt ||
           info.kind == ValueKind::kUInt;
  }

  static bool IsThrowable(TypeInfo info) {
    // Current throw/catch runtime path transports an integer payload.
    return info.kind == ValueKind::kUnknown || IsIntegralLike(info);
  }

  static bool IsStringLiteralText(std::string_view text) {
    const std::string trimmed = TrimCopy(text);
    return trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"';
  }

  static std::string InlineAsmConstraintText(std::string_view text) {
    std::string constraint = TrimCopy(text);
    if (constraint.size() >= 2 && constraint.front() == '"' && constraint.back() == '"') {
      constraint = constraint.substr(1, constraint.size() - 2);
    }
    return constraint;
  }

  static bool InlineAsmConstraintNeedsOperand(std::string_view text) {
    const std::string constraint = InlineAsmConstraintText(text);
    if (constraint.empty()) {
      return false;
    }
    if (constraint.front() == '=' || constraint.front() == '~') {
      return false;
    }

    // HolyC historical style allows raw "{reg}" strings as clobber-like entries.
    if (constraint.size() >= 3 && constraint.front() == '{' && constraint.back() == '}') {
      return false;
    }

    return true;
  }

  static bool IsCharLiteralText(std::string_view text) {
    const std::string trimmed = TrimCopy(text);
    return trimmed.size() >= 3 && trimmed.front() == '\'' && trimmed.back() == '\'';
  }

  struct PrintFormatSpec {
    char conv = '\0';
    bool width_from_arg = false;
    bool precision_from_arg = false;
  };

  static std::vector<PrintFormatSpec> CollectPrintFormatSpecifiers(std::string_view format_literal,
                                                                   std::string* error) {
    std::vector<PrintFormatSpec> specs;
    const std::string text = TrimCopy(std::string(format_literal));
    if (!IsStringLiteralText(text)) {
      if (error != nullptr) {
        *error = "print format must be a string literal";
      }
      return specs;
    }

    const std::size_t stop = text.size() - 1;
    std::size_t i = 1;
    while (i < stop) {
      const char c = text[i];
      if (c == '\\') {
        i += (i + 1 < stop) ? 2 : 1;
        continue;
      }
      if (c != '%') {
        ++i;
        continue;
      }

      if (i + 1 >= stop) {
        if (error != nullptr) {
          *error = "dangling '%' in print format string";
        }
        return {};
      }

      ++i;
      if (text[i] == '%') {
        ++i;
        continue;
      }

      while (i < stop &&
             (text[i] == '-' || text[i] == '+' || text[i] == ' ' || text[i] == '#' ||
              text[i] == '0' || text[i] == '\'')) {
        ++i;
      }

      PrintFormatSpec spec;
      if (i < stop && text[i] == '*') {
        spec.width_from_arg = true;
        ++i;
      }
      while (i < stop && std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
        ++i;
      }

      if (i < stop && text[i] == '.') {
        ++i;
        if (i < stop && text[i] == '*') {
          spec.precision_from_arg = true;
          ++i;
        }
        while (i < stop && std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
          ++i;
        }
      }

      while (i < stop) {
        const char lm = text[i];
        if (lm == 'h' || lm == 'l' || lm == 'j' || lm == 't' || lm == 'L' ||
            lm == 'q') {
          ++i;
          if ((lm == 'h' || lm == 'l') && i < stop && text[i] == lm) {
            ++i;
          }
          continue;
        }
        break;
      }

      if (i >= stop) {
        if (error != nullptr) {
          *error = "incomplete print format conversion";
        }
        return {};
      }

      const char conv = text[i++];
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

  static bool PrintSpecifierAcceptsType(char spec, TypeInfo arg_info) {
    if (arg_info.kind == ValueKind::kUnknown) {
      return true;
    }

    switch (spec) {
      case 'd':
      case 'i':
      case 'c':
        return IsIntegralLike(arg_info);
      case 'u':
      case 'x':
      case 'X':
      case 'o':
      case 'b':
        return IsIntegralLike(arg_info) || arg_info.kind == ValueKind::kPointer;
      case 'p':
      case 'P':
        return IsIntegralLike(arg_info) || arg_info.kind == ValueKind::kPointer;
      case 'z':
        return IsIntegralLike(arg_info) || arg_info.kind == ValueKind::kPointer;
      case 's':
        return arg_info.kind == ValueKind::kPointer;
      case 'f':
      case 'F':
      case 'e':
      case 'E':
      case 'g':
      case 'G':
        return IsNumeric(arg_info);
      default:
        return false;
    }
  }

  static std::string PromoteIntegerResultType(std::string_view lhs_type,
                                              std::string_view rhs_type) {
    const TypeInfo lhs = ParseTypeInfo(lhs_type);
    const TypeInfo rhs = ParseTypeInfo(rhs_type);
    if (!IsIntegralLike(lhs) || !IsIntegralLike(rhs)) {
      return "I64";
    }

    // HolyC execution is 64-bit centric; normalize integer math to 64-bit while
    // preserving unsigned intent when present.
    if (lhs.kind == ValueKind::kUInt || rhs.kind == ValueKind::kUInt) {
      return "U64";
    }
    return "I64";
  }

  static bool CanImplicitConvert(std::string_view from_type, std::string_view to_type) {
    const TypeInfo from = ParseTypeInfo(from_type);
    const TypeInfo to = ParseTypeInfo(to_type);

    if (from.kind == ValueKind::kUnknown || to.kind == ValueKind::kUnknown) {
      return true;
    }
    if (from.kind == to.kind) {
      return true;
    }
    if (IsNumeric(from) && IsNumeric(to)) {
      return true;
    }
    const bool from_integral =
        from.kind == ValueKind::kBool || from.kind == ValueKind::kInt ||
        from.kind == ValueKind::kUInt;
    const bool to_integral =
        to.kind == ValueKind::kBool || to.kind == ValueKind::kInt || to.kind == ValueKind::kUInt;
    if ((from.kind == ValueKind::kPointer && to_integral) ||
        (to.kind == ValueKind::kPointer && from_integral)) {
      return true;
    }
    return false;
  }

  static bool SameSignature(const FunctionSig& a, const FunctionSig& b) {
    if (a.return_type != b.return_type || a.params.size() != b.params.size()) {
      return false;
    }
    for (std::size_t i = 0; i < a.params.size(); ++i) {
      if (a.params[i].type != b.params[i].type || a.params[i].name != b.params[i].name) {
        return false;
      }
    }
    return true;
  }

  static std::string TrimTrailingPointerMarkers(std::string type_name) {
    type_name = TrimCopy(type_name);
    while (!type_name.empty() && type_name.back() == '*') {
      type_name.pop_back();
      type_name = TrimCopy(type_name);
    }
    return type_name;
  }

  static std::string InferCallReturnTypeFromCalleeType(std::string_view callee_type) {
    const std::string normalized = TrimCopy(std::string(callee_type));
    if (normalized.rfind("fn ", 0) == 0) {
      const std::string ret = TrimTrailingPointerMarkers(normalized.substr(3));
      return ret.empty() ? "I64" : ret;
    }

    std::smatch match;
    static const std::regex kFunctionPointerType(R"(^(.*)\(\s*[*&].*\)\s*\(.*\)\s*$)");
    if (std::regex_match(normalized, match, kFunctionPointerType)) {
      const std::string ret = TrimCopy(match[1].str());
      return ret.empty() ? "I64" : ret;
    }

    if (ParseTypeInfo(normalized).kind == ValueKind::kPointer) {
      return "I64";
    }
    return normalized.empty() ? "I64" : normalized;
  }

  static std::pair<std::string, std::string> ParseTypedNameFromNode(const Node& node) {
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

  static const Node* FindChildByKind(const Node& node, std::string_view kind) {
    const auto it = std::find_if(node.children.begin(), node.children.end(),
                                 [kind](const Node& child) { return child.kind == kind; });
    if (it != node.children.end()) {
      return &(*it);
    }
    return nullptr;
  }

  static const Node* FindVarInitializer(const Node& node) {
    const auto it = std::find_if(node.children.begin(), node.children.end(), [](const Node& child) {
      return child.kind != "DeclType" && child.kind != "DeclName";
    });
    if (it != node.children.end()) {
      return &(*it);
    }
    return nullptr;
  }

  static bool HasDeclModifier(std::string_view decl_text, std::string_view modifier) {
    std::istringstream stream(TrimCopy(std::string(decl_text)));
    std::string token;
    while (stream >> token) {
      if (token == modifier) {
        return true;
      }
    }
    return false;
  }

  static bool IsImportLinkage(std::string_view decl_text) {
    return HasDeclModifier(decl_text, "import") || HasDeclModifier(decl_text, "_import");
  }

  static std::string ResolveFunctionLinkageKind(std::string_view decl_text) {
    if (HasDeclModifier(decl_text, "static")) {
      return "internal";
    }
    return "external";
  }

  static std::string StripDeclModifiers(std::string_view decl_text) {
    static const std::unordered_set<std::string> kCompatModifiers = {
        "public", "interrupt", "noreg", "reg", "no_warn",
        "static", "extern", "import", "_extern", "_import", "export", "_export"};
    std::istringstream stream(TrimCopy(std::string(decl_text)));
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

  static bool IsPermissiveOnlyModifier(std::string_view token) {
    static const std::unordered_set<std::string> kPermissiveOnlyModifiers = {
        "public", "interrupt", "noreg", "reg", "no_warn", "_extern", "_import", "_export"};
    return kPermissiveOnlyModifiers.find(std::string(token)) != kPermissiveOnlyModifiers.end();
  }

  void ValidateDeclModifiers(std::string_view decl_text, std::string_view context) const {
    if (!strict_mode_) {
      return;
    }
    std::istringstream stream(TrimCopy(std::string(decl_text)));
    std::string token;
    while (stream >> token) {
      if (IsPermissiveOnlyModifier(token)) {
        Error("strict mode rejects compatibility modifier '" + token + "' in " +
              std::string(context) + "; pass --permissive to enable it");
      }
    }
  }

  void ValidateLinkageKind(std::string_view linkage_kind, std::string_view context) const {
    if (!strict_mode_) {
      return;
    }
    if (linkage_kind == "_extern" || linkage_kind == "_import" || linkage_kind == "_export") {
      Error("strict mode rejects compatibility linkage '" + std::string(linkage_kind) + "' in " +
            std::string(context) + "; pass --permissive to enable it");
    }
  }

  static bool IsStatementNodeKind(std::string_view kind) {
    if (kind == "VarDecl" || kind == "VarDeclList" || kind == "Block" || kind == "CaseClause" ||
        kind == "DefaultClause" || kind == "LabelStmt" || kind == "TypeAliasDecl" ||
        kind == "LinkageDecl" || kind == "ClassDecl" || kind == "EmptyStmt") {
      return true;
    }
    if (kind.size() >= 4 && kind.substr(kind.size() - 4) == "Stmt") {
      return true;
    }
    return false;
  }

  void BootstrapTempleOsBuiltins() {
    auto add_builtin_global = [this](const std::string& name, const std::string& type) {
      global_symbols_.try_emplace(name, type);
    };

    auto add_builtin_function = [this](const std::string& name, const std::string& ret_ty,
                                       std::vector<ParamSig> params) {
      if (functions_.find(name) != functions_.end()) {
        return;
      }
      FunctionSig sig;
      sig.return_type = ret_ty;
      sig.name = name;
      sig.params = std::move(params);
      functions_[name] = std::move(sig);
    };

    add_builtin_global("TRUE", "Bool");
    add_builtin_global("FALSE", "Bool");
    add_builtin_global("NULL", "U8*");
    add_builtin_global("YorN", "Bool");
    add_builtin_global("tS", "F64");
    add_builtin_global("RED", "I64");
    add_builtin_global("HTT_CLASS", "I64");
    add_builtin_global("Fs", "FsCtx *");
    add_builtin_global("Gs", "FsCtx *");

    if (class_members_.find("FsCtx") == class_members_.end()) {
      class_members_["FsCtx"]["except_ch"] = "I64";
      class_members_["FsCtx"]["except_callers"] = "U8**";
      class_members_["FsCtx"]["catch_except"] = "Bool";
      class_members_["FsCtx"]["hash_table"] = "U8*";
      class_field_offsets_["FsCtx"]["except_ch"] = 0;
      class_field_offsets_["FsCtx"]["except_callers"] = 8;
      class_field_offsets_["FsCtx"]["catch_except"] = 16;
      class_field_offsets_["FsCtx"]["hash_table"] = 24;
      class_layout_sizes_["FsCtx"] = 32;
    }

    if (class_members_.find("CHashClass") == class_members_.end()) {
      class_members_["CHashClass"]["member_lst_and_root"] = "CMemberLst *";
      class_field_offsets_["CHashClass"]["member_lst_and_root"] = 0;
      class_layout_sizes_["CHashClass"] = 8;
    }

    if (class_members_.find("CMemberLst") == class_members_.end()) {
      class_members_["CMemberLst"]["str"] = "U8*";
      class_members_["CMemberLst"]["offset"] = "I64";
      class_members_["CMemberLst"]["next"] = "CMemberLst *";
      class_field_offsets_["CMemberLst"]["str"] = 0;
      class_field_offsets_["CMemberLst"]["offset"] = 8;
      class_field_offsets_["CMemberLst"]["next"] = 16;
      class_layout_sizes_["CMemberLst"] = 24;
    }

    add_builtin_function("PressAKey", "U0", {});
    add_builtin_function("ClassRep", "U0", {ParamSig{"U8*", "ptr", false}});
    add_builtin_function("ClassRepD", "U0", {ParamSig{"U8*", "ptr", false}});
    add_builtin_function("HashFind", "CHashClass *",
                         {ParamSig{"U8*", "name", false},
                          ParamSig{"U8*", "table", false},
                          ParamSig{"I64", "kind", false}});
    add_builtin_function("MemberMetaData", "I64",
                         {ParamSig{"U8*", "key", false},
                          ParamSig{"CMemberLst *", "ml", false}});
    add_builtin_function("MemberMetaFind", "I64",
                         {ParamSig{"U8*", "key", false},
                          ParamSig{"CMemberLst *", "ml", false}});
    add_builtin_function("JobQue", "CJob *",
                         {ParamSig{"U8*", "fn", false},
                          ParamSig{"U8*", "arg", false},
                          ParamSig{"I64", "cpu", false},
                          ParamSig{"I64", "flags", false}});
    add_builtin_function("JobResGet", "I64", {ParamSig{"CJob *", "job", false}});
    add_builtin_function("CallStkGrow", "I64",
                         {ParamSig{"I64", "stack_min", false},
                          ParamSig{"I64", "stack_max", false},
                          ParamSig{"U8*", "fn", false},
                          ParamSig{"I64", "a0", true},
                          ParamSig{"I64", "a1", true},
                          ParamSig{"I64", "a2", true}});
  }

  void CollectFunctionSignatures(const Node& program) {
    for (const Node& child : program.children) {
      if (child.kind != "FunctionDecl") {
        continue;
      }

      const auto [ret_ty, fn_name] = ParseTypedNameFromNode(child);
      if (fn_name.empty()) {
        Error("invalid function declaration: " + child.text);
      }
      ValidateDeclModifiers(ret_ty, "function declaration");

      FunctionSig sig;
      const std::string normalized_ret_ty = StripDeclModifiers(ret_ty);
      sig.return_type = normalized_ret_ty.empty() ? "I64" : normalized_ret_ty;
      sig.name = fn_name;
      sig.linkage_kind = ResolveFunctionLinkageKind(ret_ty);
      sig.imported = IsImportLinkage(ret_ty);

      if (const Node* params = FindChildByKind(child, "ParamList"); params != nullptr) {
        for (const Node& p : params->children) {
          const auto [param_ty, param_name] = ParseTypedNameFromNode(p);
          if (param_name.empty()) {
            Error("invalid parameter declaration: " + p.text);
          }
          ValidateDeclModifiers(param_ty, "parameter declaration");
          const std::string normalized_param_ty = StripDeclModifiers(param_ty);
          const Node* default_expr = FindChildByKind(p, "Default");
          sig.params.push_back(ParamSig{normalized_param_ty.empty() ? "I64" : normalized_param_ty,
                                        param_name, default_expr != nullptr});
        }
      }

      const bool has_body = FindChildByKind(child, "Block") != nullptr;
      if (has_body && sig.imported) {
        Error("import linkage function cannot have a definition: " + sig.name);
      }

      const auto it = functions_.find(sig.name);
      if (it == functions_.end()) {
        functions_[sig.name] = sig;
      } else {
        if (!SameSignature(it->second, sig)) {
          Error("conflicting function declaration for: " + sig.name);
        }
        if (it->second.linkage_kind != sig.linkage_kind &&
            (it->second.linkage_kind == "internal" || sig.linkage_kind == "internal")) {
          Error("conflicting function linkage for: " + sig.name);
        }
        if (it->second.imported != sig.imported && has_body) {
          Error("conflicting import linkage declaration for: " + sig.name);
        }
      }

      if (has_body) {
        const auto [_, inserted] = function_definitions_.insert(sig.name);
        if (!inserted) {
          Error("duplicate function definition for: " + sig.name);
        }
      }
    }
  }

  void CollectGlobalSymbols(const Node& program) {
    for (const Node& child : program.children) {
      if (child.kind == "VarDecl") {
        const auto [decl_ty, name] = ParseTypedNameFromNode(child);
        if (name.empty()) {
          Error("invalid global variable declaration: " + child.text);
        }
        ValidateDeclModifiers(decl_ty, "global variable declaration");
        const std::string normalized_decl_ty = StripDeclModifiers(decl_ty);
        DeclareGlobal(name, normalized_decl_ty.empty() ? "I64" : normalized_decl_ty);
        continue;
      }

      if (child.kind == "VarDeclList") {
        for (const Node& item : child.children) {
          if (item.kind != "VarDecl") {
            continue;
          }
          const auto [decl_ty, name] = ParseTypedNameFromNode(item);
          if (name.empty()) {
            Error("invalid global variable declaration: " + item.text);
          }
          ValidateDeclModifiers(decl_ty, "global variable declaration");
          const std::string normalized_decl_ty = StripDeclModifiers(decl_ty);
          DeclareGlobal(name, normalized_decl_ty.empty() ? "I64" : normalized_decl_ty);
        }
        continue;
      }

      if (child.kind == "LinkageDecl") {
        if (child.children.empty()) {
          continue;
        }
        ValidateLinkageKind(child.text, "linkage declaration");
        const std::string& decl_spec = child.children[0].text;
        const auto [decl_ty, name] = ParseTypedName(decl_spec);
        if (name.empty()) {
          continue;
        }
        ValidateDeclModifiers(decl_ty, "linkage declaration");
        const std::string normalized_decl_ty = StripDeclModifiers(decl_ty);
        DeclareImported(name, normalized_decl_ty.empty() ? "I64" : normalized_decl_ty,
                        child.text);
        continue;
      }

      if (child.kind == "ClassDecl") {
        const auto [_, class_name] = ParseTypedName(child.text);
        if (class_name.empty()) {
          continue;
        }
        if (class_members_.find(class_name) != class_members_.end()) {
          Error("duplicate class/union declaration: " + class_name);
        }
        const bool is_union = child.text.rfind("union ", 0) == 0;
        auto& members = class_members_[class_name];
        auto& offsets = class_field_offsets_[class_name];
        std::size_t layout_size = 0;
        std::size_t running_offset = 0;
        for (const Node& field : child.children) {
          if (field.kind != "FieldDecl") {
            continue;
          }
          const auto [field_ty, field_name] = ParseTypedNameFromNode(field);
          if (field_name.empty()) {
            continue;
          }
          ValidateDeclModifiers(field_ty, "field declaration");
          if (members.find(field_name) != members.end()) {
            Error("duplicate field in " + class_name + ": " + field_name);
          }
          const std::string normalized_field_ty = StripDeclModifiers(field_ty);
          members[field_name] = normalized_field_ty.empty() ? "I64" : normalized_field_ty;
          if (is_union) {
            offsets[field_name] = 0;
            layout_size = std::max(layout_size, EstimateTypeSize(members[field_name]));
          } else {
            offsets[field_name] = running_offset;
            running_offset += EstimateTypeSize(members[field_name]);
            layout_size = running_offset;
          }
        }
        class_layout_sizes_[class_name] = layout_size;

        for (const Node& trailing : child.children) {
          if (trailing.kind != "VarDecl") {
            continue;
          }
          const auto [decl_ty, name] = ParseTypedNameFromNode(trailing);
          if (name.empty()) {
            Error("invalid global variable declaration: " + trailing.text);
          }
          DeclareGlobal(name, decl_ty.empty() ? class_name : decl_ty);
        }
      }
    }
  }

  void AnalyzeTopLevel(Node& node) {
    if (node.kind == "FunctionDecl") {
      AnalyzeFunction(node);
      return;
    }

    if (node.kind == "VarDecl") {
      AnalyzeVarDecl(node);
      return;
    }

    if (node.kind == "VarDeclList") {
      for (Node& item : node.children) {
        if (item.kind == "VarDecl") {
          AnalyzeVarDecl(item);
        }
      }
      return;
    }

    if (node.kind == "LinkageDecl" || node.kind == "TypeAliasDecl") {
      return;
    }

    if (node.kind == "ClassDecl") {
      for (Node& trailing : node.children) {
        if (trailing.kind == "VarDecl") {
          AnalyzeVarDecl(trailing);
        }
      }
      return;
    }

    AnalyzeStatement(node);
  }

  void AnalyzeFunction(Node& fn_node) {
    const auto [ret_ty, fn_name] = ParseTypedNameFromNode(fn_node);
    if (fn_name.empty()) {
      Error("invalid function name");
    }
    ValidateDeclModifiers(ret_ty, "function declaration");

    labels_.clear();
    goto_targets_.clear();
    label_positions_.clear();
    goto_infos_.clear();
    init_decl_infos_.clear();
    Node* body = nullptr;
    const auto body_it = std::find_if(
        fn_node.children.begin(), fn_node.children.end(),
        [](const Node& child) { return child.kind == "Block"; });
    if (body_it != fn_node.children.end()) {
      body = &(*body_it);
    }
    if (body != nullptr) {
      CollectLabels(*body);
      int next_index = 0;
      CollectGotoLegalityInfo(*body, 0, &next_index);
    }

    const std::string normalized_ret_ty = StripDeclModifiers(ret_ty);
    current_return_type_ = normalized_ret_ty.empty() ? "I64" : normalized_ret_ty;
    if (fn_node.line < 0) {
      fn_node.line = 0;
    }
    if (fn_node.column < 0) {
      fn_node.column = 0;
    }
    in_function_ = true;
    PushScope();

    const auto it = functions_.find(fn_name);
    if (it != functions_.end()) {
      for (const ParamSig& p : it->second.params) {
        DeclareLocal(p.name, p.type);
      }
    }

    if (body != nullptr) {
      AnalyzeStatement(*body);
    }

    for (const std::string& target : goto_targets_) {
      if (labels_.find(target) == labels_.end()) {
        Error("goto target label not found in function: " + target);
      }
    }
    ValidateGotoLegality();

    PopScope();
    in_function_ = false;
    current_return_type_.clear();
  }

  void CollectLabels(const Node& node) {
    if (node.kind == "LabelStmt") {
      const auto [_, inserted] = labels_.insert(node.text);
      if (!inserted) {
        Error("duplicate label in function: " + node.text);
      }
    }
    for (const Node& child : node.children) {
      CollectLabels(child);
    }
  }

  void CollectGotoLegalityInfo(const Node& node, int depth, int* next_index) {
    if (node.kind == "Block") {
      for (const Node& child : node.children) {
        CollectGotoLegalityInfo(child, depth + 1, next_index);
      }
      return;
    }

    const bool is_stmt = IsStatementNodeKind(node.kind);
    int this_index = -1;
    if (is_stmt) {
      this_index = (*next_index)++;
    }

    if (node.kind == "LabelStmt") {
      label_positions_[node.text] = LabelInfo{this_index, depth};
    } else if (node.kind == "GotoStmt") {
      goto_targets_.push_back(node.text);
      goto_infos_.push_back(GotoInfo{node.text, this_index, depth});
    } else if (node.kind == "VarDecl" && FindVarInitializer(node) != nullptr) {
      const auto [_, name] = ParseTypedNameFromNode(node);
      init_decl_infos_.push_back(
          InitDeclInfo{name.empty() ? node.text : name, this_index, depth});
    }

    for (const Node& child : node.children) {
      CollectGotoLegalityInfo(child, depth, next_index);
    }
  }

  void ValidateGotoLegality() {
    for (const GotoInfo& g : goto_infos_) {
      const auto label_it = label_positions_.find(g.target);
      if (label_it == label_positions_.end()) {
        continue;
      }
      const LabelInfo& label = label_it->second;

      if (label.depth > g.depth) {
        Error("goto jumps into deeper scope: " + g.target);
      }

      if (label.index > g.index) {
        for (const InitDeclInfo& init : init_decl_infos_) {
          if (init.index > g.index && init.index < label.index) {
            Error("goto jumps across initialized declaration: " + init.name);
          }
        }
      }
    }
  }

  void AnalyzeStatement(Node& node) {
    if (node.kind == "Block") {
      PushScope();
      for (Node& child : node.children) {
        AnalyzeStatement(child);
      }
      PopScope();
      return;
    }

    if (node.kind == "VarDecl") {
      AnalyzeVarDecl(node);
      return;
    }

    if (node.kind == "VarDeclList") {
      for (Node& item : node.children) {
        if (item.kind == "VarDecl") {
          AnalyzeVarDecl(item);
        }
      }
      return;
    }

    if (node.kind == "PrintStmt") {
      AnalyzePrintStmt(node);
      return;
    }

    if (node.kind == "ExprStmt") {
      if (node.children.empty()) {
        return;
      }

      Node& expr = node.children[0];
      if (expr.kind == "Identifier") {
        const auto f_it = functions_.find(expr.text);
        if (f_it != functions_.end()) {
          const bool all_default = std::all_of(
              f_it->second.params.begin(), f_it->second.params.end(),
              [](const ParamSig& p) { return p.has_default; });
          if (!all_default) {
            Error("function call without parentheses requires defaults for all params: " +
                  expr.text);
          }
          node.kind = "NoParenCallStmt";
          node.type = f_it->second.return_type;
          return;
        }
      }

      node.type = AnalyzeExpr(expr);
      return;
    }

    if (node.kind == "IfStmt" || node.kind == "WhileStmt") {
      if (!node.children.empty()) {
        AnalyzeExpr(node.children[0]);
      }
      for (std::size_t i = 1; i < node.children.size(); ++i) {
        AnalyzeStatement(node.children[i]);
      }
      return;
    }

    if (node.kind == "ForStmt") {
      for (Node& child : node.children) {
        if (child.kind == "Init" || child.kind == "Cond" || child.kind == "Inc") {
          continue;
        }
        if (child.kind == "Block" || child.kind.find("Stmt") != std::string::npos) {
          AnalyzeStatement(child);
        } else {
          AnalyzeExpr(child);
        }
      }
      return;
    }

    if (node.kind == "DoWhileStmt") {
      if (!node.children.empty()) {
        AnalyzeStatement(node.children[0]);
      }
      if (node.children.size() > 1) {
        AnalyzeExpr(node.children[1]);
      }
      return;
    }

    if (node.kind == "SwitchStmt") {
      if (!node.children.empty()) {
        AnalyzeExpr(node.children[0]);
      }
      if (node.children.size() > 1) {
        AnalyzeStatement(node.children[1]);
      }
      return;
    }

    if (node.kind == "CaseClause") {
      for (Node& child : node.children) {
        if (child.kind.find("Stmt") != std::string::npos || child.kind == "Block") {
          AnalyzeStatement(child);
        } else {
          AnalyzeExpr(child);
        }
      }
      return;
    }

    if (node.kind == "DefaultClause" || node.kind == "LockStmt") {
      for (Node& child : node.children) {
        AnalyzeStatement(child);
      }
      return;
    }

    if (node.kind == "AsmStmt") {
      if (node.text.empty()) {
        Error("inline asm requires non-empty body/template");
      }

      if (!node.children.empty()) {
        bool awaiting_operand = false;
        std::string awaiting_constraint;

        for (std::size_t i = 0; i < node.children.size(); ++i) {
          Node& arg = node.children[i];
          if (arg.kind != "AsmArg") {
            Error("inline asm argument node must be AsmArg");
          }
          if (TrimCopy(arg.text).empty()) {
            Error("inline asm argument cannot be empty");
          }

          if (arg.children.size() != 1) {
            Error("inline asm argument must parse as an expression");
          }

          Node& arg_expr = arg.children[0];
          arg.type = AnalyzeExpr(arg_expr);

          if (i == 0) {
            if (!IsStringLiteralText(arg_expr.text)) {
              Error("inline asm first argument must be a string-literal template");
            }
            continue;
          }

          if (IsStringLiteralText(arg_expr.text)) {
            if (awaiting_operand) {
              Error("inline asm input constraint requires operand expression: " +
                    awaiting_constraint);
            }

            if (InlineAsmConstraintNeedsOperand(arg_expr.text)) {
              awaiting_operand = true;
              awaiting_constraint = InlineAsmConstraintText(arg_expr.text);
            } else {
              awaiting_operand = false;
              awaiting_constraint.clear();
            }
            continue;
          }

          if (!awaiting_operand) {
            Error("inline asm operand expression must follow an input constraint string");
          }
          awaiting_operand = false;
          awaiting_constraint.clear();
        }

        if (awaiting_operand) {
          Error("inline asm input constraint requires operand expression: " +
                awaiting_constraint);
        }
      } else if (TrimCopy(node.text).empty()) {
        Error("inline asm block body cannot be empty");
      }

      return;
    }

    if (node.kind == "LinkageDecl" || node.kind == "TypeAliasDecl") {
      return;
    }

    if (node.kind == "TryStmt") {
      if (node.children.size() != 2) {
        Error("try statement requires both try and catch bodies");
      }
      AnalyzeStatement(node.children[0]);
      AnalyzeStatement(node.children[1]);
      return;
    }

    if (node.kind == "ThrowStmt") {
      if (node.children.size() != 1) {
        Error("throw requires exactly one payload expression");
      }
      const std::string payload_ty = AnalyzeExpr(node.children[0]);
      if (!IsThrowable(ParseTypeInfo(payload_ty))) {
        Error("throw payload must be integral-like, got: " + payload_ty);
      }
      node.type = "I64";
      return;
    }

    if (node.kind == "GotoStmt") {
      return;
    }

    if (node.kind == "ReturnStmt") {
      if (!node.children.empty()) {
        const std::string expr_ty = AnalyzeExpr(node.children[0]);
        if (!current_return_type_.empty() &&
            !CanImplicitConvert(expr_ty, current_return_type_)) {
          Error("return type mismatch: cannot convert " + expr_ty + " to " +
                current_return_type_);
        }
        node.type = expr_ty;
      } else {
        node.type = "U0";
      }
      return;
    }

    if (node.kind == "LabelStmt") {
      if (!node.children.empty()) {
        AnalyzeStatement(node.children[0]);
      }
      return;
    }
  }

  void AnalyzePrintStmt(Node& node) {
    if (node.children.empty()) {
      Error("print statement requires a format expression");
    }

    Node& format_node = node.children[0];
    const std::string format_ty = AnalyzeExpr(format_node);

    std::vector<std::string> arg_types;
    arg_types.reserve(node.children.size() > 1 ? node.children.size() - 1 : 0);
    for (std::size_t i = 1; i < node.children.size(); ++i) {
      arg_types.push_back(AnalyzeExpr(node.children[i]));
    }

    if (format_node.kind != "Literal") {
      const TypeInfo fmt_info = ParseTypeInfo(format_ty);
      if (fmt_info.kind != ValueKind::kPointer && fmt_info.kind != ValueKind::kUnknown) {
        Error("dynamic print format must be pointer-like, got: " + format_ty);
      }
      node.type = "U0";
      return;
    }

    if (IsCharLiteralText(format_node.text)) {
      if (!arg_types.empty()) {
        Error("char-literal print form does not take format arguments");
      }
      node.type = "U0";
      return;
    }

    if (!IsStringLiteralText(format_node.text)) {
      Error("print format must be a string or char literal");
    }

    if (TrimCopy(format_node.text) == "\"\"" && !arg_types.empty()) {
      const TypeInfo dyn_fmt_info = ParseTypeInfo(arg_types[0]);
      if (dyn_fmt_info.kind != ValueKind::kPointer &&
          dyn_fmt_info.kind != ValueKind::kUnknown) {
        Error("dynamic print format expression must be pointer-like, got: " + arg_types[0]);
      }
      node.type = "U0";
      return;
    }

    std::string format_error;
    const std::vector<PrintFormatSpec> specs =
        CollectPrintFormatSpecifiers(format_node.text, &format_error);
    if (!format_error.empty()) {
      Error(format_error);
    }

    std::size_t expected_args = 0;
    for (const PrintFormatSpec& spec : specs) {
      if (spec.width_from_arg) {
        ++expected_args;
      }
      if (spec.precision_from_arg) {
        ++expected_args;
      }
      expected_args += (spec.conv == 'z') ? 2 : 1;
    }

    if (expected_args != arg_types.size()) {
      Error("print argument count mismatch: format expects " +
            std::to_string(expected_args) + ", got " + std::to_string(arg_types.size()));
    }

    std::size_t arg_index = 0;
    for (const PrintFormatSpec& spec : specs) {
      if (spec.width_from_arg) {
        const TypeInfo width_info = ParseTypeInfo(arg_types[arg_index]);
        if (!IsIntegralLike(width_info) && width_info.kind != ValueKind::kUnknown) {
          Error("print width argument " + std::to_string(arg_index + 1) +
                " must be integral-like, got: " + arg_types[arg_index]);
        }
        ++arg_index;
      }

      if (spec.precision_from_arg) {
        const TypeInfo precision_info = ParseTypeInfo(arg_types[arg_index]);
        if (!IsIntegralLike(precision_info) && precision_info.kind != ValueKind::kUnknown) {
          Error("print precision argument " + std::to_string(arg_index + 1) +
                " must be integral-like, got: " + arg_types[arg_index]);
        }
        ++arg_index;
      }

      if (spec.conv == 'z') {
        const TypeInfo idx_info = ParseTypeInfo(arg_types[arg_index]);
        if (!PrintSpecifierAcceptsType('z', idx_info)) {
          Error("print argument " + std::to_string(arg_index + 1) +
                " has incompatible type " + arg_types[arg_index] +
                " for conversion '%z'");
        }
        const TypeInfo table_info = ParseTypeInfo(arg_types[arg_index + 1]);
        if (table_info.kind != ValueKind::kPointer &&
            table_info.kind != ValueKind::kUnknown) {
          Error("print argument " + std::to_string(arg_index + 2) +
                " must be pointer-like for conversion '%z'");
        }
        arg_index += 2;
        continue;
      }

      const TypeInfo arg_info = ParseTypeInfo(arg_types[arg_index]);
      if (!PrintSpecifierAcceptsType(spec.conv, arg_info)) {
        Error("print argument " + std::to_string(arg_index + 1) +
              " has incompatible type " + arg_types[arg_index] +
              " for conversion '%" + std::string(1, spec.conv) + "'");
      }
      ++arg_index;
    }

    node.type = "U0";
  }

  void AnalyzeVarDecl(Node& node) {
    const auto [decl_ty, name] = ParseTypedNameFromNode(node);
    if (name.empty()) {
      Error("invalid variable declaration: " + node.text);
    }
    ValidateDeclModifiers(decl_ty, "variable declaration");
    const std::string normalized_decl_ty = StripDeclModifiers(decl_ty);
    const std::string resolved_type = normalized_decl_ty.empty() ? "I64" : normalized_decl_ty;
    if (in_function_) {
      DeclareLocal(name, resolved_type);
    } else {
      const auto global_it = global_symbols_.find(name);
      if (global_it == global_symbols_.end()) {
        DeclareGlobal(name, resolved_type);
      } else if (global_it->second != resolved_type) {
        Error("conflicting global declaration type for: " + name);
      }
    }
    node.type = resolved_type;

    Node* init = nullptr;
    for (Node& child : node.children) {
      if (child.kind == "DeclType" || child.kind == "DeclName") {
        continue;
      }
      init = &child;
      break;
    }
    if (init != nullptr) {
      const std::string init_ty = AnalyzeExpr(*init);
      if (!CanImplicitConvert(init_ty, node.type)) {
        Error("initializer type mismatch for " + name + ": cannot convert " + init_ty +
              " to " + node.type);
      }
      init->type = init_ty;
    }
  }

  std::string AnalyzeExpr(Node& node) {
    if (node.kind == "Identifier") {
      const std::string* local_ty = Lookup(node.text);
      if (local_ty != nullptr) {
        node.type = *local_ty;
        return node.type;
      }

      const auto f_it = functions_.find(node.text);
      if (f_it != functions_.end()) {
        node.type = "fn " + f_it->second.return_type;
        return node.type;
      }

      Error("unknown identifier: " + node.text);
    }

    if (node.kind == "Literal") {
      if (!node.text.empty() && node.text.front() == '"') {
        node.type = "U8*";
      } else if (!node.text.empty() && node.text.front() == '\'') {
        node.type = "I64";
      } else if (node.text.find('.') != std::string::npos) {
        node.type = "F64";
      } else {
        node.type = "I64";
      }
      return node.type;
    }

    if (node.kind == "DollarExpr") {
      node.type = "I64";
      return node.type;
    }

    if (node.kind == "UnaryExpr") {
      if (!node.children.empty()) {
        const std::string child_ty = AnalyzeExpr(node.children[0]);
        const TypeInfo child_info = ParseTypeInfo(child_ty);
        if (node.text == "!") {
          if (!IsNumeric(child_info) && child_info.kind != ValueKind::kPointer &&
              child_info.kind != ValueKind::kUnknown) {
            Error("operator ! requires scalar operand");
          }
          node.type = "Bool";
        } else if (node.text == "&") {
          node.type = AddPointerLevel(child_ty);
        } else if (node.text == "*") {
          if (child_info.kind != ValueKind::kPointer && child_info.kind != ValueKind::kUnknown) {
            Error("operator * requires pointer operand");
          }
          node.type = RemovePointerLevel(child_ty);
          if (node.type.empty()) {
            node.type = "I64";
          }
        } else if (node.text == "~") {
          if (!(child_info.kind == ValueKind::kBool || child_info.kind == ValueKind::kInt ||
                child_info.kind == ValueKind::kUInt || child_info.kind == ValueKind::kUnknown)) {
            Error("operator ~ requires integer-like operand");
          }
          node.type = child_ty;
        } else if (node.text == "+" || node.text == "-") {
          if (!IsNumeric(child_info) && child_info.kind != ValueKind::kUnknown) {
            Error("unary " + node.text + " requires numeric operand");
          }
          node.type = child_ty;
        } else if (node.text == "++" || node.text == "--") {
          if (!IsNumeric(child_info) && child_info.kind != ValueKind::kPointer &&
              child_info.kind != ValueKind::kUnknown) {
            Error("operator " + node.text + " requires numeric or pointer operand");
          }
          node.type = child_ty;
        } else {
          node.type = child_ty;
        }
      }
      return node.type;
    }

    if (node.kind == "CastExpr") {
      if (!node.children.empty()) {
        AnalyzeExpr(node.children[0]);
      }
      node.type = node.text.empty() ? "I64" : node.text;
      return node.type;
    }

    if (node.kind == "PostfixExpr") {
      if (!node.children.empty()) {
        const std::string operand_ty = AnalyzeExpr(node.children[0]);
        const TypeInfo info = ParseTypeInfo(operand_ty);
        if (!IsNumeric(info) && info.kind != ValueKind::kPointer &&
            info.kind != ValueKind::kUnknown) {
          Error("postfix operator requires numeric or pointer operand");
        }
        node.type = operand_ty;
      } else {
        node.type = "I64";
      }
      return node.type;
    }

    if (node.kind == "AssignExpr") {
      if (node.children.size() == 2) {
        const std::string lhs_ty = AnalyzeExpr(node.children[0]);
        const std::string rhs_ty = AnalyzeExpr(node.children[1]);
        if (!CanImplicitConvert(rhs_ty, lhs_ty)) {
          Error("assignment type mismatch: cannot convert " + rhs_ty + " to " + lhs_ty);
        }
        node.type = lhs_ty;
      }
      return node.type;
    }

    if (node.kind == "BinaryExpr") {
      if (node.children.size() != 2) {
        Error("invalid binary expression");
      }

      AnalyzeExpr(node.children[0]);
      AnalyzeExpr(node.children[1]);
      const std::string lhs_ty = node.children[0].type;
      const std::string rhs_ty = node.children[1].type;
      const TypeInfo lhs_info = ParseTypeInfo(lhs_ty);
      const TypeInfo rhs_info = ParseTypeInfo(rhs_ty);

      if (IsRelationalOp(node.text) || node.text == "==" || node.text == "!=") {
        if (!CanImplicitConvert(lhs_ty, rhs_ty) && !CanImplicitConvert(rhs_ty, lhs_ty)) {
          Error("comparison requires implicitly comparable operands: " + lhs_ty + " vs " +
                rhs_ty);
        }
        node.type = "Bool";
        if (node.children[0].kind == "BinaryExpr" && IsRelationalOp(node.children[0].text)) {
          node.type = "Bool(chained)";
        }
        return node.type;
      }

      if (node.text == "&&" || node.text == "||") {
        if ((!IsNumeric(lhs_info) && lhs_info.kind != ValueKind::kPointer &&
             lhs_info.kind != ValueKind::kUnknown) ||
            (!IsNumeric(rhs_info) && rhs_info.kind != ValueKind::kPointer &&
             rhs_info.kind != ValueKind::kUnknown)) {
          Error("logical operators require scalar operands");
        }
        node.type = "Bool";
      } else {
        if (!CanImplicitConvert(lhs_ty, rhs_ty) && !CanImplicitConvert(rhs_ty, lhs_ty)) {
          Error("binary operator " + node.text + " requires compatible operands: " + lhs_ty +
                " vs " + rhs_ty);
        }

        if (node.text == "+" || node.text == "-" || node.text == "*" || node.text == "/" ||
            node.text == "%") {
          const bool lhs_ptr = lhs_info.kind == ValueKind::kPointer;
          const bool rhs_ptr = rhs_info.kind == ValueKind::kPointer;
          if (lhs_ptr || rhs_ptr) {
            if (node.text == "+") {
              if (lhs_ptr && IsIntegralLike(rhs_info)) {
                node.type = lhs_ty;
              } else if (rhs_ptr && IsIntegralLike(lhs_info)) {
                node.type = rhs_ty;
              } else {
                Error("pointer addition requires one pointer and one integer operand");
              }
            } else if (node.text == "-") {
              if (lhs_ptr && IsIntegralLike(rhs_info)) {
                node.type = lhs_ty;
              } else if (lhs_ptr && rhs_ptr) {
                node.type = "I64";
              } else {
                Error("pointer subtraction requires pointer-int or pointer-pointer");
              }
            } else {
              Error("pointer arithmetic supports only + and -");
            }
          } else {
            node.type = PromoteIntegerResultType(lhs_ty, rhs_ty);
          }
        } else if (node.text == "&" || node.text == "|" || node.text == "^" ||
                   node.text == "<<" || node.text == ">>") {
          if (!IsIntegralLike(lhs_info) || !IsIntegralLike(rhs_info)) {
            Error("bitwise/shift operators require integral operands");
          }
          node.type = PromoteIntegerResultType(lhs_ty, rhs_ty);
        } else {
          node.type = PromoteIntegerResultType(lhs_ty, rhs_ty);
        }
      }
      return node.type;
    }

    if (node.kind == "CommaExpr") {
      for (Node& child : node.children) {
        AnalyzeExpr(child);
      }
      node.type = node.children.empty() ? "I64" : node.children.back().type;
      return node.type;
    }

    if (node.kind == "CallExpr") {
      if (node.children.size() < 2) {
        Error("invalid call expression");
      }
      if (node.children[1].kind != "CallArgs") {
        Error("invalid call argument list");
      }
      Node& callee = node.children[0];
      Node& arg_list = node.children[1];
      const bool direct_named_call =
          callee.kind == "Identifier" &&
          functions_.find(callee.text) != functions_.end() &&
          LookupLocalOnly(callee.text) == nullptr;

      if (direct_named_call) {
        const std::string& fn_name = callee.text;
        const FunctionSig& sig = functions_.find(fn_name)->second;
        std::size_t param_i = 0;
        for (Node& arg : arg_list.children) {
          if (param_i >= sig.params.size()) {
            Error("too many arguments for function: " + fn_name);
          }

          if (arg.kind == "EmptyArg") {
            if (!sig.params[param_i].has_default) {
              Error("missing argument without default at position " +
                    std::to_string(param_i + 1) + " in call to " + fn_name);
            }
          } else {
            const std::string arg_ty = AnalyzeExpr(arg);
            if (!CanImplicitConvert(arg_ty, sig.params[param_i].type)) {
              Error("argument type mismatch at position " + std::to_string(param_i + 1) +
                    " in call to " + fn_name + ": cannot convert " + arg_ty + " to " +
                    sig.params[param_i].type);
            }
          }
          ++param_i;
        }

        while (param_i < sig.params.size()) {
          if (!sig.params[param_i].has_default) {
            Error("missing required argument at position " + std::to_string(param_i + 1) +
                  " in call to " + fn_name);
          }
          ++param_i;
        }

        node.type = sig.return_type;
        return node.type;
      }

      const std::string callee_ty = AnalyzeExpr(callee);
      const TypeInfo callee_info = ParseTypeInfo(callee_ty);
      if (callee_info.kind != ValueKind::kPointer &&
          callee_info.kind != ValueKind::kUnknown &&
          callee_ty.rfind("fn ", 0) != 0) {
        Error("call target is not callable: " + callee_ty);
      }

      for (Node& arg : arg_list.children) {
        if (arg.kind == "EmptyArg") {
          Error("sparse/default call arguments require a direct named function");
        }
        (void)AnalyzeExpr(arg);
      }

      node.type = InferCallReturnTypeFromCalleeType(callee_ty);
      return node.type;
    }

    if (node.kind == "LaneExpr") {
      if (node.children.size() != 2) {
        Error("lane access requires base and index expression");
      }

      const std::string base_ty = AnalyzeExpr(node.children[0]);
      const TypeInfo base_info = ParseTypeInfo(base_ty);
      if (!IsIntegralLike(base_info) && base_info.kind != ValueKind::kUnknown) {
        Error("lane base must be integral-like, got: " + base_ty);
      }

      const std::string index_ty = AnalyzeExpr(node.children[1]);
      const TypeInfo index_info = ParseTypeInfo(index_ty);
      if (!IsIntegralLike(index_info) && index_info.kind != ValueKind::kUnknown) {
        Error("lane index must be integral, got: " + index_ty);
      }

      const std::string lane_ty = LaneElementType(node.text);
      if (lane_ty.empty()) {
        Error("unknown lane selector: " + node.text);
      }

      const int lane_bits = LaneElementBits(node.text);
      if (lane_bits <= 0) {
        Error("invalid lane selector width: " + node.text);
      }

      if (base_info.kind != ValueKind::kUnknown) {
        if (base_info.bits <= 0 || lane_bits > base_info.bits) {
          Error("lane selector '" + node.text + "' is wider than base type " + base_ty);
        }

        const int lane_count = base_info.bits / lane_bits;
        if (lane_count <= 0) {
          Error("invalid lane count for selector '" + node.text + "' on " + base_ty);
        }

        std::int64_t lane_index = 0;
        if (TryParseIntLiteral(node.children[1], &lane_index)) {
          if (lane_index < 0 ||
              lane_index >= static_cast<std::int64_t>(lane_count)) {
            Error("lane index out of range for selector '" + node.text + "': " +
                  std::to_string(lane_index));
          }
        }
      }

      node.type = lane_ty;
      return node.type;
    }

    if (node.kind == "MemberExpr") {
      if (!node.children.empty()) {
        const std::string base_ty = AnalyzeExpr(node.children[0]);
        const std::string aggregate_name = NormalizeAggregateTypeName(base_ty);
        const auto agg_it = class_members_.find(aggregate_name);
        if (agg_it != class_members_.end()) {
          const auto member_it = agg_it->second.find(node.text);
          if (member_it == agg_it->second.end()) {
            Error("unknown member '" + node.text + "' on " + aggregate_name);
          }
          node.type = member_it->second;
          return node.type;
        }
      }
      node.type = "I64";
      return node.type;
    }

    if (node.kind == "IndexExpr") {
      if (node.children.size() == 2) {
        AnalyzeExpr(node.children[0]);
        AnalyzeExpr(node.children[1]);
      }
      node.type = "I64";
      return node.type;
    }

    return "I64";
  }

  void PushScope() { scopes_.push_back({}); }

  void PopScope() {
    if (!scopes_.empty()) {
      scopes_.pop_back();
    }
  }

  void DeclareLocal(const std::string& name, const std::string& type) {
    if (scopes_.empty()) {
      PushScope();
    }
    auto& top = scopes_.back();
    if (top.find(name) != top.end()) {
      Error("duplicate declaration: " + name);
    }
    top[name] = type;
  }

  void DeclareGlobal(const std::string& name, const std::string& type) {
    if (global_symbols_.find(name) != global_symbols_.end()) {
      Error("duplicate global declaration: " + name);
    }
    if (functions_.find(name) != functions_.end()) {
      Error("global declaration conflicts with function symbol: " + name);
    }
    const auto imported_it = imported_symbols_.find(name);
    if (imported_it != imported_symbols_.end()) {
      if (imported_it->second != type) {
        Error("global declaration type conflicts with imported symbol: " + name);
      }
      imported_symbols_.erase(imported_it);
    }
    global_symbols_[name] = type;
  }

  void DeclareImported(const std::string& name, const std::string& type,
                      std::string_view linkage_kind) {
    const auto global_it = global_symbols_.find(name);
    if (global_it != global_symbols_.end()) {
      if (global_it->second != type) {
        Error("imported symbol conflicts with global declaration: " + name);
      }
      return;
    }
    if (functions_.find(name) != functions_.end()) {
      Error("imported symbol conflicts with function symbol: " + name);
    }
    if (linkage_kind != "extern" && linkage_kind != "_extern" && linkage_kind != "import" &&
        linkage_kind != "_import" && linkage_kind != "export" &&
        linkage_kind != "_export") {
      Error("unsupported linkage declaration: " + std::string(linkage_kind));
    }
    const auto [it, inserted] = imported_symbols_.insert({name, type});
    if (!inserted && it->second != type) {
      Error("conflicting imported symbol declaration: " + name);
    }
  }

  const std::string* Lookup(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto found = it->find(name);
      if (found != it->end()) {
        return &found->second;
      }
    }
    const auto global_it = global_symbols_.find(name);
    if (global_it != global_symbols_.end()) {
      return &global_it->second;
    }
    const auto imported_it = imported_symbols_.find(name);
    if (imported_it != imported_symbols_.end()) {
      return &imported_it->second;
    }
    return nullptr;
  }

  const std::string* LookupLocalOnly(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto found = it->find(name);
      if (found != it->end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  std::string filename_;
  bool strict_mode_ = true;
  std::string current_return_type_;
  bool in_function_ = false;
  std::unordered_map<std::string, FunctionSig> functions_;
  std::unordered_set<std::string> function_definitions_;
  std::unordered_map<std::string, std::string> global_symbols_;
  std::unordered_map<std::string, std::string> imported_symbols_;
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
      class_members_;
  std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>>
      class_field_offsets_;
  std::unordered_map<std::string, std::size_t> class_layout_sizes_;
  std::unordered_map<std::string, LabelInfo> label_positions_;
  std::vector<GotoInfo> goto_infos_;
  std::vector<InitDeclInfo> init_decl_infos_;
  std::vector<std::unordered_map<std::string, std::string>> scopes_;
  std::unordered_set<std::string> labels_;
  std::vector<std::string> goto_targets_;
};

}  // namespace

TypedNode AnalyzeSemantics(const ParsedNode& program, std::string_view filename,
                           bool strict_mode) {
  Node typed = ConvertToTyped(program);
  SemanticAnalyzer analyzer(filename, strict_mode);
  analyzer.Analyze(&typed);
  return typed;
}

}  // namespace holyc::frontend::internal

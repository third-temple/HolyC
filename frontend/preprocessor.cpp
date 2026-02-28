#include "preprocessor.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace holyc::frontend::internal {

namespace {
enum class DiagnosticSeverity {
  kError,
  kWarning,
  kNote,
};

struct Diagnostic {
  std::string code;
  DiagnosticSeverity severity = DiagnosticSeverity::kError;
  std::string file;
  int line = 0;
  int column = 0;
  std::string message;
  std::string remediation;
};

std::string FormatDiagnostic(const Diagnostic& diag) {
  std::ostringstream oss;
  const char* sev = "error";
  if (diag.severity == DiagnosticSeverity::kWarning) {
    sev = "warning";
  } else if (diag.severity == DiagnosticSeverity::kNote) {
    sev = "note";
  }

  oss << sev;
  if (!diag.code.empty()) {
    oss << "[" << diag.code << "]";
  }
  oss << ": ";

  if (!diag.file.empty()) {
    oss << diag.file;
    if (diag.line > 0) {
      oss << ":" << diag.line;
      if (diag.column > 0) {
        oss << ":" << diag.column;
      }
    }
    oss << ": ";
  }

  oss << diag.message;
  if (!diag.remediation.empty()) {
    oss << "\nhelp: " << diag.remediation;
  }
  return oss.str();
}

[[noreturn]] void ThrowDiagnostic(const Diagnostic& diag) {
  throw std::runtime_error(FormatDiagnostic(diag));
}

class Preprocessor {
 public:
  explicit Preprocessor(bool jit_mode, std::vector<std::string> include_dirs = {})
      : jit_mode_(jit_mode), include_dirs_(std::move(include_dirs)) {}

  std::string Process(std::string_view source, std::string_view filename) {
    std::vector<std::string> include_stack;
    include_stack.push_back(CanonicalPath(std::string(filename)));
    return ProcessFile(source, std::string(filename), 0, &include_stack);
  }

 private:
  struct CondFrame {
    bool parent_active;
    bool branch_taken;
    bool current_active;
  };

  struct MacroDef {
    bool function_like = false;
    std::vector<std::string> params;
    std::string body;
  };

  [[noreturn]] static void Fail(std::string_view code, const std::string& file, int line,
                                const std::string& message,
                                std::string remediation = std::string()) {
    ThrowDiagnostic(Diagnostic{std::string(code), DiagnosticSeverity::kError, file, line, 1,
                               message, std::move(remediation)});
  }

  [[noreturn]] static void FailExpr(std::string_view code, const std::string& message,
                                    std::string remediation = std::string()) {
    ThrowDiagnostic(Diagnostic{std::string(code), DiagnosticSeverity::kError, "preprocessor", 0,
                               0, message, std::move(remediation)});
  }

  std::string ProcessFile(std::string_view source, const std::string& file, int depth,
                          std::vector<std::string>* include_stack) {
    if (depth > 64) {
      Fail("HC1001", file, 1, "preprocessor include depth exceeded",
           "reduce include nesting or break include cycles");
    }

    std::vector<CondFrame> cond;
    std::istringstream input{std::string(source)};
    std::ostringstream out;

    int line_no = 0;
    std::string line;
    while (std::getline(input, line)) {
      ++line_no;
      const std::string trimmed = LTrim(line);

      if (StartsWith(trimmed, "#")) {
        std::string directive = trimmed;
        const int directive_line_no = line_no;
        if (IsExeDirectiveLine(trimmed)) {
          while (!HasClosedExeBody(directive)) {
            std::string continued;
            if (!std::getline(input, continued)) {
              Fail("HC1018", file, directive_line_no, "unterminated #exe block");
            }
            ++line_no;
            directive.push_back('\n');
            directive += continued;
          }
        }
        HandleDirective(directive, file, directive_line_no, depth, include_stack, &cond, &out);
        continue;
      }

      if (!IsActive(cond)) {
        continue;
      }

      out << ExpandLine(line, file, line_no) << '\n';
    }

    if (!cond.empty()) {
      Fail("HC1002", file, line_no, "missing #endif",
           "ensure every #if/#ifdef/#ifndef block is closed");
    }

    return out.str();
  }

  void HandleDirective(const std::string& line, const std::string& file, int line_no,
                       int depth, std::vector<std::string>* include_stack,
                       std::vector<CondFrame>* cond, std::ostringstream* out) {
    if (line.empty() || line[0] != '#') {
      Fail("HC1003", file, line_no, "malformed directive line");
    }

    std::istringstream iss(line.substr(1));
    std::string directive;
    iss >> directive;

    if (directive == "ifdef") {
      std::string name;
      iss >> name;
      PushCond(cond, IsDefined(name));
      return;
    }

    if (directive == "ifndef") {
      std::string name;
      iss >> name;
      PushCond(cond, !IsDefined(name));
      return;
    }

    if (directive == "if") {
      std::string expr;
      std::getline(iss, expr);
      PushCond(cond, EvalIfExpr(expr));
      return;
    }

    if (directive == "ifjit") {
      PushCond(cond, jit_mode_);
      return;
    }

    if (directive == "ifaot") {
      PushCond(cond, !jit_mode_);
      return;
    }

    if (directive == "else") {
      if (cond->empty()) {
        Fail("HC1004", file, line_no, "stray #else");
      }
      CondFrame& top = cond->back();
      top.current_active = top.parent_active && !top.branch_taken;
      top.branch_taken = true;
      return;
    }

    if (directive == "elif") {
      if (cond->empty()) {
        Fail("HC1005", file, line_no, "stray #elif");
      }
      CondFrame& top = cond->back();
      if (!top.parent_active || top.branch_taken) {
        top.current_active = false;
        return;
      }
      std::string expr;
      std::getline(iss, expr);
      const bool matched = EvalIfExpr(expr);
      top.current_active = matched;
      if (matched) {
        top.branch_taken = true;
      }
      return;
    }

    if (directive == "endif") {
      if (cond->empty()) {
        Fail("HC1006", file, line_no, "stray #endif");
      }
      cond->pop_back();
      return;
    }

    if (!IsActive(*cond)) {
      return;
    }

    if (directive == "define") {
      std::string rest;
      std::getline(iss, rest);
      ParseDefine(Trim(rest), file, line_no);
      return;
    }

    if (directive == "include") {
      std::string rest;
      std::getline(iss, rest);
      const std::string target = ExtractQuoted(Trim(rest), file, line_no, "#include");
      const std::string include_path = ResolveIncludePath(file, target, line_no);
      const std::string canonical_include = CanonicalPath(include_path);
      const auto cycle_it =
          std::find(include_stack->begin(), include_stack->end(), canonical_include);
      if (cycle_it != include_stack->end()) {
        std::ostringstream trace;
        for (auto it = cycle_it; it != include_stack->end(); ++it) {
          if (it != cycle_it) {
            trace << " -> ";
          }
          trace << *it;
        }
        trace << " -> " << canonical_include;
        Fail("HC1023", file, line_no, "include cycle detected: " + target, trace.str());
      }

      std::ifstream in(include_path);
      if (!in.is_open()) {
        Fail("HC1007", file, line_no, "include not found: " + target,
             "verify include search roots and file path");
      }

      std::ostringstream buf;
      buf << in.rdbuf();
      include_stack->push_back(canonical_include);
      *out << ProcessFile(buf.str(), include_path, depth + 1, include_stack);
      include_stack->pop_back();
      return;
    }

    if (directive == "exe") {
      std::string rest;
      std::getline(iss, rest, '\0');
      const std::string body = Trim(rest);
      *out << EvaluateExe(body, file, line_no);
      return;
    }

    if (directive == "assert") {
      std::string expr;
      std::getline(iss, expr);
      if (!EvalIfExpr(expr)) {
        Fail("HC1008", file, line_no, "#assert failed");
      }
      return;
    }

    Fail("HC1009", file, line_no, "unsupported directive #" + directive);
  }

  void ParseDefine(const std::string& rest, const std::string& file, int line_no) {
    if (rest.empty()) {
      Fail("HC1026", file, line_no, "#define requires a macro name");
    }

    std::size_t i = 0;
    if (!IsIdentStart(rest[i])) {
      Fail("HC1027", file, line_no, "invalid macro name in #define");
    }
    ++i;
    while (i < rest.size() && IsIdentContinue(rest[i])) {
      ++i;
    }
    const std::string name = rest.substr(0, i);

    MacroDef def;
    if (i < rest.size() && rest[i] == '(') {
      def.function_like = true;
      ++i;
      std::string current;
      bool expect_param = true;
      while (i < rest.size() && rest[i] != ')') {
        const char c = rest[i];
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
          ++i;
          continue;
        }
        if (c == ',') {
          if (expect_param) {
            Fail("HC1028", file, line_no, "empty parameter in function-like macro: " + name);
          }
          def.params.push_back(current);
          current.clear();
          expect_param = true;
          ++i;
          continue;
        }
        if (!IsIdentStart(c)) {
          Fail("HC1029", file, line_no,
               "invalid function-like macro parameter list for: " + name);
        }
        current.clear();
        current.push_back(c);
        ++i;
        while (i < rest.size() && IsIdentContinue(rest[i])) {
          current.push_back(rest[i]);
          ++i;
        }
        expect_param = false;
      }
      if (i >= rest.size() || rest[i] != ')') {
        Fail("HC1030", file, line_no,
             "unterminated function-like macro definition for: " + name);
      }
      if (!current.empty()) {
        def.params.push_back(current);
      } else if (!expect_param) {
        Fail("HC1031", file, line_no,
             "malformed function-like macro parameter list for: " + name);
      }
      ++i;
      def.body = Trim(rest.substr(i));
      macros_[name] = std::move(def);
      return;
    }

    def.body = Trim(rest.substr(i));
    macros_[name] = std::move(def);
  }

  static void PushCond(std::vector<CondFrame>* cond, bool condition_true) {
    const bool parent_active = IsActive(*cond);
    cond->push_back(CondFrame{parent_active, condition_true, parent_active && condition_true});
  }

  static bool IsActive(const std::vector<CondFrame>& cond) {
    if (cond.empty()) {
      return true;
    }
    return cond.back().current_active;
  }

  bool IsDefined(const std::string& name) const {
    return macros_.find(name) != macros_.end();
  }

  bool EvalIfExpr(const std::string& expr) const {
    return EvalIfExprValue(expr, 0) != 0;
  }

  int64_t EvalIfExprValue(const std::string& expr, int depth) const {
    if (depth > 64) {
      FailExpr("HC1010", "#if expression recursion depth exceeded");
    }

    enum class TokKind {
      kEnd,
      kNumber,
      kIdentifier,
      kLParen,
      kRParen,
      kOp,
    };

    struct Tok {
      TokKind kind = TokKind::kEnd;
      std::string text;
    };

    auto tokenize = [](const std::string& input) -> std::vector<Tok> {
      std::vector<Tok> tokens;
      std::size_t i = 0;

      auto is_ident_start = [](char c) {
        return (std::isalpha(static_cast<unsigned char>(c)) != 0) || c == '_';
      };
      auto is_ident_continue = [&](char c) {
        return is_ident_start(c) || (std::isdigit(static_cast<unsigned char>(c)) != 0);
      };

      while (i < input.size()) {
        const char c = input[i];
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
          ++i;
          continue;
        }

        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
          const std::size_t begin = i;
          ++i;
          while (i < input.size()) {
            const char n = input[i];
            if (std::isalnum(static_cast<unsigned char>(n)) != 0 || n == '_') {
              ++i;
              continue;
            }
            break;
          }
          tokens.push_back(Tok{TokKind::kNumber, input.substr(begin, i - begin)});
          continue;
        }

        if (is_ident_start(c)) {
          const std::size_t begin = i;
          ++i;
          while (i < input.size() && is_ident_continue(input[i])) {
            ++i;
          }
          tokens.push_back(Tok{TokKind::kIdentifier, input.substr(begin, i - begin)});
          continue;
        }

        if (c == '(') {
          tokens.push_back(Tok{TokKind::kLParen, "("});
          ++i;
          continue;
        }

        if (c == ')') {
          tokens.push_back(Tok{TokKind::kRParen, ")"});
          ++i;
          continue;
        }

        if (i + 1 < input.size()) {
          const std::string two = input.substr(i, 2);
          if (two == "||" || two == "&&" || two == "==" || two == "!=" || two == "<=" ||
              two == ">=" || two == "<<" || two == ">>") {
            tokens.push_back(Tok{TokKind::kOp, two});
            i += 2;
            continue;
          }
        }

        if (std::string("!~+-*/%|^&<>").find(c) != std::string::npos) {
          tokens.push_back(Tok{TokKind::kOp, std::string(1, c)});
          ++i;
          continue;
        }

        throw std::runtime_error(std::string("preprocessor: unsupported token in #if: ") + c);
      }

      tokens.push_back(Tok{TokKind::kEnd, ""});
      return tokens;
    };

    const std::vector<Tok> tokens = tokenize(expr);
    std::size_t index = 0;

    auto peek = [&]() -> const Tok& { return tokens[index]; };
    auto match_op = [&](std::string_view op) {
      if (peek().kind == TokKind::kOp && peek().text == op) {
        ++index;
        return true;
      }
      return false;
    };
    auto match = [&](TokKind kind) {
      if (peek().kind == kind) {
        ++index;
        return true;
      }
      return false;
    };
    auto parse_number = [](std::string text) -> int64_t {
      text.erase(std::remove(text.begin(), text.end(), '_'), text.end());
      char* end = nullptr;
      const long long value = std::strtoll(text.c_str(), &end, 0);
      if (end == text.c_str() || (end != nullptr && *end != '\0')) {
        return 0;
      }
      return static_cast<int64_t>(value);
    };
    auto parse_identifier_value = [&](const std::string& name) -> int64_t {
      if (name == "TRUE" || name == "true") {
        return 1;
      }
      if (name == "FALSE" || name == "false") {
        return 0;
      }
      const auto it = macros_.find(name);
      if (it == macros_.end()) {
        return 0;
      }
      if (it->second.function_like) {
        return 0;
      }
      return EvalIfExprValue(it->second.body, depth + 1);
    };

    std::function<int64_t()> parse_primary;
    std::function<int64_t()> parse_unary;
    std::function<int64_t()> parse_mul;
    std::function<int64_t()> parse_add;
    std::function<int64_t()> parse_shift;
    std::function<int64_t()> parse_relational;
    std::function<int64_t()> parse_equality;
    std::function<int64_t()> parse_bitand;
    std::function<int64_t()> parse_bitxor;
    std::function<int64_t()> parse_bitor;
    std::function<int64_t()> parse_logical_and;
    std::function<int64_t()> parse_logical_or;

    parse_primary = [&]() -> int64_t {
      if (match(TokKind::kLParen)) {
        const int64_t value = parse_logical_or();
        if (!match(TokKind::kRParen)) {
          FailExpr("HC1012", "expected ')' in #if expression");
        }
        return value;
      }

      if (peek().kind == TokKind::kNumber) {
        const std::string text = peek().text;
        ++index;
        return parse_number(text);
      }

      if (peek().kind == TokKind::kIdentifier) {
        const std::string name = peek().text;
        ++index;

        if (name == "defined") {
          std::string target;
          if (match(TokKind::kLParen)) {
            if (peek().kind != TokKind::kIdentifier) {
              FailExpr("HC1013", "expected identifier after defined(");
            }
            target = peek().text;
            ++index;
            if (!match(TokKind::kRParen)) {
              FailExpr("HC1014", "expected ')' after defined(name)");
            }
          } else {
            if (peek().kind != TokKind::kIdentifier) {
              FailExpr("HC1015", "expected identifier after defined");
            }
            target = peek().text;
            ++index;
          }
          return IsDefined(target) ? 1 : 0;
        }

        return parse_identifier_value(name);
      }

      FailExpr("HC1016", "malformed #if expression");
    };

    parse_unary = [&]() -> int64_t {
      if (match_op("!")) {
        return parse_unary() == 0 ? 1 : 0;
      }
      if (match_op("+")) {
        return parse_unary();
      }
      if (match_op("-")) {
        return -parse_unary();
      }
      if (match_op("~")) {
        return ~parse_unary();
      }
      return parse_primary();
    };

    parse_mul = [&]() -> int64_t {
      int64_t value = parse_unary();
      while (true) {
        if (match_op("*")) {
          value *= parse_unary();
          continue;
        }
        if (match_op("/")) {
          const int64_t rhs = parse_unary();
          if (rhs == 0) {
            return 0;
          }
          value /= rhs;
          continue;
        }
        if (match_op("%")) {
          const int64_t rhs = parse_unary();
          if (rhs == 0) {
            return 0;
          }
          value %= rhs;
          continue;
        }
        break;
      }
      return value;
    };

    parse_add = [&]() -> int64_t {
      int64_t value = parse_mul();
      while (true) {
        if (match_op("+")) {
          value += parse_mul();
          continue;
        }
        if (match_op("-")) {
          value -= parse_mul();
          continue;
        }
        break;
      }
      return value;
    };

    parse_shift = [&]() -> int64_t {
      int64_t value = parse_add();
      while (true) {
        if (match_op("<<")) {
          value <<= parse_add();
          continue;
        }
        if (match_op(">>")) {
          value >>= parse_add();
          continue;
        }
        break;
      }
      return value;
    };

    parse_relational = [&]() -> int64_t {
      int64_t value = parse_shift();
      while (true) {
        if (match_op("<")) {
          value = (value < parse_shift()) ? 1 : 0;
          continue;
        }
        if (match_op(">")) {
          value = (value > parse_shift()) ? 1 : 0;
          continue;
        }
        if (match_op("<=")) {
          value = (value <= parse_shift()) ? 1 : 0;
          continue;
        }
        if (match_op(">=")) {
          value = (value >= parse_shift()) ? 1 : 0;
          continue;
        }
        break;
      }
      return value;
    };

    parse_equality = [&]() -> int64_t {
      int64_t value = parse_relational();
      while (true) {
        if (match_op("==")) {
          value = (value == parse_relational()) ? 1 : 0;
          continue;
        }
        if (match_op("!=")) {
          value = (value != parse_relational()) ? 1 : 0;
          continue;
        }
        break;
      }
      return value;
    };

    parse_bitand = [&]() -> int64_t {
      int64_t value = parse_equality();
      while (match_op("&")) {
        value &= parse_equality();
      }
      return value;
    };

    parse_bitxor = [&]() -> int64_t {
      int64_t value = parse_bitand();
      while (match_op("^")) {
        value ^= parse_bitand();
      }
      return value;
    };

    parse_bitor = [&]() -> int64_t {
      int64_t value = parse_bitxor();
      while (match_op("|")) {
        value |= parse_bitxor();
      }
      return value;
    };

    parse_logical_and = [&]() -> int64_t {
      int64_t value = parse_bitor();
      while (match_op("&&")) {
        const int64_t rhs = parse_bitor();
        value = (value != 0 && rhs != 0) ? 1 : 0;
      }
      return value;
    };

    parse_logical_or = [&]() -> int64_t {
      int64_t value = parse_logical_and();
      while (match_op("||")) {
        const int64_t rhs = parse_logical_and();
        value = (value != 0 || rhs != 0) ? 1 : 0;
      }
      return value;
    };

    const int64_t result = parse_logical_or();
    if (peek().kind != TokKind::kEnd) {
      FailExpr("HC1017", "trailing tokens in #if expression");
    }
    return result;
  }

  std::string ExpandLine(const std::string& line, const std::string& file, int line_no) const {
    std::unordered_set<std::string> active_macros;
    return ExpandText(line, file, line_no, &active_macros);
  }

  std::string ExpandText(std::string_view text, const std::string& file, int line_no,
                         std::unordered_set<std::string>* active_macros) const {
    std::ostringstream out;
    std::size_t i = 0;
    while (i < text.size()) {
      const char c = text[i];
      if (c == '"' || c == '\'') {
        const char quote = c;
        out << c;
        ++i;
        while (i < text.size()) {
          const char qc = text[i];
          out << qc;
          ++i;
          if (qc == '\\' && i < text.size()) {
            out << text[i];
            ++i;
            continue;
          }
          if (qc == quote) {
            break;
          }
        }
        continue;
      }

      if (IsIdentStart(c)) {
        std::size_t j = i + 1;
        while (j < text.size() && IsIdentContinue(text[j])) {
          ++j;
        }

        const std::string ident(text.substr(i, j - i));
        if (ident == "__FILE__") {
          out << '"' << file << '"';
        } else if (ident == "__DIR__") {
          out << '"' << DirName(file) << '"';
        } else if (ident == "__DATE__") {
          out << "\"1970-01-01\"";
        } else if (ident == "__TIME__") {
          out << "\"00:00:00\"";
        } else if (ident == "__LINE__") {
          out << line_no;
        } else if (ident == "__CMD_LINE__") {
          out << 0;
        } else {
          const auto it = macros_.find(ident);
          if (it != macros_.end()) {
            if (active_macros->find(ident) != active_macros->end()) {
              out << ident;
              i = j;
              continue;
            }

            const MacroDef& def = it->second;
            if (def.function_like) {
              std::size_t open = j;
              while (open < text.size() &&
                     std::isspace(static_cast<unsigned char>(text[open])) != 0) {
                ++open;
              }
              if (open >= text.size() || text[open] != '(') {
                out << ident;
                i = j;
                continue;
              }

              std::size_t call_end = 0;
              const std::vector<std::string> args =
                  ParseMacroCallArgs(text, open, file, line_no, &call_end);
              if (args.size() != def.params.size()) {
                Fail("HC1032", file, line_no,
                     "wrong argument count for macro " + ident + " (expected " +
                         std::to_string(def.params.size()) + ", got " +
                         std::to_string(args.size()) + ")");
              }

              const std::string substituted = SubstituteMacroParams(def, args);
              active_macros->insert(ident);
              out << ExpandText(substituted, file, line_no, active_macros);
              active_macros->erase(ident);
              i = call_end + 1;
              continue;
            }

            active_macros->insert(ident);
            out << ExpandText(def.body, file, line_no, active_macros);
            active_macros->erase(ident);
          } else {
            out << ident;
          }
        }
        i = j;
        continue;
      }

      out << c;
      ++i;
    }

    return out.str();
  }

  static std::vector<std::string> ParseMacroCallArgs(std::string_view text,
                                                     std::size_t open_paren,
                                                     const std::string& file, int line_no,
                                                     std::size_t* out_close_paren) {
    if (open_paren >= text.size() || text[open_paren] != '(') {
      Fail("HC1033", file, line_no, "internal macro call parse error");
    }

    std::vector<std::string> args;
    std::string current;
    int depth = 1;
    bool saw_any = false;
    std::size_t i = open_paren + 1;
    while (i < text.size()) {
      const char c = text[i];

      if (c == '"' || c == '\'') {
        saw_any = true;
        const char quote = c;
        current.push_back(c);
        ++i;
        while (i < text.size()) {
          const char qc = text[i];
          current.push_back(qc);
          ++i;
          if (qc == '\\' && i < text.size()) {
            current.push_back(text[i]);
            ++i;
            continue;
          }
          if (qc == quote) {
            break;
          }
        }
        continue;
      }

      if (c == '(') {
        saw_any = true;
        ++depth;
        current.push_back(c);
        ++i;
        continue;
      }
      if (c == ')') {
        --depth;
        if (depth == 0) {
          *out_close_paren = i;
          const std::string trimmed = Trim(current);
          if (saw_any) {
            args.push_back(trimmed);
          }
          return args;
        }
        current.push_back(c);
        ++i;
        continue;
      }
      if (c == ',' && depth == 1) {
        args.push_back(Trim(current));
        current.clear();
        saw_any = true;
        ++i;
        continue;
      }

      if (std::isspace(static_cast<unsigned char>(c)) == 0) {
        saw_any = true;
      }
      current.push_back(c);
      ++i;
    }

    Fail("HC1034", file, line_no, "unterminated macro invocation");
  }

  static std::string SubstituteMacroParams(const MacroDef& def,
                                           const std::vector<std::string>& args) {
    std::unordered_map<std::string, std::string> param_map;
    for (std::size_t i = 0; i < def.params.size(); ++i) {
      param_map[def.params[i]] = args[i];
    }

    std::ostringstream out;
    std::size_t i = 0;
    while (i < def.body.size()) {
      const char c = def.body[i];
      if (c == '"' || c == '\'') {
        const char quote = c;
        out << c;
        ++i;
        while (i < def.body.size()) {
          const char qc = def.body[i];
          out << qc;
          ++i;
          if (qc == '\\' && i < def.body.size()) {
            out << def.body[i];
            ++i;
            continue;
          }
          if (qc == quote) {
            break;
          }
        }
        continue;
      }

      if (IsIdentStart(c)) {
        std::size_t j = i + 1;
        while (j < def.body.size() && IsIdentContinue(def.body[j])) {
          ++j;
        }
        const std::string ident = def.body.substr(i, j - i);
        const auto it = param_map.find(ident);
        if (it != param_map.end()) {
          out << it->second;
        } else {
          out << ident;
        }
        i = j;
        continue;
      }

      out << c;
      ++i;
    }

    return out.str();
  }

  std::string EvaluateExe(const std::string& expr, const std::string& file, int line_no) const {
    const std::string source = Trim(expr);
    std::size_t pos = 0;
    std::ostringstream output;

    auto skip_ws = [&]() {
      while (pos < source.size() && std::isspace(static_cast<unsigned char>(source[pos])) != 0) {
        ++pos;
      }
    };

    auto at_end = [&]() { return pos >= source.size(); };

    auto parse_identifier = [&]() -> std::string {
      skip_ws();
      if (at_end() || !IsIdentStart(source[pos])) {
        Fail("HC1020", file, line_no, "expected identifier in #exe block");
      }
      const std::size_t begin = pos++;
      while (pos < source.size() && IsIdentContinue(source[pos])) {
        ++pos;
      }
      return source.substr(begin, pos - begin);
    };

    auto parse_balanced = [&](char open, char close, const std::string& context) -> std::string {
      skip_ws();
      if (at_end() || source[pos] != open) {
        Fail("HC1020", file, line_no, "expected '" + std::string(1, open) + "' for " + context);
      }
      ++pos;
      std::ostringstream inner;
      int depth = 1;
      while (pos < source.size()) {
        const char c = source[pos++];
        if (c == '"' || c == '\'') {
          inner << c;
          const char quote = c;
          while (pos < source.size()) {
            const char qc = source[pos++];
            inner << qc;
            if (qc == '\\' && pos < source.size()) {
              inner << source[pos++];
              continue;
            }
            if (qc == quote) {
              break;
            }
          }
          continue;
        }
        if (c == open) {
          ++depth;
          inner << c;
          continue;
        }
        if (c == close) {
          --depth;
          if (depth == 0) {
            return inner.str();
          }
          inner << c;
          continue;
        }
        inner << c;
      }
      Fail("HC1020", file, line_no, "unterminated " + context + " in #exe block");
    };

    auto split_args = [&](const std::string& payload) {
      std::vector<std::string> args;
      std::string current;
      int paren_depth = 0;
      int bracket_depth = 0;
      int brace_depth = 0;
      bool saw_token = false;
      for (std::size_t i = 0; i < payload.size(); ++i) {
        const char c = payload[i];
        if (c == '"' || c == '\'') {
          saw_token = true;
          const char quote = c;
          current.push_back(c);
          ++i;
          while (i < payload.size()) {
            const char qc = payload[i];
            current.push_back(qc);
            if (qc == '\\' && i + 1 < payload.size()) {
              current.push_back(payload[++i]);
            } else if (qc == quote) {
              break;
            }
            ++i;
          }
          continue;
        }
        if (c == '(') {
          ++paren_depth;
          saw_token = true;
          current.push_back(c);
          continue;
        }
        if (c == ')') {
          --paren_depth;
          saw_token = true;
          current.push_back(c);
          continue;
        }
        if (c == '[') {
          ++bracket_depth;
          saw_token = true;
          current.push_back(c);
          continue;
        }
        if (c == ']') {
          --bracket_depth;
          saw_token = true;
          current.push_back(c);
          continue;
        }
        if (c == '{') {
          ++brace_depth;
          saw_token = true;
          current.push_back(c);
          continue;
        }
        if (c == '}') {
          --brace_depth;
          saw_token = true;
          current.push_back(c);
          continue;
        }
        if (c == ',' && paren_depth == 0 && bracket_depth == 0 && brace_depth == 0) {
          const std::string trimmed_arg = Trim(current);
          if (trimmed_arg.empty()) {
            Fail("HC1020", file, line_no, "empty argument in #exe call");
          }
          args.push_back(trimmed_arg);
          current.clear();
          saw_token = false;
          continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) == 0) {
          saw_token = true;
        }
        current.push_back(c);
      }
      const std::string tail = Trim(current);
      if (!tail.empty()) {
        args.push_back(tail);
      } else if (saw_token) {
        Fail("HC1020", file, line_no, "empty trailing argument in #exe call");
      }
      return args;
    };

    auto parse_concatenated_string_literals = [&](std::string_view text,
                                                  std::string* out) -> bool {
      out->clear();
      std::size_t i = 0;
      auto skip_local_ws = [&]() {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
          ++i;
        }
      };

      bool saw_literal = false;
      skip_local_ws();
      while (i < text.size() && text[i] == '"') {
        saw_literal = true;
        ++i;
        std::ostringstream raw;
        while (i < text.size()) {
          const char c = text[i++];
          if (c == '\\' && i < text.size()) {
            raw << c << text[i++];
            continue;
          }
          if (c == '"') {
            break;
          }
          raw << c;
        }
        out->append(Unescape(raw.str()));
        skip_local_ws();
      }

      return saw_literal && i == text.size();
    };

    auto evaluate_stream_arg = [&](const std::string& arg) {
      std::unordered_set<std::string> active_macros;
      const std::string expanded = Trim(ExpandText(arg, file, line_no, &active_macros));
      if (expanded.empty()) {
        return std::string();
      }
      std::string concatenated;
      if (parse_concatenated_string_literals(expanded, &concatenated)) {
        return concatenated;
      }
      return expanded;
    };

    auto evaluate_condition = [&](const std::string& condition) {
      std::unordered_set<std::string> active_macros;
      const std::string expanded = ExpandText(condition, file, line_no, &active_macros);
      return EvalIfExprValue(expanded, 0) != 0;
    };

    auto match_keyword = [&](std::string_view keyword) -> bool {
      skip_ws();
      if (source.substr(pos, keyword.size()) != keyword) {
        return false;
      }
      const std::size_t next = pos + keyword.size();
      if (next < source.size() && IsIdentContinue(source[next])) {
        return false;
      }
      pos = next;
      return true;
    };

    std::function<void(bool)> parse_stmt;
    std::function<void(bool)> parse_block;

    parse_block = [&](bool execute) {
      skip_ws();
      if (at_end() || source[pos] != '{') {
        Fail("HC1018", file, line_no, "#exe requires a braced block body");
      }
      ++pos;
      while (true) {
        skip_ws();
        if (at_end()) {
          Fail("HC1018", file, line_no, "unterminated #exe block");
        }
        if (source[pos] == '}') {
          ++pos;
          return;
        }
        parse_stmt(execute);
      }
    };

    parse_stmt = [&](bool execute) {
      skip_ws();
      if (at_end()) {
        Fail("HC1018", file, line_no, "unterminated #exe block");
      }

      if (source[pos] == ';') {
        ++pos;
        return;
      }
      if (source[pos] == '{') {
        parse_block(execute);
        return;
      }

      if (match_keyword("if")) {
        const std::string condition = parse_balanced('(', ')', "if condition");
        const bool condition_true = execute ? evaluate_condition(condition) : false;
        parse_stmt(execute && condition_true);
        if (match_keyword("else")) {
          parse_stmt(execute && !condition_true);
        }
        return;
      }

      const std::string callee = parse_identifier();
      const std::string args_payload = parse_balanced('(', ')', "call arguments");
      const std::vector<std::string> args = split_args(args_payload);
      skip_ws();
      if (at_end() || source[pos] != ';') {
        Fail("HC1024", file, line_no, "#exe call must end with ';'");
      }
      ++pos;
      if (!execute) {
        return;
      }

      if (callee == "StreamPrint" || callee == "StreamDoc" || callee == "StreamExePrint") {
        if (args.size() != 1) {
          Fail("HC1025", file, line_no,
               callee + " in #exe currently supports a single argument");
        }
        output << evaluate_stream_arg(args.front());
        return;
      }
      if (callee == "Option" || callee == "Cd") {
        return;
      }

      Fail("HC1019", file, line_no, "unsupported #exe call: " + callee);
    };

    parse_block(true);
    skip_ws();
    if (!at_end()) {
      Fail("HC1024", file, line_no, "trailing tokens after #exe block");
    }
    return output.str();
  }

  static std::string Unescape(const std::string& escaped) {
    std::ostringstream out;
    for (std::size_t i = 0; i < escaped.size(); ++i) {
      const char c = escaped[i];
      if (c != '\\' || i + 1 >= escaped.size()) {
        out << c;
        continue;
      }

      const char n = escaped[++i];
      switch (n) {
        case 'n':
          out << '\n';
          break;
        case 't':
          out << '\t';
          break;
        case '\\':
          out << '\\';
          break;
        case '"':
          out << '"';
          break;
        default:
          out << n;
          break;
      }
    }
    return out.str();
  }

  static std::string ExtractQuoted(const std::string& text, const std::string& file,
                                   int line_no, std::string_view directive) {
    const std::size_t first = text.find('"');
    const std::size_t last = text.rfind('"');
    if (first == std::string::npos || last == std::string::npos || first == last) {
      ThrowDiagnostic(Diagnostic{
          "HC1022",
          DiagnosticSeverity::kError,
          file,
          line_no,
          1,
          std::string(directive) + " expects quoted path",
          "use " + std::string(directive) + " \"relative/path\"",
      });
    }
    return text.substr(first + 1, last - first - 1);
  }

  static bool IsIdentStart(char c) {
    return (std::isalpha(static_cast<unsigned char>(c)) != 0) || c == '_';
  }

  static bool IsIdentContinue(char c) {
    return IsIdentStart(c) || (std::isdigit(static_cast<unsigned char>(c)) != 0);
  }

  static bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
  }

  static bool IsExeDirectiveLine(std::string_view line) {
    if (!StartsWith(line, "#exe")) {
      return false;
    }
    if (line.size() == 4) {
      return true;
    }
    const char next = line[4];
    return std::isspace(static_cast<unsigned char>(next)) != 0 || next == '{';
  }

  static bool HasClosedExeBody(std::string_view line) {
    std::size_t i = 4;
    if (line.size() < 4) {
      return false;
    }
    bool saw_open = false;
    int depth = 0;
    while (i < line.size()) {
      const char c = line[i++];
      if (c == '"' || c == '\'') {
        const char quote = c;
        while (i < line.size()) {
          const char qc = line[i++];
          if (qc == '\\' && i < line.size()) {
            ++i;
            continue;
          }
          if (qc == quote) {
            break;
          }
        }
        continue;
      }
      if (c == '{') {
        saw_open = true;
        ++depth;
        continue;
      }
      if (c == '}' && depth > 0) {
        --depth;
      }
    }
    return saw_open && depth == 0;
  }

  static std::string LTrim(std::string text) {
    text.erase(text.begin(),
               std::find_if(text.begin(), text.end(), [](char c) {
                 return std::isspace(static_cast<unsigned char>(c)) == 0;
               }));
    return text;
  }

  static std::string Trim(std::string text) {
    text = LTrim(std::move(text));
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
      text.pop_back();
    }
    return text;
  }

  static std::string DirName(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) {
      return ".";
    }
    if (slash == 0) {
      return path.substr(0, 1);
    }
    return path.substr(0, slash);
  }

  static std::string JoinPath(const std::string& base, const std::string& leaf) {
    if (leaf.empty()) {
      return base;
    }
    if (leaf[0] == '/' || leaf[0] == '\\') {
      return leaf;
    }
    if (base.empty() || base == ".") {
      return leaf;
    }
    if (base.back() == '/' || base.back() == '\\') {
      return base + leaf;
    }
    return base + "/" + leaf;
  }

  std::string ResolveIncludePath(const std::string& including_file, const std::string& target,
                                 int line_no) const {
    std::vector<std::string> roots;
    roots.push_back(DirName(including_file));
    roots.insert(roots.end(), include_dirs_.begin(), include_dirs_.end());

    for (const std::string& root : roots) {
      const std::string candidate = JoinPath(root, target);
      std::error_code ec;
      const bool exists =
          std::filesystem::exists(std::filesystem::path(candidate), ec) && !ec;
      if (exists) {
        return candidate;
      }
    }

    std::ostringstream remediation;
    remediation << "searched include roots in order:";
    for (const std::string& root : roots) {
      remediation << " " << root;
    }
    Fail("HC1007", including_file, line_no, "include not found: " + target, remediation.str());
  }

  static std::string CanonicalPath(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
      return canonical.string();
    }
    return path;
  }

  bool jit_mode_;
  std::vector<std::string> include_dirs_;
  std::unordered_map<std::string, MacroDef> macros_;
};

}  // namespace

std::string RunPreprocessor(std::string_view source, std::string_view filename,
                            ExecutionMode mode) {
  Preprocessor preprocessor(mode == ExecutionMode::kJit);
  return preprocessor.Process(source, filename);
}

}  // namespace holyc::frontend::internal

#include "parser.h"

#include "diagnostics.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace holyc::frontend::internal {

namespace {
using Node = ParsedNode;

enum class TokenKind {
  kIdentifier,
  kKeyword,
  kNumber,
  kString,
  kChar,
  kPunct,
  kEnd,
};

struct Token {
  TokenKind kind;
  std::string text;
  int line;
  int column;
};

class Lexer {
 public:
  Lexer(std::string_view source, std::string_view filename)
      : source_(source), filename_(filename), pos_(0), line_(1), col_(1) {}

  std::vector<Token> Tokenize() {
    std::vector<Token> out;
    while (!AtEnd()) {
      SkipWhitespaceAndComments();
      if (AtEnd()) {
        break;
      }

      const int start_line = line_;
      const int start_col = col_;
      const char c = Peek();

      if (IsIdentStart(c)) {
        out.push_back(MakeIdentifierOrKeyword(start_line, start_col));
        continue;
      }

      if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
        out.push_back(MakeNumber(start_line, start_col));
        continue;
      }

      if (c == '"') {
        out.push_back(MakeString(start_line, start_col));
        continue;
      }

      if (c == '\'') {
        out.push_back(MakeChar(start_line, start_col));
        continue;
      }

      out.push_back(MakePunct(start_line, start_col));
    }

    out.push_back(Token{TokenKind::kEnd, "", line_, col_});
    return out;
  }

 private:
  bool AtEnd() const { return pos_ >= source_.size(); }

  char Peek() const { return source_[pos_]; }

  char PeekNext() const {
    if (pos_ + 1 >= source_.size()) {
      return '\0';
    }
    return source_[pos_ + 1];
  }

  void Advance() {
    if (AtEnd()) {
      return;
    }
    if (source_[pos_] == '\n') {
      ++line_;
      col_ = 1;
    } else {
      ++col_;
    }
    ++pos_;
  }

  void SkipWhitespaceAndComments() {
    while (!AtEnd()) {
      const char c = Peek();
      if (std::isspace(static_cast<unsigned char>(c)) != 0) {
        Advance();
        continue;
      }

      if (c == '/' && PeekNext() == '/') {
        while (!AtEnd() && Peek() != '\n') {
          Advance();
        }
        continue;
      }

      if (c == '/' && PeekNext() == '*') {
        const int comment_line = line_;
        const int comment_col = col_;
        Advance();
        Advance();
        bool terminated = false;
        while (!AtEnd()) {
          if (Peek() == '*' && PeekNext() == '/') {
            Advance();
            Advance();
            terminated = true;
            break;
          }
          Advance();
        }
        if (!terminated) {
          throw std::runtime_error(
              FormatError("HC2003", comment_line, comment_col, "unterminated block comment"));
        }
        continue;
      }

      break;
    }
  }

  static bool IsIdentStart(char c) {
    return (std::isalpha(static_cast<unsigned char>(c)) != 0) || c == '_';
  }

  static bool IsIdentContinue(char c) {
    return IsIdentStart(c) || (std::isdigit(static_cast<unsigned char>(c)) != 0);
  }

  Token MakeIdentifierOrKeyword(int line, int col) {
    const std::size_t start = pos_;
    while (!AtEnd() && IsIdentContinue(Peek())) {
      Advance();
    }
    const std::string text(source_.substr(start, pos_ - start));
    static const std::unordered_set<std::string> kKeywords = {
        "U0",      "I8",      "U8",      "I16",      "U16",      "I32",
        "U32",     "I64",     "U64",     "F64",      "Bool",     "class",
        "union",   "if",      "else",    "for",      "while",    "do",
        "switch",  "case",    "start",   "end",      "break",    "goto",
        "return",  "try",     "catch",   "throw",    "lock",     "public",
        "extern",  "import",  "_extern", "_import",  "export",   "_export",
        "interrupt", "noreg",
        "reg",     "no_warn", "lastclass", "static", "typedef", "asm"};

    const TokenKind kind = (kKeywords.find(text) != kKeywords.end())
                               ? TokenKind::kKeyword
                               : TokenKind::kIdentifier;
    return Token{kind, text, line, col};
  }

  Token MakeNumber(int line, int col) {
    const std::size_t start = pos_;
    bool seen_dot = false;

    while (!AtEnd()) {
      const char c = Peek();
      if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_') {
        Advance();
        continue;
      }

      if (c == '.') {
        if (PeekNext() == '.') {
          break;
        }
        if (seen_dot) {
          break;
        }
        seen_dot = true;
        Advance();
        continue;
      }

      break;
    }
    return Token{TokenKind::kNumber, std::string(source_.substr(start, pos_ - start)),
                 line, col};
  }

  Token MakeString(int line, int col) {
    std::string text;
    text.push_back(Peek());
    Advance();

    while (!AtEnd()) {
      const char c = Peek();
      text.push_back(c);
      Advance();

      if (c == '\\' && !AtEnd()) {
        text.push_back(Peek());
        Advance();
        continue;
      }

      if (c == '"') {
        return Token{TokenKind::kString, text, line, col};
      }
    }

    throw std::runtime_error(
        FormatError("HC2001", line, col, "unterminated string literal"));
  }

  Token MakeChar(int line, int col) {
    std::string text;
    text.push_back(Peek());
    Advance();

    while (!AtEnd()) {
      const char c = Peek();
      text.push_back(c);
      Advance();

      if (c == '\\' && !AtEnd()) {
        text.push_back(Peek());
        Advance();
        continue;
      }

      if (c == '\'') {
        return Token{TokenKind::kChar, text, line, col};
      }
    }

    throw std::runtime_error(
        FormatError("HC2002", line, col, "unterminated char literal"));
  }

  Token MakePunct(int line, int col) {
    static const std::vector<std::string> kMulti = {
        "...", "<<=", ">>=", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>",
        "++",  "--", "->",  "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
        "::"};

    const auto it =
        std::find_if(kMulti.begin(), kMulti.end(), [this](const std::string& punct) {
          return source_.substr(pos_, punct.size()) == punct;
        });
    if (it != kMulti.end()) {
      pos_ += it->size();
      col_ += static_cast<int>(it->size());
      return Token{TokenKind::kPunct, *it, line, col};
    }

    const char c = Peek();
    Advance();
    return Token{TokenKind::kPunct, std::string(1, c), line, col};
  }

  std::string FormatError(std::string_view code, int line, int col, std::string_view message,
                          std::string remediation = std::string()) const {
    return FormatDiagnostic(
        Diagnostic{std::string(code), DiagnosticSeverity::kError, filename_, line, col,
                   std::string(message), std::move(remediation)});
  }

  std::string_view source_;
  std::string filename_;
  std::size_t pos_;
  int line_;
  int col_;
};

class Parser {
 public:
  Parser(std::vector<Token> tokens, std::string_view filename)
      : tokens_(std::move(tokens)), filename_(filename), idx_(0), anon_aggregate_counter_(0) {}

  Node ParseProgram() {
    Node program{"Program", std::string(filename_), {}};
    while (!IsEnd()) {
      program.children.push_back(ParseTopLevel());
    }
    return program;
  }

 private:
  bool IsEnd() const { return Peek().kind == TokenKind::kEnd; }

  const Token& Peek(int offset = 0) const {
    const std::size_t want = idx_ + static_cast<std::size_t>(offset);
    if (want >= tokens_.size()) {
      return tokens_.back();
    }
    return tokens_[want];
  }

  const Token& Advance() {
    const Token& current = Peek();
    if (!IsEnd()) {
      ++idx_;
    }
    return current;
  }

  bool Match(std::string_view text) {
    if (Peek().text == text) {
      Advance();
      return true;
    }
    return false;
  }

  const Token& Expect(std::string_view text) {
    if (Peek().text != text) {
      throw std::runtime_error(TokenError(Peek(), std::string("expected '") +
                                                     std::string(text) + "'"));
    }
    return Advance();
  }

  Node ParseTopLevel() {
    if (LooksLikeFunctionDecl()) {
      return ParseFunctionDecl();
    }
    return ParseStatement();
  }

  bool LooksLikeFunctionDecl() const {
    if (Peek().kind == TokenKind::kEnd) {
      return false;
    }

    std::size_t i = idx_;
    bool saw_type = false;
    bool saw_name = false;

    while (i < tokens_.size()) {
      const Token& t = tokens_[i];
      if (t.kind == TokenKind::kKeyword) {
        saw_type = true;
        ++i;
        continue;
      }

      if (t.kind == TokenKind::kIdentifier) {
        if (!saw_type) {
          saw_type = true;
          ++i;
          continue;
        }
        saw_name = true;
        ++i;
        break;
      }

      if (t.text == "*" || t.text == "&") {
        ++i;
        continue;
      }

      return false;
    }

    if (!saw_type || !saw_name || i >= tokens_.size() || tokens_[i].text != "(") {
      return false;
    }

    int depth = 0;
    while (i < tokens_.size()) {
      if (tokens_[i].text == "(") {
        ++depth;
      } else if (tokens_[i].text == ")") {
        --depth;
        if (depth == 0) {
          ++i;
          break;
        }
      }
      ++i;
    }

    if (i >= tokens_.size()) {
      return false;
    }
    return tokens_[i].text == "{" || tokens_[i].text == ";";
  }

  Node ParseFunctionDecl() {
    Node fn{"FunctionDecl", "", {}};

    std::vector<std::string> sig;
    while (Peek().kind != TokenKind::kEnd && Peek().text != "(") {
      sig.push_back(Advance().text);
    }

    if (sig.empty()) {
      throw std::runtime_error(TokenError(Peek(), "expected function signature"));
    }

    fn.text = Join(sig, " ");
    Expect("(");
    fn.children.push_back(ParseParamList());
    Expect(")");

    if (Match(";")) {
      AttachDeclParts(&fn, sig);
      return fn;
    }

    fn.children.push_back(ParseBlock());
    AttachDeclParts(&fn, sig);
    return fn;
  }

  Node ParseParamList() {
    Node params{"ParamList", "", {}};

    if (Peek().text == ")") {
      return params;
    }

    while (!IsEnd()) {
      if (Peek().text == ")") {
        break;
      }

      std::vector<std::string> left;
      std::vector<Token> right;
      bool has_default = false;
      int nested = 0;

      while (!IsEnd()) {
        if (Peek().text == "(" || Peek().text == "[" || Peek().text == "{") {
          ++nested;
          left.push_back(Advance().text);
          continue;
        }
        if (Peek().text == ")" || Peek().text == "]" || Peek().text == "}") {
          if (nested == 0 && Peek().text == ")") {
            break;
          }
          if (nested > 0) {
            --nested;
          }
          left.push_back(Advance().text);
          continue;
        }
        if (nested == 0 && Peek().text == ",") {
          break;
        }
        if (nested == 0 && Peek().text == "=") {
          has_default = true;
          Advance();
          break;
        }
        left.push_back(Advance().text);
      }

      nested = 0;
      while (has_default && !IsEnd()) {
        if (Peek().text == "(" || Peek().text == "[" || Peek().text == "{") {
          ++nested;
          right.push_back(Advance());
          continue;
        }
        if (Peek().text == ")" || Peek().text == "]" || Peek().text == "}") {
          if (nested == 0 && Peek().text == ")") {
            break;
          }
          if (nested > 0) {
            --nested;
          }
          right.push_back(Advance());
          continue;
        }
        if (nested == 0 && Peek().text == ",") {
          break;
        }
        right.push_back(Advance());
      }

      Node param{"Param", Join(left, " "), {}};
      AttachDeclParts(&param, left);
      if (has_default) {
        if (right.empty()) {
          throw std::runtime_error(TokenError(Peek(), "expected default argument expression"));
        }
        Node default_expr = ParseExpressionFromTokens(right);
        Node default_node{"Default", JoinTokens(right, " "), {}};
        default_node.children.push_back(std::move(default_expr));
        param.children.push_back(std::move(default_node));
      }
      params.children.push_back(param);

      if (!Match(",")) {
        break;
      }
    }

    return params;
  }

  Node ParseBlock() {
    Node block{"Block", "", {}};
    Expect("{");
    while (!IsEnd() && Peek().text != "}") {
      block.children.push_back(ParseStatement());
    }
    Expect("}");
    return block;
  }

  Node ParseStatement() {
    if (Peek().text == "{") {
      return ParseBlock();
    }

    if (Match(";")) {
      return Node{"EmptyStmt", "", {}};
    }

    if (Match("typedef")) {
      return ParseTypeAliasDecl();
    }

    if (IsLinkageKeyword(Peek().text)) {
      return ParseLinkageDecl();
    }

    if (LooksLikeVarDecl()) {
      return ParseVarDecl();
    }

    if (Match("switch")) {
      return ParseSwitch();
    }

    if (Match("case")) {
      return ParseCase();
    }

    if (Match("default")) {
      Node n{"DefaultClause", "", {}};
      Expect(":");
      n.children.push_back(ParseStatement());
      return n;
    }

    if (Match("start")) {
      Expect(":");
      return Node{"StartLabel", "start", {}};
    }

    if (Match("end")) {
      Expect(":");
      return Node{"EndLabel", "end", {}};
    }

    if (Match("if")) {
      Node n{"IfStmt", "", {}};
      Expect("(");
      n.children.push_back(ParseExpression());
      Expect(")");
      n.children.push_back(ParseStatement());
      if (Match("else")) {
        n.children.push_back(ParseStatement());
      }
      return n;
    }

    if (Match("for")) {
      Node n{"ForStmt", "", {}};
      Expect("(");
      if (Peek().text != ";") {
        n.children.push_back(ParseExpression());
      } else {
        n.children.push_back(Node{"Init", "", {}});
      }
      Expect(";");
      if (Peek().text != ";") {
        n.children.push_back(ParseExpression());
      } else {
        n.children.push_back(Node{"Cond", "", {}});
      }
      Expect(";");
      if (Peek().text != ")") {
        n.children.push_back(ParseExpression());
      } else {
        n.children.push_back(Node{"Inc", "", {}});
      }
      Expect(")");
      n.children.push_back(ParseStatement());
      return n;
    }

    if (Match("while")) {
      Node n{"WhileStmt", "", {}};
      Expect("(");
      n.children.push_back(ParseExpression());
      Expect(")");
      n.children.push_back(ParseStatement());
      return n;
    }

    if (Match("do")) {
      Node n{"DoWhileStmt", "", {}};
      n.children.push_back(ParseStatement());
      Expect("while");
      Expect("(");
      n.children.push_back(ParseExpression());
      Expect(")");
      Expect(";");
      return n;
    }

    if (Match("return")) {
      Node n{"ReturnStmt", "", {}};
      if (!Match(";")) {
        n.children.push_back(ParseExpression());
        Expect(";");
      }
      return n;
    }

    if (Match("break")) {
      Expect(";");
      return Node{"BreakStmt", "", {}};
    }

    if (Peek().text == "continue") {
      throw std::runtime_error(TokenError(Peek(), "HolyC has no continue; use goto"));
    }

    if (Match("goto")) {
      Node n{"GotoStmt", "", {}};
      n.text = Advance().text;
      Expect(";");
      return n;
    }

    if (Match("try")) {
      Node n{"TryStmt", "", {}};
      n.children.push_back(ParseStatement());
      Expect("catch");
      n.children.push_back(ParseStatement());
      return n;
    }

    if (Match("throw")) {
      Node n{"ThrowStmt", "", {}};
      Expect("(");
      n.children.push_back(ParseExpression());
      Expect(")");
      Expect(";");
      return n;
    }

    if (Match("lock")) {
      Node n{"LockStmt", "", {}};
      n.children.push_back(ParseStatement());
      return n;
    }

    if (Match("asm")) {
      return ParseInlineAsm();
    }

    if (Peek().kind == TokenKind::kKeyword && (Peek().text == "class" || Peek().text == "union")) {
      return ParseClassDecl();
    }

    if (Peek().kind == TokenKind::kString || Peek().kind == TokenKind::kChar) {
      return ParsePrintStmt();
    }

    if (Peek().kind == TokenKind::kIdentifier && Peek(1).text == ":") {
      const std::string label = Advance().text;
      Expect(":");
      Node n{"LabelStmt", label, {}};
      n.children.push_back(ParseStatement());
      return n;
    }

    Node stmt{"ExprStmt", "", {}};
    stmt.children.push_back(ParseExpression());
    Expect(";");
    return stmt;
  }

  Node ParsePrintStmt() {
    Node stmt{"PrintStmt", "", {}};
    stmt.children.push_back(ParseAssign());
    if (Peek().text != ";" && Peek().text != ",") {
      // HolyC permits an implicit second print expression after the first
      // literal, e.g. `"" fmt,*arg;` for dynamic format forwarding.
      stmt.children.push_back(ParseAssign());
    }
    while (Match(",")) {
      stmt.children.push_back(ParseAssign());
    }
    Expect(";");
    return stmt;
  }

  Node ParseClassDecl() {
    Node n{"ClassDecl", Advance().text, {}};
    if (Peek().kind == TokenKind::kIdentifier) {
      n.text += " " + Advance().text;
    }

    if (Match("{")) {
      while (!IsEnd() && Peek().text != "}") {
        if (Peek().kind == TokenKind::kKeyword &&
            (Peek().text == "class" || Peek().text == "union")) {
          n.children.push_back(ParseClassDecl());
          continue;
        }

        if (Match("typedef")) {
          n.children.push_back(ParseTypeAliasDecl());
          continue;
        }

        if (Match(";")) {
          continue;
        }

        std::vector<std::string> field_tokens;
        int nested = 0;
        while (!IsEnd()) {
          if (Peek().text == "{" || Peek().text == "(" || Peek().text == "[") {
            ++nested;
          } else if (Peek().text == "}" || Peek().text == ")" || Peek().text == "]") {
            if (nested == 0 && Peek().text == "}") {
              break;
            }
            if (nested > 0) {
              --nested;
            }
          }

          if (nested == 0 && Peek().text == ";") {
            break;
          }

          field_tokens.push_back(Advance().text);
        }

        if (!field_tokens.empty()) {
          n.children.push_back(BuildFieldDeclNode(field_tokens));
        }

        Match(";");
      }
      Expect("}");
    }

    if (!Match(";")) {
      std::string aggregate_name = ExtractAggregateName(n.text);
      if (aggregate_name.empty()) {
        ++anon_aggregate_counter_;
        aggregate_name = "__holyc_anon_aggregate_" + std::to_string(anon_aggregate_counter_);
        n.text += " " + aggregate_name;
      }

      while (!IsEnd()) {
        std::vector<std::string> decl_tokens;
        while (!IsEnd() && Peek().text != ";" && Peek().text != "," && Peek().text != "=") {
          decl_tokens.push_back(Advance().text);
        }
        if (decl_tokens.empty()) {
          throw std::runtime_error(TokenError(Peek(), "expected trailing declarator"));
        }

        std::vector<std::string> full_decl_tokens;
        full_decl_tokens.push_back(aggregate_name);
        full_decl_tokens.insert(full_decl_tokens.end(), decl_tokens.begin(), decl_tokens.end());

        Node trailing_decl{"VarDecl", Join(full_decl_tokens, " "), {}};
        AttachDeclParts(&trailing_decl, full_decl_tokens);
        if (Match("=")) {
          trailing_decl.children.push_back(ParseAssign());
        }
        n.children.push_back(std::move(trailing_decl));

        if (Match(",")) {
          continue;
        }
        Expect(";");
        break;
      }
    }

    return n;
  }

  Node ParseTypeAliasDecl() {
    Node decl{"TypeAliasDecl", "", {}};

    std::vector<std::string> parts;
    while (!IsEnd() && Peek().text != ";") {
      parts.push_back(Advance().text);
    }
    if (parts.size() < 2) {
      throw std::runtime_error(TokenError(Peek(), "expected typedef declaration"));
    }

    decl.text = Join(parts, " ");
    Expect(";");
    return decl;
  }

  Node ParseInlineAsm() {
    Node stmt{"AsmStmt", "", {}};

    if (Match("{")) {
      int depth = 1;
      std::vector<std::string> body_tokens;
      while (!IsEnd() && depth > 0) {
        if (Peek().text == "{") {
          ++depth;
        } else if (Peek().text == "}") {
          --depth;
          if (depth == 0) {
            Advance();
            break;
          }
        }
        body_tokens.push_back(Advance().text);
      }
      stmt.text = Join(body_tokens, " ");
      Match(";");
      return stmt;
    }

    Expect("(");
    while (!IsEnd() && Peek().text != ")") {
      std::vector<std::string> arg_tokens;
      int nested = 0;
      while (!IsEnd()) {
        if (Peek().text == "(" || Peek().text == "[" || Peek().text == "{") {
          ++nested;
        } else if (Peek().text == ")" || Peek().text == "]" || Peek().text == "}") {
          if (nested == 0 && Peek().text == ")") {
            break;
          }
          if (nested > 0) {
            --nested;
          }
        }

        if (nested == 0 && Peek().text == ",") {
          break;
        }

        arg_tokens.push_back(Advance().text);
      }

      if (!arg_tokens.empty()) {
        Node arg{"AsmArg", Join(arg_tokens, " "), {}};
        arg.children.push_back(ParseExpressionFromTokens(TokenizeArgTokens(arg_tokens)));
        stmt.children.push_back(std::move(arg));
      }

      if (!Match(",")) {
        break;
      }
    }

    Expect(")");
    Expect(";");
    if (!stmt.children.empty()) {
      stmt.text = stmt.children[0].text;
    }
    return stmt;
  }

  Node ParseLinkageDecl() {
    Node decl{"LinkageDecl", Advance().text, {}};

    std::vector<std::string> payload;
    while (!IsEnd() && Peek().text != ";") {
      payload.push_back(Advance().text);
    }
    if (payload.empty()) {
      throw std::runtime_error(TokenError(Peek(), "expected linkage declaration payload"));
    }
    decl.children.push_back(Node{"DeclSpec", Join(payload, " "), {}});
    Expect(";");
    return decl;
  }

  Node ParseVarDecl() {
    if (HasTopLevelCommaInDecl()) {
      return ParseVarDeclList();
    }

    Node decl{"VarDecl", "", {}};

    std::vector<std::string> left;
    while (!IsEnd() && Peek().text != ";" && Peek().text != "=") {
      left.push_back(Advance().text);
    }

    if (left.size() < 2) {
      throw std::runtime_error(TokenError(Peek(), "expected variable declaration"));
    }

    decl.text = Join(left, " ");
    AttachDeclParts(&decl, left);
    if (Match("=")) {
      decl.children.push_back(ParseExpression());
    }
    Expect(";");
    return decl;
  }

  Node ParseVarDeclList() {
    Node list{"VarDeclList", "", {}};
    std::vector<std::string> base_tokens;

    while (!IsEnd()) {
      std::vector<std::string> decl_tokens;
      while (!IsEnd() && Peek().text != ";" && Peek().text != "," && Peek().text != "=") {
        decl_tokens.push_back(Advance().text);
      }

      if (decl_tokens.empty()) {
        throw std::runtime_error(TokenError(Peek(), "expected variable declarator"));
      }

      std::vector<std::string> full_decl_tokens;
      if (base_tokens.empty()) {
        if (decl_tokens.size() < 2) {
          throw std::runtime_error(TokenError(Peek(), "expected variable declaration"));
        }
        base_tokens = ExtractBaseDeclTokensForList(decl_tokens);
        full_decl_tokens = decl_tokens;
      } else {
        full_decl_tokens = base_tokens;
        full_decl_tokens.insert(full_decl_tokens.end(), decl_tokens.begin(), decl_tokens.end());
      }

      Node decl{"VarDecl", Join(full_decl_tokens, " "), {}};
      AttachDeclParts(&decl, full_decl_tokens);
      if (Match("=")) {
        // In multi-declarator declarations, ',' separates declarators, so the
        // initializer must stop at assignment-expression precedence.
        decl.children.push_back(ParseAssign());
      }
      list.children.push_back(std::move(decl));

      if (Match(",")) {
        continue;
      }

      Expect(";");
      break;
    }

    return list;
  }

  bool HasTopLevelCommaInDecl() const {
    int depth = 0;
    for (std::size_t i = idx_; i < tokens_.size(); ++i) {
      const std::string& tok = tokens_[i].text;
      if (tok == "{" || tok == "(" || tok == "[") {
        ++depth;
      } else if (tok == "}" || tok == ")" || tok == "]") {
        if (depth > 0) {
          --depth;
        }
      }

      if (depth == 0 && tok == ";") {
        return false;
      }
      if (depth == 0 && tok == ",") {
        return true;
      }
    }
    return false;
  }

  Node ParseSwitch() {
    Node n{"SwitchStmt", "", {}};
    Expect("(");
    n.children.push_back(ParseExpression());
    Expect(")");
    n.children.push_back(ParseStatement());
    return n;
  }

  Node ParseCase() {
    Node n{"CaseClause", "", {}};
    if (Match(":")) {
      n.text = "null-case";
      n.children.push_back(ParseStatement());
      return n;
    }

    Node begin = ParseExpression();
    n.children.push_back(begin);
    if (Match("...")) {
      n.text = "range-case";
      n.children.push_back(ParseExpression());
    }
    Expect(":");
    n.children.push_back(ParseStatement());
    return n;
  }

  Node ParseExpression() { return ParseComma(); }

  Node ParseComma() {
    Node lhs = ParseAssign();
    while (Match(",")) {
      Node rhs = ParseAssign();
      Node merged{"CommaExpr", ",", {}};
      merged.children.push_back(std::move(lhs));
      merged.children.push_back(std::move(rhs));
      lhs = std::move(merged);
    }
    return lhs;
  }

  Node ParseAssign() {
    Node lhs = ParseLogicalOr();
    if (Match("?")) {
      throw std::runtime_error(TokenError(Peek(), "HolyC has no ?: operator"));
    }

    static const std::unordered_set<std::string> kAssignOps = {
        "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="};
    if (kAssignOps.find(Peek().text) != kAssignOps.end()) {
      const std::string op = Advance().text;
      Node rhs = ParseAssign();
      Node out{"AssignExpr", op, {}};
      out.children.push_back(std::move(lhs));
      out.children.push_back(std::move(rhs));
      return out;
    }
    return lhs;
  }

  Node ParseLogicalOr() {
    Node lhs = ParseLogicalAnd();
    while (Match("||")) {
      Node rhs = ParseLogicalAnd();
      Node out{"BinaryExpr", "||", {}};
      out.children.push_back(std::move(lhs));
      out.children.push_back(std::move(rhs));
      lhs = std::move(out);
    }
    return lhs;
  }

  Node ParseLogicalAnd() {
    Node lhs = ParseBitOr();
    while (Match("&&")) {
      Node rhs = ParseBitOr();
      Node out{"BinaryExpr", "&&", {}};
      out.children.push_back(std::move(lhs));
      out.children.push_back(std::move(rhs));
      lhs = std::move(out);
    }
    return lhs;
  }

  Node ParseBitOr() {
    Node lhs = ParseBitXor();
    while (Match("|")) {
      Node rhs = ParseBitXor();
      Node out{"BinaryExpr", "|", {}};
      out.children.push_back(std::move(lhs));
      out.children.push_back(std::move(rhs));
      lhs = std::move(out);
    }
    return lhs;
  }

  Node ParseBitXor() {
    Node lhs = ParseBitAnd();
    while (Match("^")) {
      Node rhs = ParseBitAnd();
      Node out{"BinaryExpr", "^", {}};
      out.children.push_back(std::move(lhs));
      out.children.push_back(std::move(rhs));
      lhs = std::move(out);
    }
    return lhs;
  }

  Node ParseBitAnd() {
    Node lhs = ParseEquality();
    while (Match("&")) {
      Node rhs = ParseEquality();
      Node out{"BinaryExpr", "&", {}};
      out.children.push_back(std::move(lhs));
      out.children.push_back(std::move(rhs));
      lhs = std::move(out);
    }
    return lhs;
  }

  Node ParseEquality() {
    Node lhs = ParseRelational();
    while (Peek().text == "==" || Peek().text == "!=") {
      const std::string op = Advance().text;
      Node rhs = ParseRelational();
      Node out{"BinaryExpr", op, {}};
      out.children.push_back(std::move(lhs));
      out.children.push_back(std::move(rhs));
      lhs = std::move(out);
    }
    return lhs;
  }

  Node ParseRelational() {
    Node lhs = ParseShift();
    while (Peek().text == "<" || Peek().text == ">" || Peek().text == "<=" ||
           Peek().text == ">=") {
      const std::string op = Advance().text;
      Node rhs = ParseShift();
      Node out{"BinaryExpr", op, {}};
      out.children.push_back(std::move(lhs));
      out.children.push_back(std::move(rhs));
      lhs = std::move(out);
    }
    return lhs;
  }

  Node ParseShift() {
    Node lhs = ParseAdd();
    while (Peek().text == "<<" || Peek().text == ">>") {
      const std::string op = Advance().text;
      Node rhs = ParseAdd();
      Node out{"BinaryExpr", op, {}};
      out.children.push_back(std::move(lhs));
      out.children.push_back(std::move(rhs));
      lhs = std::move(out);
    }
    return lhs;
  }

  Node ParseAdd() {
    Node lhs = ParseMul();
    while (Peek().text == "+" || Peek().text == "-") {
      const std::string op = Advance().text;
      Node rhs = ParseMul();
      Node out{"BinaryExpr", op, {}};
      out.children.push_back(std::move(lhs));
      out.children.push_back(std::move(rhs));
      lhs = std::move(out);
    }
    return lhs;
  }

  Node ParseMul() {
    Node lhs = ParseUnary();
    while (Peek().text == "*" || Peek().text == "/" || Peek().text == "%") {
      const std::string op = Advance().text;
      Node rhs = ParseUnary();
      Node out{"BinaryExpr", op, {}};
      out.children.push_back(std::move(lhs));
      out.children.push_back(std::move(rhs));
      lhs = std::move(out);
    }
    return lhs;
  }

  Node ParseUnary() {
    if (Peek().text == "+" || Peek().text == "-" || Peek().text == "!" || Peek().text == "~" ||
        Peek().text == "&" || Peek().text == "*" || Peek().text == "++" ||
        Peek().text == "--") {
      const std::string op = Advance().text;
      Node n{"UnaryExpr", op, {}};
      n.children.push_back(ParseUnary());
      return n;
    }

    if (LooksLikeCastType()) {
      Expect("(");
      std::vector<std::string> cast_type_tokens;
      while (!IsEnd() && Peek().text != ")") {
        cast_type_tokens.push_back(Advance().text);
      }
      Expect(")");
      Node n{"CastExpr", Join(cast_type_tokens, " "), {}};
      n.children.push_back(ParseUnary());
      return n;
    }

    return ParsePostfix();
  }

  Node ParsePostfix() {
    Node base = ParsePrimary();
    while (true) {
      if (Peek().text == "(" && LooksLikePostfixCast(base)) {
        Expect("(");
        std::vector<std::string> cast_type_tokens;
        while (!IsEnd() && Peek().text != ")") {
          cast_type_tokens.push_back(Advance().text);
        }
        Expect(")");
        Node out{"CastExpr", Join(cast_type_tokens, " "), {}};
        out.children.push_back(std::move(base));
        base = std::move(out);
        continue;
      }

      if (Match("(")) {
        Node call{"CallExpr", "", {std::move(base)}};
        call.children.push_back(ParseCallArgs());
        Expect(")");
        base = std::move(call);
        continue;
      }

      if (Peek().text == "." || Peek().text == "->") {
        Advance();
        const Token member = Advance();
        if (IsLaneSelector(member.text) && Match("[")) {
          Node out{"LaneExpr", member.text, {}};
          out.children.push_back(std::move(base));
          out.children.push_back(ParseExpression());
          Expect("]");
          base = std::move(out);
          continue;
        }
        Node out{"MemberExpr", member.text, {}};
        out.children.push_back(std::move(base));
        base = std::move(out);
        continue;
      }

      if (Match("[")) {
        Node out{"IndexExpr", "[]", {}};
        out.children.push_back(std::move(base));
        out.children.push_back(ParseExpression());
        Expect("]");
        base = std::move(out);
        continue;
      }

      if (Peek().text == "++" || Peek().text == "--") {
        const std::string op = Advance().text;
        Node out{"PostfixExpr", op, {}};
        out.children.push_back(std::move(base));
        base = std::move(out);
        continue;
      }

      break;
    }
    return base;
  }

  Node ParseCallArgs() {
    Node args{"CallArgs", "", {}};

    if (Peek().text == ")") {
      return args;
    }

    bool need_arg = true;
    while (!IsEnd() && Peek().text != ")") {
      if (Peek().text == ",") {
        args.children.push_back(Node{"EmptyArg", "", {}});
        Advance();
        need_arg = true;
        continue;
      }

      args.children.push_back(ParseAssign());
      need_arg = false;

      if (Match(",")) {
        need_arg = true;
        continue;
      }
      break;
    }

    if (need_arg && Peek().text == ")" && !args.children.empty()) {
      args.children.push_back(Node{"EmptyArg", "", {}});
    }

    return args;
  }

  Node ParsePrimary() {
    if (Match("(")) {
      Node n = ParseExpression();
      Expect(")");
      return n;
    }

    const Token tok = Advance();
    if (tok.kind == TokenKind::kIdentifier || tok.kind == TokenKind::kKeyword) {
      return Node{"Identifier", tok.text, {}};
    }

    if (tok.kind == TokenKind::kString) {
      std::string merged = tok.text;
      while (Peek().kind == TokenKind::kString) {
        const std::string next = Advance().text;
        if (!merged.empty() && !next.empty() && merged.back() == '"' && next.front() == '"') {
          merged.pop_back();
          merged.append(next.substr(1));
          continue;
        }
        merged.append(next);
      }
      return Node{"Literal", merged, {}};
    }

    if (tok.kind == TokenKind::kNumber || tok.kind == TokenKind::kChar) {
      return Node{"Literal", tok.text, {}};
    }

    if (tok.text == "$") {
      return Node{"DollarExpr", tok.text, {}};
    }

    throw std::runtime_error(TokenError(tok, "unexpected token in expression"));
  }

  static std::string Join(const std::vector<std::string>& parts, std::string_view sep) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (i > 0) {
        oss << sep;
      }
      oss << parts[i];
    }
    return oss.str();
  }

  static std::string JoinTokens(const std::vector<Token>& tokens, std::string_view sep) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      if (i > 0) {
        oss << sep;
      }
      oss << tokens[i].text;
    }
    return oss.str();
  }

  Node ParseExpressionFromTokens(const std::vector<Token>& tokens) {
    if (tokens.empty()) {
      throw std::runtime_error(TokenError(Peek(), "expected expression"));
    }

    std::vector<Token> inner = tokens;
    const Token& last = inner.back();
    inner.push_back(Token{TokenKind::kEnd, "", last.line, last.column});

    Parser nested(std::move(inner), filename_);
    Node expr = nested.ParseExpression();
    if (!nested.IsEnd()) {
      throw std::runtime_error(
          TokenError(nested.Peek(), "unexpected token in expression"));
    }
    return expr;
  }

  std::vector<Token> TokenizeArgTokens(const std::vector<std::string>& tokens) const {
    const std::string arg_text = Join(tokens, " ");
    Lexer lexer(arg_text, filename_);
    std::vector<Token> out = lexer.Tokenize();
    if (out.back().kind == TokenKind::kEnd) {
      out.pop_back();
    }
    return out;
  }

  std::string TokenError(const Token& token, std::string_view msg) const {
    return FormatDiagnostic(Diagnostic{
        "HC2100",
        DiagnosticSeverity::kError,
        filename_,
        token.line,
        token.column,
        std::string(msg),
        "",
    });
  }

  static bool IsBuiltinTypeKeyword(std::string_view text) {
    static const std::unordered_set<std::string> kTypes = {
        "U0", "I8", "U8", "I16", "U16", "I32", "U32", "I64", "U64", "F64", "Bool"};
    return kTypes.find(std::string(text)) != kTypes.end();
  }

  static bool IsDeclModifierKeyword(std::string_view text) {
    static const std::unordered_set<std::string> kDeclModifiers = {
        "extern",  "import", "_extern", "_import", "export", "_export", "public",
        "interrupt", "noreg",  "reg",     "no_warn", "static"};
    return kDeclModifiers.find(std::string(text)) != kDeclModifiers.end();
  }

  static bool IsLinkageKeyword(std::string_view text) {
    return text == "extern" || text == "import" || text == "_extern" || text == "_import" ||
           text == "export" || text == "_export";
  }

  static bool IsIdentifierToken(std::string_view token) {
    if (token.empty()) {
      return false;
    }
    const unsigned char first = static_cast<unsigned char>(token.front());
    if (!(std::isalpha(first) != 0 || token.front() == '_')) {
      return false;
    }
    return std::all_of(token.begin(), token.end(), [](char c) {
      const unsigned char uc = static_cast<unsigned char>(c);
      return (std::isalnum(uc) != 0) || c == '_';
    });
  }

  static bool IsLaneSelector(std::string_view text) {
    static const std::unordered_set<std::string> kLaneSelectors = {
        "i8",  "u8",  "i16", "u16", "i32", "u32", "i64", "u64",
        "I8",  "U8",  "I16", "U16", "I32", "U32", "I64", "U64"};
    return kLaneSelectors.find(std::string(text)) != kLaneSelectors.end();
  }

  void AttachDeclParts(Node* decl, const std::vector<std::string>& decl_tokens) const {
    if (decl == nullptr || decl_tokens.empty()) {
      return;
    }

    std::size_t name_index = std::string::npos;

    for (std::size_t i = 0; i + 3 < decl_tokens.size(); ++i) {
      if (decl_tokens[i] == "(" && (decl_tokens[i + 1] == "*" || decl_tokens[i + 1] == "&") &&
          IsIdentifierToken(decl_tokens[i + 2]) && decl_tokens[i + 3] == ")") {
        name_index = i + 2;
        break;
      }
    }

    if (name_index == std::string::npos) {
      for (std::size_t i = decl_tokens.size(); i > 0; --i) {
        const std::size_t idx = i - 1;
        if (!IsIdentifierToken(decl_tokens[idx])) {
          continue;
        }
        if (idx > 0 && decl_tokens[idx - 1] == "::") {
          continue;
        }
        name_index = idx;
        break;
      }
    }

    if (name_index == std::string::npos) {
      return;
    }

    auto name_it = decl_tokens.begin();
    std::advance(name_it,
                 static_cast<std::vector<std::string>::difference_type>(name_index));
    std::vector<std::string> type_tokens(decl_tokens.begin(), name_it);
    decl->children.push_back(Node{"DeclType", Join(type_tokens, " "), {}});
    decl->children.push_back(Node{"DeclName", decl_tokens[name_index], {}});
  }

  static std::vector<std::string> ExtractBaseDeclTokensForList(
      const std::vector<std::string>& first_decl_tokens) {
    if (first_decl_tokens.empty()) {
      return {};
    }

    std::size_t name_index = std::string::npos;
    for (std::size_t i = first_decl_tokens.size(); i > 0; --i) {
      const std::size_t idx = i - 1;
      if (IsIdentifierToken(first_decl_tokens[idx])) {
        name_index = idx;
        break;
      }
    }

    if (name_index == std::string::npos) {
      if (first_decl_tokens.size() == 1) {
        return first_decl_tokens;
      }
      return std::vector<std::string>(first_decl_tokens.begin(),
                                      first_decl_tokens.end() - 1);
    }

    std::size_t base_end = name_index;
    while (base_end > 0 &&
           (first_decl_tokens[base_end - 1] == "*" ||
            first_decl_tokens[base_end - 1] == "&")) {
      --base_end;
    }
    if (base_end == 0) {
      base_end = 1;
    }

    auto end_it = first_decl_tokens.begin();
    std::advance(end_it,
                 static_cast<std::vector<std::string>::difference_type>(base_end));
    return std::vector<std::string>(first_decl_tokens.begin(), end_it);
  }

  static std::string ExtractAggregateName(std::string_view class_text) {
    std::istringstream stream{std::string(class_text)};
    std::string keyword;
    std::string name;
    stream >> keyword;
    stream >> name;
    if ((keyword == "class" || keyword == "union") && !name.empty()) {
      return name;
    }
    return "";
  }

  Node BuildFieldDeclNode(const std::vector<std::string>& field_tokens) const {
    if (field_tokens.empty()) {
      return Node{"FieldDecl", "", {}};
    }

    std::size_t split = field_tokens.size();
    std::size_t name_index = std::string::npos;

    if (field_tokens.size() > 1 && IsIdentifierToken(field_tokens[1])) {
      name_index = 1;
    } else {
      for (std::size_t i = 0; i < field_tokens.size(); ++i) {
        if (!IsIdentifierToken(field_tokens[i])) {
          continue;
        }
        if (i > 0 && (field_tokens[i - 1] == "*" || field_tokens[i - 1] == "&" ||
                      field_tokens[i - 1] == "(")) {
          name_index = i;
          break;
        }
      }
    }

    if (name_index == std::string::npos) {
      for (std::size_t i = 0; i < field_tokens.size(); ++i) {
        if (IsIdentifierToken(field_tokens[i])) {
          name_index = i;
          break;
        }
      }
    }

    if (name_index != std::string::npos) {
      split = name_index + 1;
      while (split < field_tokens.size()) {
        if (field_tokens[split] == "[") {
          int depth = 0;
          while (split < field_tokens.size()) {
            if (field_tokens[split] == "[") {
              ++depth;
            } else if (field_tokens[split] == "]") {
              --depth;
              if (depth == 0) {
                ++split;
                break;
              }
            }
            ++split;
          }
          continue;
        }

        if (field_tokens[split] == "(") {
          int depth = 0;
          while (split < field_tokens.size()) {
            if (field_tokens[split] == "(") {
              ++depth;
            } else if (field_tokens[split] == ")") {
              --depth;
              if (depth == 0) {
                ++split;
                break;
              }
            }
            ++split;
          }
          continue;
        }

        break;
      }
    }

    if (split == field_tokens.size()) {
      Node field{"FieldDecl", Join(field_tokens, " "), {}};
      AttachDeclParts(&field, field_tokens);
      return field;
    }

    Node field{"FieldDecl", Join(field_tokens, " "), {}};
    auto split_it = field_tokens.begin();
    std::advance(split_it, static_cast<std::vector<std::string>::difference_type>(split));
    std::vector<std::string> decl_tokens(field_tokens.begin(), split_it);
    std::vector<std::string> meta_tokens(split_it, field_tokens.end());
    field.text = Join(decl_tokens, " ");
    AttachDeclParts(&field, decl_tokens);
    field.children.push_back(Node{"FieldMetaTokens", Join(meta_tokens, " "), {}});
    return field;
  }

  bool LooksLikeVarDecl() const {
    std::size_t i = idx_;
    while (i < tokens_.size() && tokens_[i].kind == TokenKind::kKeyword &&
           IsDeclModifierKeyword(tokens_[i].text)) {
      ++i;
    }

    if (i >= tokens_.size()) {
      return false;
    }

    if (!(tokens_[i].kind == TokenKind::kIdentifier ||
          (tokens_[i].kind == TokenKind::kKeyword &&
           (IsBuiltinTypeKeyword(tokens_[i].text) || tokens_[i].text == "class" ||
            tokens_[i].text == "union")))) {
      return false;
    }
    ++i;

    while (i < tokens_.size() && (tokens_[i].text == "*" || tokens_[i].text == "&")) {
      ++i;
    }

    if (i + 3 < tokens_.size() && tokens_[i].text == "(" &&
        (tokens_[i + 1].text == "*" || tokens_[i + 1].text == "&") &&
        tokens_[i + 2].kind == TokenKind::kIdentifier && tokens_[i + 3].text == ")") {
      i += 4;
      if (i >= tokens_.size() || tokens_[i].text != "(") {
        return false;
      }
      int depth = 0;
      while (i < tokens_.size()) {
        if (tokens_[i].text == "(") {
          ++depth;
        } else if (tokens_[i].text == ")") {
          --depth;
          if (depth == 0) {
            ++i;
            break;
          }
        }
        ++i;
      }
    } else if (i < tokens_.size() && tokens_[i].kind == TokenKind::kIdentifier) {
      ++i;
    } else {
      return false;
    }

    while (i < tokens_.size() && tokens_[i].text == "[") {
      int depth = 0;
      while (i < tokens_.size()) {
        if (tokens_[i].text == "[") {
          ++depth;
        } else if (tokens_[i].text == "]") {
          --depth;
          if (depth == 0) {
            ++i;
            break;
          }
        }
        ++i;
      }
    }

    return i < tokens_.size() &&
           (tokens_[i].text == ";" || tokens_[i].text == "=" || tokens_[i].text == ",");
  }

  bool LooksLikeCastType() const {
    if (Peek().text != "(") {
      return false;
    }

    std::size_t i = idx_ + 1;
    bool saw_any = false;
    while (i < tokens_.size()) {
      const Token& t = tokens_[i];
      if (t.text == ")") {
        const std::size_t next_i = i + 1;
        if (!saw_any || next_i >= tokens_.size()) {
          return false;
        }

        const Token& next = tokens_[next_i];
        if (next.text == "(" || next.text == "+" || next.text == "-" || next.text == "!" ||
            next.text == "~" || next.text == "&" || next.text == "*" ||
            next.text == "++" || next.text == "--") {
          return true;
        }
        return next.kind == TokenKind::kIdentifier || next.kind == TokenKind::kKeyword ||
               next.kind == TokenKind::kNumber || next.kind == TokenKind::kString ||
               next.kind == TokenKind::kChar;
      }
      if (t.text == "*" || t.text == "&" || t.text == "::") {
        ++i;
        continue;
      }
      if (t.kind == TokenKind::kIdentifier ||
          (t.kind == TokenKind::kKeyword &&
           (IsBuiltinTypeKeyword(t.text) || t.text == "class" || t.text == "union"))) {
        saw_any = true;
        ++i;
        continue;
      }
      return false;
    }
    return false;
  }

  bool LooksLikePostfixCast(const Node& base) const {
    if (Peek().text != "(") {
      return false;
    }

    std::size_t i = idx_ + 1;
    bool saw_any = false;
    bool saw_keyword_type = false;
    bool saw_pointer_marker = false;
    bool saw_identifier = false;
    bool saw_core_type_token = false;

    while (i < tokens_.size()) {
      const Token& t = tokens_[i];
      if (t.text == ")") {
        if (!saw_any) {
          return false;
        }
        if (saw_keyword_type || saw_pointer_marker) {
          return true;
        }
        return base.kind != "Identifier" && saw_identifier;
      }

      if (t.text == "," || t.text == "[" || t.text == "]" || t.text == "{" ||
          t.text == "}" || t.text == ";" || t.text == "...") {
        return false;
      }

      if (t.text == "*" || t.text == "&" || t.text == "::") {
        if (!saw_core_type_token) {
          return false;
        }
        saw_any = true;
        saw_pointer_marker = true;
        ++i;
        continue;
      }

      if (t.kind == TokenKind::kKeyword &&
          (IsBuiltinTypeKeyword(t.text) || t.text == "class" || t.text == "union")) {
        saw_any = true;
        saw_keyword_type = true;
        saw_core_type_token = true;
        ++i;
        continue;
      }

      if (t.kind == TokenKind::kIdentifier) {
        saw_any = true;
        saw_identifier = true;
        saw_core_type_token = true;
        ++i;
        continue;
      }

      return false;
    }

    return false;
  }

  std::vector<Token> tokens_;
  std::string filename_;
  std::size_t idx_;
  std::size_t anon_aggregate_counter_;
};

}  // namespace

ParsedNode ParseAst(std::string_view source, std::string_view filename) {
  Lexer lexer(source, filename);
  std::vector<Token> tokens = lexer.Tokenize();
  Parser parser(std::move(tokens), filename);
  return parser.ParseProgram();
}

}  // namespace holyc::frontend::internal

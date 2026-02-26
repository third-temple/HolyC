#pragma once

#include <string>
#include <utility>
#include <vector>

namespace holyc::frontend::internal {

struct ParsedNode {
  ParsedNode() = default;

  ParsedNode(std::string in_kind, std::string in_text, std::vector<ParsedNode> in_children)
      : kind(std::move(in_kind)),
        text(std::move(in_text)),
        children(std::move(in_children)) {}

  std::string kind;
  std::string text;
  std::vector<ParsedNode> children;
  int line = 0;
  int column = 0;
};

struct TypedNode {
  TypedNode() = default;

  TypedNode(std::string in_kind, std::string in_text, std::vector<TypedNode> in_children)
      : kind(std::move(in_kind)),
        text(std::move(in_text)),
        children(std::move(in_children)) {}

  std::string kind;
  std::string text;
  std::vector<TypedNode> children;
  int line = 0;
  int column = 0;
  std::string type;
};

}  // namespace holyc::frontend::internal

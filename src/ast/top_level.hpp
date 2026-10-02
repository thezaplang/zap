#pragma once
#include "../visibility.hpp"
#include "attribute.hpp"
#include "node.hpp"
#include "token/syntax_name.hpp"
#include "visitor.hpp"
#include <vector>

class TopLevel : public virtual Node {
public:
  Visibility visibility_ = Visibility::Private;
  std::vector<AttributeNode> attributes_;
  SyntaxName declarationName_;
  SyntaxName resultBorrowName_;
  SyntaxRange syntaxRange_;

  virtual ~TopLevel() noexcept = default;

  void accept(Visitor &v) override { v.visit(*this); }
};

#pragma once
#include "body_node.hpp"
#include "statement_node.hpp"
#include "visitor.hpp"
#include <memory>
#include <string>
#include <utility>
#include <vector>

enum class CasePatternKind { Literal, Variant };

enum class CasePayloadPatternKind { None, Empty, Binding, Wildcard };

struct CasePattern {
  CasePatternKind kind = CasePatternKind::Literal;
  SourceSpan span;
  std::unique_ptr<ExpressionNode> literal;
  std::vector<std::string> variantPath;
  CasePayloadPatternKind payloadKind = CasePayloadPatternKind::None;
  std::string payloadBinding;
  SourceSpan payloadBindingSpan;
};

struct CaseArm {
  SourceSpan span;
  bool isElse = false;
  std::vector<CasePattern> patterns;
  std::unique_ptr<BodyNode> body;
};

class CaseNode : public StatementNode {
public:
  std::unique_ptr<ExpressionNode> scrutinee;
  std::vector<CaseArm> arms;

  CaseNode(std::unique_ptr<ExpressionNode> value, std::vector<CaseArm> caseArms)
      : scrutinee(std::move(value)), arms(std::move(caseArms)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};

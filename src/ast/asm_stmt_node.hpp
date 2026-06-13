#pragma once
#include "expr_node.hpp"
#include "node.hpp"
#include "statement_node.hpp"
#include "visitor.hpp"
#include <memory>
#include <string>
#include <vector>

struct AsmOperandNode {
  std::string constraint;
  std::unique_ptr<ExpressionNode> expr;
};

class AsmStmtNode : public StatementNode {
public:
  std::string assembly;
  std::vector<AsmOperandNode> outputs;
  std::vector<AsmOperandNode> inputs;
  std::vector<std::string> clobbers;

  void accept(Visitor &v) override { v.visit(*this); }
};

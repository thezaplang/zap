#pragma once
#include "../sema/bound_nodes.hpp"
#include "module.hpp"
#include <map>
#include <memory>
#include <stack>
#include <string>
#include <vector>

namespace zir {

class BoundIRGenerator : public sema::BoundVisitor {
public:
  std::unique_ptr<Module> generate(sema::BoundRootNode &root);

  void visit(sema::BoundRootNode &node) override;
  void visit(sema::BoundFunctionDeclaration &node) override;
  void visit(sema::BoundBlock &node) override;
  void visit(sema::BoundVariableDeclaration &node) override;
  void visit(sema::BoundReturnStatement &node) override;
  void visit(sema::BoundAssignment &node) override;
  void visit(sema::BoundLiteral &node) override;
  void visit(sema::BoundVariableExpression &node) override;
  void visit(sema::BoundBinaryExpression &node) override;
  void visit(sema::BoundUnaryExpression &node) override;
  void visit(sema::BoundFunctionCall &node) override;
  void visit(sema::BoundArrayLiteral &node) override;

private:
  std::unique_ptr<Module> module_;
  Function *currentFunction_ = nullptr;
  BasicBlock *currentBlock_ = nullptr;

  std::map<std::shared_ptr<sema::Symbol>, std::shared_ptr<Value>> symbolMap_;
  std::stack<std::shared_ptr<Value>> valueStack_;

  int nextRegisterId_ = 0;
  int nextBlockId_ = 0;

  std::shared_ptr<Value> createRegister(std::shared_ptr<Type> type);
  std::string createBlockLabel(const std::string &prefix);
};

} // namespace zir

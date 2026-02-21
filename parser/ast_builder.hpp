#pragma once
#include <memory>
#include <string>
#include <vector>

#include "../ast/root_node.hpp"
#include "../ast/fun_decl.hpp"
#include "../ast/body_node.hpp"
#include "../ast/var_decl.hpp"
#include "../ast/return_node.hpp"
#include "../ast/bin_expr.hpp"
#include "../ast/const/const_int.hpp"
#include "../ast/const/const_id.hpp"
#include "../ast/parameter_node.hpp"
#include "../ast/type_node.hpp"
#include "../ast/enum_decl.hpp"
#include "../ast/record_decl.hpp"

class AstBuilder {
public:
  std::unique_ptr<RootNode> makeRoot() { return std::make_unique<RootNode>(); }

  std::unique_ptr<FunDecl> makeFunDecl(const std::string &name)
  {
    auto f = std::make_unique<FunDecl>();
    f->name_ = name;
    return f;
  }

  std::unique_ptr<BodyNode> makeBody()
  {
    return std::make_unique<BodyNode>();
  }

  std::unique_ptr<VarDecl> makeVarDecl(const std::string &name,
                                       std::unique_ptr<TypeNode> type,
                                       std::unique_ptr<ExpressionNode> init)
  {
    return std::make_unique<VarDecl>(name, std::move(type), std::move(init));
  }

  std::unique_ptr<ReturnNode> makeReturn(std::unique_ptr<ExpressionNode> value)
  {
    return std::make_unique<ReturnNode>(std::move(value));
  }

  std::unique_ptr<BinExpr> makeBinExpr(std::unique_ptr<ExpressionNode> left,
                                       const std::string &op,
                                       std::unique_ptr<ExpressionNode> right)
  {
    return std::make_unique<BinExpr>(std::move(left), op, std::move(right));
  }

  std::unique_ptr<ConstInt> makeConstInt(int value)
  {
    return std::make_unique<ConstInt>(value);
  }

  std::unique_ptr<ConstId> makeConstId(const std::string &value)
  {
    return std::make_unique<ConstId>(value);
  }

  std::unique_ptr<ParameterNode> makeParam(const std::string &name,
                                           std::unique_ptr<TypeNode> type)
  {
    return std::make_unique<ParameterNode>(name, std::move(type));
  }

  std::unique_ptr<TypeNode> makeType(const std::string &name)
  {
    return std::make_unique<TypeNode>(name);
  }

  std::unique_ptr<EnumDecl> makeEnumDecl(const std::string &name, std::vector<std::string> entries)
  {
    return std::make_unique<EnumDecl>(name, std::move(entries));
  }

  std::unique_ptr<RecordDecl> makeRecordDecl(const std::string &name, std::vector<std::unique_ptr<ParameterNode>> fields)
  {
    return std::make_unique<RecordDecl>(name, std::move(fields));
  }

  // Utility: set span on a node
  template <typename T>
  T* setSpan(T* node, unsigned int start, unsigned int end)
  {
    if (node) {
      node->span.start = start;
      node->span.end = end;
    }
    return node;
  }
};

#pragma once
#include "expr_node.hpp"
#include "type_node.hpp"
#include "visitor.hpp"
#include <memory>
#include <string>
#include <vector>

struct StructFieldInit {
  std::string name;
  std::unique_ptr<ExpressionNode> value;
  
  StructFieldInit(std::string n, std::unique_ptr<ExpressionNode> v) 
      : name(std::move(n)), value(std::move(v)) {}
};

class StructLiteralNode : public ExpressionNode {
public:
  std::unique_ptr<TypeNode> type_;
  std::vector<StructFieldInit> fields_;

  StructLiteralNode(std::unique_ptr<TypeNode> type,
                    std::vector<StructFieldInit> fields)
      : type_(std::move(type)), fields_(std::move(fields)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};

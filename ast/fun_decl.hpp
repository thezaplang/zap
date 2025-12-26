#pragma once
#include "node.hpp"
#include "top_level.hpp"
#include "type_node.hpp"
#include "body_node.hpp"
#include "parameter_node.hpp"
#include <vector>
#include <memory>
#include <iostream>

class FunDecl : public TopLevel {
public:
    std::string name_;
    std::vector<std::unique_ptr<TypeNode>> genericParams_;
    std::vector<std::unique_ptr<ParameterNode>> params_;
    std::unique_ptr<TypeNode> returnType_;
    std::unique_ptr<BodyNode> body_;
    std::unique_ptr<ExpressionNode> lambdaExpr_;
    bool isExtern_ = false;
    bool isStatic_ = false;
    bool isPublic_ = false;

    FunDecl() = default;

    FunDecl(const std::string& name,
            std::vector<std::unique_ptr<TypeNode>> genericParams,
            std::vector<std::unique_ptr<ParameterNode>> params,
            std::unique_ptr<TypeNode> returnType,
            std::unique_ptr<BodyNode> body,
            std::unique_ptr<ExpressionNode> lambdaExpr,
            bool isExtern = false,
            bool isStatic = false,
            bool isPublic = false)
        : name_(name),
          genericParams_(std::move(genericParams)),
          params_(std::move(params)),
          returnType_(std::move(returnType)),
          body_(std::move(body)),
          lambdaExpr_(std::move(lambdaExpr)),
          isExtern_(isExtern),
          isStatic_(isStatic),
          isPublic_(isPublic) {}
};

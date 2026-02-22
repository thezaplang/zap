#pragma once
#include "parameter_node.hpp"
#include "top_level.hpp"
#include "type_node.hpp"
#include "visitor.hpp"
#include <memory>
#include <vector>

class ExtDecl : public TopLevel
{
public:
    std::string name_;
    std::vector<std::unique_ptr<ParameterNode>> params_;
    std::unique_ptr<TypeNode> returnType_;
    bool isPublic_ = false;

    ExtDecl() = default;

    ExtDecl(const std::string &name,
            std::vector<std::unique_ptr<ParameterNode>> params,
            std::unique_ptr<TypeNode> returnType, bool isPublic = false)
        : name_(name), params_(std::move(params)),
          returnType_(std::move(returnType)), isPublic_(isPublic) {}

    void accept(Visitor &v) override { v.visit(*this); }
};

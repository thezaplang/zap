#pragma once

#include "fun_decl.hpp"
#include "generic_constraint.hpp"
#include "top_level.hpp"
#include "type_node.hpp"
#include "visitor.hpp"
#include <memory>
#include <vector>

class ExtensionDecl : public TopLevel {
public:
    std::vector<std::unique_ptr<TypeNode>> genericParams_;
    std::unique_ptr<TypeNode> targetType_;
    std::vector<GenericConstraint> genericConstraints_;
    std::vector<std::unique_ptr<FunDecl>> methods_;

    void accept(Visitor& v) override { v.visit(*this); }
};

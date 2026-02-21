#pragma once
#include "basic_block.hpp"
#include <string>
#include <vector>
#include <memory>

namespace zir {

class Function {
public:
    std::string name;
    std::string returnType;
    std::vector<std::shared_ptr<Argument>> arguments;
    std::vector<std::unique_ptr<BasicBlock>> blocks;

    Function(std::string name, std::string returnType) : name(std::move(name)), returnType(std::move(returnType)) {}

    void addBlock(std::unique_ptr<BasicBlock> block) {
        blocks.push_back(std::move(block));
    }

    std::string toString() const {
        std::string res = "@" + name + "(";
        for (size_t i = 0; i < arguments.size(); ++i) {
            res += arguments[i]->getTypeName() + " " + arguments[i]->getName();
            if (i < arguments.size() - 1) res += ", ";
        }
        res += ") " + returnType + " {\n";
        for (const auto& block : blocks) {
            res += block->toString();
        }
        res += "}\n";
        return res;
    }
};

} // namespace zir

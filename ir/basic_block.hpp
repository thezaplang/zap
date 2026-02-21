#pragma once
#include "instruction.hpp"
#include <string>
#include <vector>
#include <memory>

namespace zir {

class BasicBlock {
public:
    std::string label;
    std::vector<std::unique_ptr<Instruction>> instructions;

    BasicBlock(std::string label) : label(std::move(label)) {}

    void addInstruction(std::unique_ptr<Instruction> inst) {
        instructions.push_back(std::move(inst));
    }

    std::string toString() const {
        std::string res = label + ":\n";
        for (const auto& inst : instructions) {
            res += "    " + inst->toString() + "\n";
        }
        return res;
    }
};

} // namespace zir

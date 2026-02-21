#pragma once
#include "function.hpp"
#include <vector>
#include <memory>
#include <string>

namespace zir {

class Module {
public:
    std::string name;
    std::vector<std::unique_ptr<Function>> functions;

    Module(std::string name) : name(std::move(name)) {}

    void addFunction(std::unique_ptr<Function> func) {
        functions.push_back(std::move(func));
    }

    std::string toString() const {
        std::string res = "; Module: " + name + "\n";
        for (const auto& func : functions) {
            res += func->toString() + "\n";
        }
        return res;
    }
};

} // namespace zir

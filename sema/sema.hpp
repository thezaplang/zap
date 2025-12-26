#pragma once
#include "ast/parameter_node.hpp"
#include "ast/type_node.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
namespace zap::sema
{
    struct FunctionSymbol
    {
        std::string name;
        std::vector<std::unique_ptr<TypeNode>> genericParams_;
        std::vector<std::unique_ptr<ParameterNode>> params_;
        std::unique_ptr<TypeNode> returnType_;
        bool isExtern_ = false;
        bool isStatic_ = false;
        bool isPublic_ = false;
    };

    class SymbolTable
    {
    private:
        std::unordered_map<std::string, FunctionSymbol> functions_;

    public:
        void addFunction(const FunctionSymbol &func);
        void analyze();
    };
} // namespace sema
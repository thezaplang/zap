#pragma once
#include "../ast/visitor.hpp"
#include "module.hpp"
#include <map>
#include <string>
#include <memory>
#include <stack>
#include <vector>

namespace zir {

struct Scope {
    std::map<std::string, std::shared_ptr<Value>> symbols;
    std::vector<std::shared_ptr<Value>> refVariables;
};

class IRGenerator : public Visitor {
public:
    std::unique_ptr<Module> generate(RootNode& root);

    void visit(RootNode& node) override;
    void visit(FunDecl& node) override;
    void visit(BodyNode& node) override;
    void visit(VarDecl& node) override;
    void visit(ReturnNode& node) override;
    void visit(BinExpr& node) override;
    void visit(ConstInt& node) override;
    void visit(IfNode& node) override;
    void visit(WhileNode& node) override;
    void visit(AssignNode& node) override;
    void visit(FunCall& node) override;
    void visit(ConstId& node) override;
    void visit(ConstFloat& node) override;
    void visit(ConstString& node) override;

private:
    std::unique_ptr<Module> module_;
    Function* currentFunction_ = nullptr;
    BasicBlock* currentBlock_ = nullptr;
    
    std::vector<Scope> scopeStack_;
    std::stack<std::shared_ptr<Value>> valueStack_;
    
    int nextRegisterId_ = 0;
    int nextBlockId_ = 0;

    void pushScope();
    void popScope();
    void addSymbol(const std::string& name, std::shared_ptr<Value> val);
    std::shared_ptr<Value> findSymbol(const std::string& name);

    std::shared_ptr<Type> mapType(const std::string& typeName);
    std::shared_ptr<Value> createRegister(std::shared_ptr<Type> type);
    std::string createBlockLabel(const std::string& prefix);
};

} // namespace zir

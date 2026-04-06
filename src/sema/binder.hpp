#pragma once
#include "../ast/nodes.hpp"
#include "../ast/visitor.hpp"
#include "../utils/diagnostics.hpp"
#include "bound_nodes.hpp"
#include "module_info.hpp"
#include "symbol_table.hpp"
#include <map>
#include <memory>
#include <optional>
#include <stack>

namespace sema
{

  class Binder : public Visitor
  {
  public:
    Binder(zap::DiagnosticEngine &diag, bool allowUnsafe = false);
    std::unique_ptr<BoundRootNode> bind(RootNode &root);
    std::unique_ptr<BoundRootNode> bind(std::vector<ModuleInfo> &modules);

    void visit(RootNode &node) override;
    void visit(ImportNode &node) override;
    void visit(FunDecl &node) override;
    void visit(ExtDecl &node) override;
    void visit(BodyNode &node) override;
    void visit(VarDecl &node) override;
    void visit(ConstDecl &node) override;
    void visit(ReturnNode &node) override;
    void visit(BinExpr &node) override;
    void visit(TernaryExpr &node) override;
    void visit(ConstInt &node) override;
    void visit(ConstBool &node) override;
    void visit(IfNode &node) override;
    void visit(WhileNode &node) override;
    void visit(MemberAccessNode &node) override;
    void visit(IndexAccessNode &node) override;
    void visit(BreakNode &node) override;
    void visit(ContinueNode &node) override;
    void visit(AssignNode &node) override;
    void visit(FunCall &node) override;
    void visit(ConstId &node) override;
    void visit(ConstFloat &node) override;
    void visit(ConstString &node) override;
    void visit(ConstChar &node) override;
    void visit(UnaryExpr &node) override;
    void visit(CastExpr &node) override;
    void visit(ArrayLiteralNode &node) override;
    void visit(ConstNull &node) override;
    void visit(EnumDecl &node) override;
    void visit(TypeAliasDecl &node) override;
    void visit(RecordDecl &node) override;
    void visit(StructDeclarationNode &node) override;
    void visit(StructLiteralNode &node) override;
    void visit(UnsafeBlockNode &node) override;

  private:
    zap::DiagnosticEngine &_diag;
    std::shared_ptr<SymbolTable> currentScope_;
    std::shared_ptr<SymbolTable> builtinScope_;
    std::unique_ptr<BoundRootNode> boundRoot_;

    std::stack<std::unique_ptr<BoundExpression>> expressionStack_;
    std::stack<std::unique_ptr<BoundStatement>> statementStack_;
    std::unique_ptr<BoundBlock> currentBlock_;

    int loopDepth_ = 0;
    int unsafeDepth_ = 0;
    int unsafeTypeContextDepth_ = 0;
    int externTypeContextDepth_ = 0;
    bool allowUnsafe_ = false;

    void pushScope();
    void popScope();

    std::shared_ptr<FunctionSymbol> currentFunction_ = nullptr;
    std::string currentModuleId_;

    struct ModuleState {
      ModuleInfo *info = nullptr;
      std::shared_ptr<SymbolTable> scope;
      std::shared_ptr<ModuleSymbol> symbol;
    };
    std::map<std::string, ModuleState> modules_;

    std::shared_ptr<zir::Type> mapType(const TypeNode &typeNode);
    std::shared_ptr<Symbol> resolveQualifiedSymbol(const std::vector<std::string> &parts,
                                                   SourceSpan span,
                                                   SymbolKind expectedKind = SymbolKind::Variable,
                                                   bool allowAnyKind = false);
    std::shared_ptr<Symbol> resolveModuleMember(const std::string &moduleName,
                                                const std::string &memberName,
                                                SourceSpan span);
    std::optional<int64_t> evaluateConstantInt(const BoundExpression *expr);
    std::unique_ptr<BoundExpression> wrapInCast(std::unique_ptr<BoundExpression> expr, std::shared_ptr<zir::Type> targetType);
    void error(SourceSpan span, const std::string &message);
    std::string mangleName(const std::string &modulePath, const std::string &name) const;
    std::string currentModuleLinkPath() const;
    std::string displayTypeName(const std::string &moduleName, const std::string &name) const;
    std::unique_ptr<BoundBlock> bindBody(BodyNode *body, bool createScope);
    void initializeBuiltins();
    void predeclareModuleTypes(ModuleState &module);
    void predeclareModuleAliases(ModuleState &module);
    void predeclareModuleValues(ModuleState &module);
    void applyImports(ModuleState &module, bool allowIncomplete = false);
    std::shared_ptr<Symbol> lookupVisibleSymbol(const std::string &name) const;

    bool isNumeric(std::shared_ptr<zir::Type> type);
    bool isPointerType(std::shared_ptr<zir::Type> type) const;
    bool isNullType(std::shared_ptr<zir::Type> type) const;
    bool isUnsafeActive() const;
    void requireUnsafeEnabled(SourceSpan span, const std::string &feature);
    void requireUnsafeContext(SourceSpan span, const std::string &feature);
    bool canConvert(std::shared_ptr<zir::Type> from,
                    std::shared_ptr<zir::Type> to);
    std::shared_ptr<zir::Type> getPromotedType(std::shared_ptr<zir::Type> t1,
                                               std::shared_ptr<zir::Type> t2);
    std::shared_ptr<zir::Type> getCVariadicArgumentType(
        std::shared_ptr<zir::Type> type);

    bool hadError_ = false;
  };

} // namespace sema

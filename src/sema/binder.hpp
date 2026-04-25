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
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sema {

class Binder : public Visitor {
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
  void visit(IfTypeNode &node) override;
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
  void visit(ClassDecl &node) override;
  void visit(StructLiteralNode &node) override;
  void visit(UnsafeBlockNode &node) override;
  void visit(NewExpr &node) override;

private:
  zap::DiagnosticEngine &_diag;
  std::shared_ptr<SymbolTable> currentScope_;
  std::shared_ptr<SymbolTable> builtinScope_;
  std::unique_ptr<BoundRootNode> boundRoot_;

  std::stack<std::unique_ptr<BoundExpression>> expressionStack_;
  std::stack<std::unique_ptr<BoundStatement>> statementStack_;
  std::unique_ptr<BoundBlock> currentBlock_;
  std::vector<std::shared_ptr<zir::Type>> expectedExpressionTypes_;

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
    bool valuesPredeclared = false;
    bool finalImportsApplied = false;
    bool valuesPreparationInProgress = false;
  };
  std::map<std::string, ModuleState> modules_;
  std::unordered_map<const Node *, std::shared_ptr<FunctionSymbol>>
      declaredFunctionSymbols_;
  std::unordered_map<const TypeSymbol *, const RecordDecl *>
      recordTypeDeclarationNodes_;
  std::unordered_map<const TypeSymbol *, const StructDeclarationNode *>
      structTypeDeclarationNodes_;
  std::unordered_map<const TypeSymbol *, const ClassDecl *>
      classTypeDeclarationNodes_;
  std::unordered_map<const TypeSymbol *, std::string> typeDeclarationModuleIds_;
  std::unordered_map<const FunctionSymbol *, const FunDecl *>
      functionDeclarationNodes_;
  std::unordered_map<const FunctionSymbol *, std::string>
      functionDeclarationModuleIds_;
  std::unordered_map<const FunctionSymbol *, std::vector<std::string>>
      functionGenericParamNames_;
  std::unordered_map<std::string, std::shared_ptr<FunctionSymbol>>
      genericFunctionInstantiations_;
  std::unordered_map<std::string, std::shared_ptr<TypeSymbol>>
      genericTypeInstantiations_;
  std::unordered_map<const FunctionSymbol *, std::string>
      genericFunctionDeclarationKeys_;
  std::unordered_map<const FunctionSymbol *, bool>
      genericInstantiationEmitted_;
  std::vector<std::unordered_map<std::string, std::shared_ptr<zir::Type>>>
      activeGenericBindingsStack_;
  std::vector<std::string> genericInstantiationInProgress_;
  struct ClassInfo {
    std::shared_ptr<TypeSymbol> typeSymbol;
    std::shared_ptr<zir::ClassType> classType;
    std::shared_ptr<FunctionSymbol> constructor;
    std::shared_ptr<FunctionSymbol> destructor;
    std::map<std::string, std::shared_ptr<VariableSymbol>> fields;
    std::map<std::string, std::shared_ptr<Symbol>> methods;
    int nextVirtualSlot = 0;
    std::string ownerQualifiedName;
  };
  std::unordered_map<std::string, ClassInfo> classInfos_;
  std::vector<std::string> currentClassStack_;

  std::shared_ptr<zir::Type> mapType(const TypeNode &typeNode);
  std::shared_ptr<zir::Type>
  mapTypeWithGenericBindings(
      const TypeNode &typeNode,
      const std::unordered_map<std::string, std::shared_ptr<zir::Type>>
          &genericBindings);
  bool isGenericTypeParameterName(std::string_view name) const;
  std::shared_ptr<Symbol>
  resolveQualifiedSymbol(const std::vector<std::string> &parts, SourceSpan span,
                         SymbolKind expectedKind = SymbolKind::Variable,
                         bool allowAnyKind = false);
  std::shared_ptr<Symbol> resolveModuleMember(const std::string &moduleName,
                                              const std::string &memberName,
                                              SourceSpan span);
  std::optional<int64_t> evaluateConstantInt(const BoundExpression *expr);
  std::unique_ptr<BoundExpression>
  wrapInCast(std::unique_ptr<BoundExpression> expr,
             std::shared_ptr<zir::Type> targetType);
  void error(SourceSpan span, const std::string &message);
  std::string mangleName(const std::string &modulePath,
                         const std::string &name) const;
  std::string mangleFunctionName(const std::string &modulePath,
                                 const FunctionSymbol &function) const;
  std::string currentModuleLinkPath() const;
  std::string displayTypeName(const std::string &moduleName,
                              const std::string &name) const;
  std::string renderTypeForUser(const std::shared_ptr<zir::Type> &type) const;
  std::string functionSignatureKey(const FunctionSymbol &function) const;
  std::string renderFunctionSignature(const FunctionSymbol &function) const;
  std::shared_ptr<FunctionSymbol>
  findFunctionBySignature(const std::shared_ptr<Symbol> &symbol,
                          const FunctionSymbol &prototype) const;
  std::shared_ptr<FunctionSymbol>
  ensureGenericFunctionInstantiation(
      const std::shared_ptr<FunctionSymbol> &baseFunction,
      const std::vector<std::pair<std::string, std::shared_ptr<zir::Type>>>
          &genericBindings,
      SourceSpan callSpan);
  std::unordered_map<std::string, std::shared_ptr<zir::Type>>
  buildGenericBindings(
      const FunctionSymbol &function,
      const std::vector<std::unique_ptr<BoundExpression>> &arguments,
      const std::vector<std::unique_ptr<TypeNode>> &explicitTypeArgs,
      SourceSpan callSpan,
      std::string *failureReason = nullptr);
  std::vector<std::pair<std::string, std::shared_ptr<zir::Type>>>
  orderedGenericBindings(
      const std::unordered_map<std::string, std::shared_ptr<zir::Type>>
          &genericBindings) const;
  std::shared_ptr<zir::Type> substituteGenericType(
      std::shared_ptr<zir::Type> type,
      const std::unordered_map<std::string, std::shared_ptr<zir::Type>>
          &genericBindings) const;
  bool validateGenericConstraints(
      const std::vector<GenericConstraint> &constraints,
      std::unordered_map<std::string, std::shared_ptr<zir::Type>> &bindings,
      std::string *failureReason = nullptr);
  std::shared_ptr<TypeSymbol> instantiateGenericTypeSymbol(
      const std::shared_ptr<TypeSymbol> &baseSymbol, const TypeNode &typeNode);
  std::unique_ptr<BoundExpression>
  bindExpressionWithExpected(ExpressionNode *expr,
                             std::shared_ptr<zir::Type> expectedType);
  std::shared_ptr<zir::Type> currentExpectedExpressionType() const;
  bool bindWeakBuiltinCall(FunCall &node);
  int conversionCost(std::shared_ptr<zir::Type> from,
                     std::shared_ptr<zir::Type> to) const;
  bool isSignedIntegerType(std::shared_ptr<zir::Type> type) const;
  bool isUnsignedIntegerType(std::shared_ptr<zir::Type> type) const;
  int typeBitWidth(std::shared_ptr<zir::Type> type) const;
  std::string describeConversion(std::shared_ptr<zir::Type> from,
                                 std::shared_ptr<zir::Type> to) const;
  std::unique_ptr<BoundBlock> bindBody(BodyNode *body, bool createScope);
  void initializeBuiltins();
  void predeclareModuleTypes(ModuleState &module);
  void predeclareModuleAliases(ModuleState &module);
  void predeclareModuleValues(ModuleState &module);
  void applyImports(ModuleState &module, bool allowIncomplete = false);
  void ensureModuleValuesReady(ModuleState &module);
  std::shared_ptr<Symbol> lookupVisibleSymbol(const std::string &name) const;

  bool isNumeric(std::shared_ptr<zir::Type> type) const;
  bool isPointerType(std::shared_ptr<zir::Type> type) const;
  bool isNullType(std::shared_ptr<zir::Type> type) const;
  bool isUnsafeActive() const;
  void requireUnsafeEnabled(SourceSpan span, const std::string &feature);
  void requireUnsafeContext(SourceSpan span, const std::string &feature);
  bool canConvert(std::shared_ptr<zir::Type> from,
                  std::shared_ptr<zir::Type> to) const;
  std::shared_ptr<zir::Type> getPromotedType(std::shared_ptr<zir::Type> t1,
                                             std::shared_ptr<zir::Type> t2);
  std::shared_ptr<zir::Type>
  getCVariadicArgumentType(std::shared_ptr<zir::Type> type);

  bool hadError_ = false;
};

} // namespace sema

//
// Created by Funcieq on 29.11.2025.
//
#ifndef IGNIS_SEMA_H
#define IGNIS_SEMA_H
#include "../ast/arena.h"
#include "../ast/node.h"
#include <string>
#include <map>

struct FunctionSymbol
{
  std::string name;
  IgnType returnType;
  std::vector<Param> parameters;
};

struct VariableSymbol
{
  std::string name;
  IgnType type;
};

struct StructSymbol
{
  std::string name;
  std::vector<StructField> fields;
};

class SemanticAnalyzer
{
public:
  bool foundMain = false;
  SemanticAnalyzer() = default;
  void Analyze(NodeArena &arena, const std::string &source,
               const std::string &filePath);
  void RegisterFunction(const FunctionSymbol &funcSym);
  bool FuncExsistsCheck(const std::string &funcName);
  void RegisterStruct(const StructSymbol &structSym);
  StructSymbol *FindStruct(const std::string &structName);
  void SetCurrentReturnType(const IgnType &retType);
  IgnType GetCurrentReturnType() const { return currentReturnType; }
  VariableSymbol *FindVariable(const std::string &varName);

private:
  void AnalyzeFunction(Node &funcNode, NodeArena &arena,
                       const std::string &source);
  void AnalyzeFunctionBody(Node &funcNode, NodeArena &arena,
                           const std::string &source);
  void AnalyzeReturnStatement(Node &returnNode, NodeArena &arena,
                              const std::string &source);
  void AnalyzeLetStatement(Node &letNode, NodeArena &arena,
                           const std::string &source);
  void AnalyzeExpression(Node &exprNode, NodeArena &arena,
                         const std::string &source);
  void ReportError(const std::string &msg, const Node &node,
                   const std::string &source);
  void ResolveType(IgnType &type, const std::string &contextName = "");
  std::string GetNamespaceFrom(const std::string &name);

  void PushScope();
  void PopScope();

  std::vector<FunctionSymbol> functionSymbols;
  std::vector<std::vector<VariableSymbol>> variableScopes;
  std::vector<StructSymbol> structSymbols;
  std::map<std::string, std::string> importAliases;
  IgnType currentReturnType;
  std::string currentFilePath;
  std::string currentScopeName;
};
#endif // IGNIS_SEMA_H
//
// Created by Funcieq on 29.11.2025.
//

#include "sema.h"
#include <iostream>
#include <set>

void SemanticAnalyzer::Analyze(NodeArena &arena, const std::string &source,
                               const std::string &filePath)
{

  currentFilePath = filePath;
  std::cout << "Semantic Analysis started.\n";

  std::set<std::string> processedStructs;
  std::set<std::string> processedFuncs;

  for (auto &node : arena.getAllNodes())
  {
    if (node.nodeType == NodeType::TImport)
    {
      if (!node.funcName.empty())
      {
        importAliases[node.funcName] = node.stringValue;
        std::cout << "Registered import alias: " << node.funcName << " = "
                  << node.stringValue << "\n";
      }
      else
      {
        std::string firstPart = node.stringValue;
        size_t colonPos = node.stringValue.find("::");
        if (colonPos != std::string::npos)
        {
          firstPart = node.stringValue.substr(0, colonPos);
        }
        importAliases[firstPart] = node.stringValue;
        std::cout << "Registered import: " << firstPart << " = "
                  << node.stringValue << "\n";
      }
    }
  }

  for (auto &node : arena.getAllNodes())
  {
    if (node.nodeType == NodeType::TModule)
      continue;
    if (node.nodeType == NodeType::TStruct)
    {
      if (processedStructs.count(node.funcName))
        continue;
      processedStructs.insert(node.funcName);
      StructSymbol structSym;
      structSym.name = node.funcName;
      structSym.fields = node.structDef.fields;
      RegisterStruct(structSym);
      std::cout << "Registered struct: " << structSym.name << "\n";
    }
  }

  for (auto &node : arena.getAllNodes())
  {
    if (node.nodeType == NodeType::TStruct)
    {
      for (auto &field : node.structDef.fields)
      {
        ResolveType(field.type, node.funcName);
      }
      StructSymbol *sym = FindStruct(node.funcName);
      if (sym)
      {
        sym->fields = node.structDef.fields;
      }
    }
  }

  for (size_t idx = 0; idx < arena.getAllNodes().size(); ++idx)
  {
    auto &node = arena.getAllNodes()[idx];
    if (node.nodeType == NodeType::TModule)
      continue;
    if (node.nodeType != NodeType::TFun)
      continue;

    if (processedFuncs.count(node.funcName))
      continue;
    processedFuncs.insert(node.funcName);

    ResolveType(node.returnType, node.funcName);
    for (auto &param : node.paramList)
    {
      ResolveType(param.type, node.funcName);
    }

    RegisterFunction({node.funcName, node.returnType, node.paramList});
    std::cout << "Registered function: " << node.funcName << "\n";

    if (!node.isDeclaration)
    {
      AnalyzeFunction(node, arena, source);
    }
  }
}

void SemanticAnalyzer::AnalyzeFunction(Node &funcNode, NodeArena &arena,
                                       const std::string &source)
{
  currentScopeName = funcNode.funcName;
  if (funcNode.funcName == "main")
  {
    foundMain = true;
  }
  else
  {
    std::cout << "Analyzing function: " << funcNode.funcName << "\n";
  }
  SetCurrentReturnType(funcNode.returnType);
  AnalyzeFunctionBody(funcNode, arena, source);
}

void SemanticAnalyzer::AnalyzeFunctionBody(Node &funcNode, NodeArena &arena,
                                           const std::string &source)
{
  PushScope();

  for (auto &param : funcNode.paramList)
  {
    VariableSymbol varSym;
    varSym.name = param.name;
    varSym.type = param.type;
    if (!variableScopes.empty())
    {
      variableScopes.back().push_back(varSym);
    }
  }

  for (auto &childId : funcNode.body)
  {
    Node &childNode = arena.get(childId);
    if (childNode.nodeType == NodeType::TRet)
    {
      AnalyzeReturnStatement(childNode, arena, source);
    }
    else if (childNode.nodeType == NodeType::TLet)
    {
      AnalyzeLetStatement(childNode, arena, source);
    }
    else if (childNode.nodeType == NodeType::TExpr)
    {
      AnalyzeExpression(childNode, arena, source);
    }
    else
    {
      std::cout << "Sema: Function body node type " << childNode.nodeType
                << " not handled yet.\n";
    }
  }

  PopScope();
}

void SemanticAnalyzer::AnalyzeReturnStatement(Node &returnNode,
                                              NodeArena &arena,
                                              const std::string &source)
{
  std::cout << "Analyzing return statement.\n";

  if (returnNode.body.size() > 0)
  {
    Node &returnExpr = arena.get(returnNode.body[0]);
    AnalyzeExpression(returnExpr, arena, source);
  }

  IgnType expectedType = GetCurrentReturnType();
  if (returnNode.exprType.base != expectedType.base)
  {
    ReportError("Return type mismatch", returnNode, source);
  }
}

void SemanticAnalyzer::ReportError(const std::string &msg, const Node &node,
                                   const std::string &source)
{
  size_t pos = std::string::npos;
  if (!node.funcName.empty())
  {
    pos = source.find(node.funcName);
  }
  if (pos == std::string::npos && node.nodeType == NodeType::TRet)
  {

    size_t p = source.find("return");
    if (p != std::string::npos)
    {
      if (node.intValue != 0 || source.find("0", p) != std::string::npos)
      {
        std::string num = std::to_string(node.intValue);
        size_t p2 = source.find(num, p);
        if (p2 != std::string::npos)
          pos = p2;
        else
          pos = p;
      }
      else
      {
        pos = p;
      }
    }
  }
  if (pos == std::string::npos)
    pos = 0;

  unsigned long line = 1;
  unsigned long column = 1;
  unsigned long maxPos =
      static_cast<unsigned long>(std::min(pos, source.size()));
  for (unsigned long i = 0; i < maxPos; ++i)
  {
    if (source[i] == '\n')
    {
      ++line;
      column = 1;
    }
    else
    {
      ++column;
    }
  }

  std::cerr << "Error: " << msg;
  if (!node.funcName.empty())
    std::cerr << " at function '" << node.funcName << "'";
  std::cerr << " in " << currentFilePath << " at line " << line << ", column "
            << column << "\n";
}

void SemanticAnalyzer::RegisterFunction(const FunctionSymbol &funcSym)
{
  functionSymbols.push_back(funcSym);
}
bool SemanticAnalyzer::FuncExsistsCheck(const std::string &funcName)
{
  for (const auto &func : functionSymbols)
  {
    if (func.name == funcName)
    {
      return true;
    }
  }
  return false;
}

void SemanticAnalyzer::SetCurrentReturnType(const IgnType &retType)
{
  currentReturnType = retType;
}

void SemanticAnalyzer::AnalyzeLetStatement(Node &letNode, NodeArena &arena,
                                           const std::string &source)
{
  std::cout << "Analyzing let statement for variable: " << letNode.funcName
            << "\n";

  VariableSymbol varSym;
  varSym.name = letNode.funcName;
  ResolveType(letNode.exprType, letNode.funcName);
  varSym.type = letNode.exprType;

  if (!variableScopes.empty())
  {
    variableScopes.back().push_back(varSym);
  }

  if (letNode.body.size() > 0)
  {
    Node &initExpr = arena.get(letNode.body[0]);
    AnalyzeExpression(initExpr, arena, source);

    if (initExpr.exprType.base != letNode.exprType.base)
    {
      ReportError("Variable initialization type mismatch", letNode, source);
    }
  }
}

void SemanticAnalyzer::AnalyzeExpression(Node &exprNode, NodeArena &arena,
                                         const std::string &source)
{
  if (exprNode.exprKind == ExprType::ExprFieldAccess)
  {
    Node &objectNode = arena.get(exprNode.fieldObject);
    AnalyzeExpression(objectNode, arena, source);

    if (objectNode.exprType.base == PrimType::PTUserType &&
        objectNode.exprType.isStruct)
    {
      StructSymbol *structSym = FindStruct(objectNode.exprType.typeName);
      if (structSym)
      {
        for (const auto &field : structSym->fields)
        {
          if (field.name == exprNode.fieldName)
          {
            exprNode.exprType = field.type;
            std::cout << "Field access '" << exprNode.fieldName
                      << "' of struct '" << objectNode.exprType.typeName
                      << "' has type " << field.type.base << "\n";
            return;
          }
        }
        ReportError("Field '" + exprNode.fieldName + "' not found in struct '" +
                        objectNode.exprType.typeName + "'",
                    exprNode, source);
      }
      else
      {
        ReportError("Struct '" + objectNode.exprType.typeName + "' not defined",
                    exprNode, source);
      }
    }
    else
    {
      ReportError("Field access on non-struct type", exprNode, source);
    }
    return;
  }

  if (exprNode.exprKind == ExprType::ExprStructConstructor)
  {
    IgnType t;
    t.base = PrimType::PTUserType;
    t.typeName = exprNode.funcName;
    ResolveType(t, currentScopeName);

    StructSymbol *structSym = nullptr;
    if (t.isStruct)
    {
      exprNode.funcName = t.typeName;
      structSym = FindStruct(t.typeName);
    }
    else
    {
      // Try direct lookup as fallback
      structSym = FindStruct(exprNode.funcName);
    }

    if (structSym)
    {
      exprNode.exprType.base = PrimType::PTUserType;
      exprNode.exprType.isStruct = true;
      exprNode.exprType.typeName = exprNode.funcName;

      for (const auto &field : structSym->fields)
      {
        if (exprNode.structFields.find(field.name) ==
            exprNode.structFields.end())
        {
          ReportError("Struct field '" + field.name +
                          "' not initialized in constructor",
                      exprNode, source);
        }
      }
      std::cout << "Struct constructor '" << exprNode.funcName
                << "' validated\n";
    }
    else
    {
      ReportError("Struct '" + exprNode.funcName + "' not defined", exprNode,
                  source);
    }
    return;
  }

  if (exprNode.exprKind == ExprType::ExprFuncCall)
  {
    std::string resolvedName = exprNode.funcName;

    size_t firstColonPos = exprNode.funcName.find("::");
    if (firstColonPos != std::string::npos)
    {
      std::string firstPart = exprNode.funcName.substr(0, firstColonPos);
      auto aliasIt = importAliases.find(firstPart);
      if (aliasIt != importAliases.end())
      {
        resolvedName = aliasIt->second + "::" + exprNode.funcName.substr(firstColonPos + 2);
        exprNode.funcName = resolvedName;
        std::cout << "Resolved alias: " << exprNode.funcName << " -> " << resolvedName << "\n";
      }
    }

    for (const auto &func : functionSymbols)
    {
      if (func.name == exprNode.funcName)
      {
        exprNode.exprType = func.returnType;
        std::cout << "Function call '" << exprNode.funcName
                  << "' found with return type " << func.returnType.base
                  << "\n";
        return;
      }
    }
    std::cout << "Sema: Warning - Function '" << exprNode.funcName
              << "' not found in registered symbols\n";
    return;
  }

  if (exprNode.exprKind != ExprType::ExprFuncCall &&
      !exprNode.funcName.empty())
  {
    VariableSymbol *varSym = FindVariable(exprNode.funcName);
    if (varSym)
    {
      exprNode.exprType = varSym->type;
      std::cout << "Variable '" << exprNode.funcName << "' found with type "
                << varSym->type.base << "\n";
    }
    else
    {
      std::cout << "Sema: Warning - Variable or function '" << exprNode.funcName
                << "' not resolved\n";
    }
  }
}

VariableSymbol *SemanticAnalyzer::FindVariable(const std::string &varName)
{
  for (auto it = variableScopes.rbegin(); it != variableScopes.rend(); ++it)
  {
    for (auto &var : *it)
    {
      if (var.name == varName)
      {
        return &var;
      }
    }
  }
  return nullptr;
}

void SemanticAnalyzer::PushScope()
{
  variableScopes.push_back(std::vector<VariableSymbol>());
}

void SemanticAnalyzer::PopScope()
{
  if (!variableScopes.empty())
  {
    variableScopes.pop_back();
  }
}

void SemanticAnalyzer::RegisterStruct(const StructSymbol &structSym)
{
  structSymbols.push_back(structSym);
}

StructSymbol *SemanticAnalyzer::FindStruct(const std::string &structName)
{
  for (auto &structSym : structSymbols)
  {
    if (structSym.name == structName)
    {
      return &structSym;
    }
  }
  return nullptr;
}

std::string SemanticAnalyzer::GetNamespaceFrom(const std::string &name)
{
  size_t lastColon = name.rfind("::");
  if (lastColon == std::string::npos)
  {
    return "";
  }
  return name.substr(0, lastColon);
}

void SemanticAnalyzer::ResolveType(IgnType &type,
                                   const std::string &contextName)
{
  if (type.base == PrimType::PTUserType && !type.typeName.empty())
  {
    if (FindStruct(type.typeName))
    {
      type.isStruct = true;
      return;
    }

    std::string ns = GetNamespaceFrom(contextName);
    if (!ns.empty())
    {
      std::string qualified = ns + "::" + type.typeName;
      if (FindStruct(qualified))
      {
        type.isStruct = true;
        type.typeName = qualified;
        return;
      }
    }
  }
}
//
// Created by Funcieq on 29.11.2025.
//

#include "sema.h"
#include <iostream>

void SemanticAnalyzer::Analyze(NodeArena &arena, const std::string &source, const std::string &filePath)
{

    currentFilePath = filePath;
    std::cout << "Semantic Analysis started.\n";

    // First pass: register all struct definitions
    for (auto &node : arena.getAllNodes())
    {
        if (node.nodeType == NodeType::TStruct)
        {
            StructSymbol structSym;
            structSym.name = node.funcName;
            structSym.fields = node.structDef.fields;
            RegisterStruct(structSym);
            std::cout << "Registered struct: " << structSym.name << "\n";
        }
    }

    // Second pass: register and analyze functions
    for (auto &node : arena.getAllNodes())
    {
        if (node.nodeType != NodeType::TFun)
            continue;

        // Register all function declarations/definitions first
        RegisterFunction({node.funcName, node.returnType, node.paramList});

        // Only analyze body if it's not just a declaration
        if (!node.isDeclaration)
        {
            AnalyzeFunction(node, arena, source);
        }
    }
}

void SemanticAnalyzer::AnalyzeFunction(Node &funcNode, NodeArena &arena, const std::string &source)
{
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

void SemanticAnalyzer::AnalyzeFunctionBody(Node &funcNode, NodeArena &arena, const std::string &source)
{
    PushScope();

    // Register parameters in the scope
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
            AnalyzeReturnStatement(childNode, source);
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
            std::cout << "Sema: Function body node type " << childNode.nodeType << " not handled yet.\n";
        }
    }

    PopScope(); // Exit function scope
}

void SemanticAnalyzer::AnalyzeReturnStatement(Node &returnNode, const std::string &source)
{
    std::cout << "Analyzing return statement.\n";
    IgnType expectedType = GetCurrentReturnType();
    if (returnNode.exprType.base != expectedType.base)
    {
        ReportError("Return type mismatch", returnNode, source);
    }
}

void SemanticAnalyzer::ReportError(const std::string &msg, const Node &node, const std::string &source)
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
    unsigned long maxPos = static_cast<unsigned long>(std::min(pos, source.size()));
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
    std::cerr << " in " << currentFilePath << " at line " << line << ", column " << column << "\n";
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

void SemanticAnalyzer::AnalyzeLetStatement(Node &letNode, NodeArena &arena, const std::string &source)
{
    std::cout << "Analyzing let statement for variable: " << letNode.funcName << "\n";

    // Register the variable in current scope
    VariableSymbol varSym;
    varSym.name = letNode.funcName;
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

void SemanticAnalyzer::AnalyzeExpression(Node &exprNode, NodeArena &arena, const std::string &source)
{

    if (exprNode.exprKind == ExprType::ExprFieldAccess)
    {
        Node &objectNode = arena.get(exprNode.fieldObject);
        AnalyzeExpression(objectNode, arena, source);

        if (objectNode.exprType.base == PrimType::PTUserType && objectNode.exprType.isStruct)
        {
            StructSymbol *structSym = FindStruct(objectNode.exprType.typeName);
            if (structSym)
            {

                for (const auto &field : structSym->fields)
                {
                    if (field.name == exprNode.fieldName)
                    {
                        exprNode.exprType = field.type;
                        std::cout << "Field access '" << exprNode.fieldName << "' of struct '"
                                  << objectNode.exprType.typeName << "' has type " << field.type.base << "\n";
                        return;
                    }
                }
                ReportError("Field '" + exprNode.fieldName + "' not found in struct '" + objectNode.exprType.typeName + "'", exprNode, source);
            }
            else
            {
                ReportError("Struct '" + objectNode.exprType.typeName + "' not defined", exprNode, source);
            }
        }
        else
        {
            ReportError("Field access on non-struct type", exprNode, source);
        }
        return;
    }

    // If this is a struct constructor
    if (exprNode.exprKind == ExprType::ExprStructConstructor)
    {
        StructSymbol *structSym = FindStruct(exprNode.funcName);
        if (structSym)
        {
            exprNode.exprType.base = PrimType::PTUserType;
            exprNode.exprType.isStruct = true;
            exprNode.exprType.typeName = exprNode.funcName;

            // Validate all fields are provided
            for (const auto &field : structSym->fields)
            {
                if (exprNode.structFields.find(field.name) == exprNode.structFields.end())
                {
                    ReportError("Struct field '" + field.name + "' not initialized in constructor", exprNode, source);
                }
            }
            std::cout << "Struct constructor '" << exprNode.funcName << "' validated\n";
        }
        else
        {
            ReportError("Struct '" + exprNode.funcName + "' not defined", exprNode, source);
        }
        return;
    }

    // If this is a function call
    if (exprNode.exprKind == ExprType::ExprFuncCall)
    {

        for (const auto &func : functionSymbols)
        {
            if (func.name == exprNode.funcName)
            {
                exprNode.exprType = func.returnType;
                std::cout << "Function call '" << exprNode.funcName << "' found with return type " << func.returnType.base << "\n";
                return;
            }
        }
        std::cout << "Sema: Warning - Function '" << exprNode.funcName << "' not found in registered symbols\n";
        return;
    }

    if (exprNode.exprKind != ExprType::ExprFuncCall && !exprNode.funcName.empty())
    {

        VariableSymbol *varSym = FindVariable(exprNode.funcName);
        if (varSym)
        {
            exprNode.exprType = varSym->type;
            std::cout << "Variable '" << exprNode.funcName << "' found with type " << varSym->type.base << "\n";
        }
        else
        {

            std::cout << "Sema: Warning - Variable or function '" << exprNode.funcName << "' not resolved\n";
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
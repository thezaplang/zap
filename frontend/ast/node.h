//
// Created by Funcieq on 27.11.2025.
//

#ifndef IGNIS_NODE_H
#define IGNIS_NODE_H
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum NodeType
{
    TFun,
    TLet,
    TRet,
    TIf,
    TWhile,
    TParam,
    TExpr,
    TAssign,
};

using NodeId = uint32_t;

enum PrimType
{
    PTI32,
    PTF32,
    PTBool,
    PTChar,
    PTVoid,
    PTUserType,
    PTString
};

enum ExprType
{
    ExprInt,
    ExprString,
    ExprFloat,
    ExprFuncCall,
    ExprBinary,
    ExprAssign,
    ExprUnary,
};

struct IgnType
{
    bool isPtr;
    bool isStruct;
    bool isArray;
    bool isRef;
    PrimType base;
};

struct Param
{
    IgnType type;
    std::string name;
    bool isVarargs = false; // true for ... parameter
};

struct Variable
{
    std::string name;
    IgnType type;
};

struct Node
{
    NodeType nodeType;
    std::string funcName;
    std::vector<Param> paramList;
    std::vector<Variable> vars; // Local variables in scope
    IgnType returnType;
    IgnType exprType;
    // Children are stored as indices into a NodeArena
    std::vector<NodeId> body;
    int intValue;
    std::string stringValue;
    ExprType exprKind;
    std::vector<NodeId> exprArgs;
    std::string op;
    bool isDeclaration = false; // true for fn foo(); false for fn foo() { ... }
};
#endif // IGNIS_NODE_H
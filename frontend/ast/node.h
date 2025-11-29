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
    TParam,
    TExpr,
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
};

struct Node
{
    NodeType nodeType;
    std::string funcName;
    std::vector<Param> paramList;
    IgnType returnType;
    IgnType exprType;
    // Children are stored as indices into a NodeArena
    std::vector<NodeId> body;
    int intValue;
};
#endif // IGNIS_NODE_H
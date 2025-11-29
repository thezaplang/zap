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
};

using NodeId = uint32_t;

struct IgnType
{
    bool isPtr;
    bool isStruct;
    bool isArray;
    bool isRef;
    std::string base;
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
    // Children are stored as indices into a NodeArena
    std::vector<NodeId> body;
    int intValue;
};
#endif // IGNIS_NODE_H
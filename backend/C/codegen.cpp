//
// Created by Funcieq on 29.11.2025.
//

#include "codegen.h"
#include <fstream>
#include <iostream>

namespace ignis
{
    namespace backend
    {
        namespace c
        {
            void CodeGen::generate(const NodeArena &arena, const std::string &outputPath)
            {
                code.str("");
                code.clear();

                // Emit standard C includes
                emitIncludes();
                code << "\n";

                // Emit only top-level function nodes
                for (size_t i = 0; i < arena.size(); ++i)
                {
                    const Node &node = arena.get(i);
                    if (node.nodeType == TFun)
                    {
                        emitFunction(node, arena);
                    }
                }

                std::ofstream outFile(outputPath);
                if (!outFile.is_open())
                {
                    std::cerr << "Error: Could not open output file: " << outputPath << std::endl;
                    return;
                }

                outFile << code.str();
                outFile.close();

                std::cout << "Generated C code to: " << outputPath << std::endl;
            }

            void CodeGen::emitIncludes()
            {
                code << "#include \"ignis_std.h\"\n";
            }

            void CodeGen::emitNode(const Node &node, const NodeArena &arena)
            {
                switch (node.nodeType)
                {
                case TFun:
                    emitFunction(node, arena);
                    break;
                case TLet:

                    break;
                case TRet:
                    emitReturnStatement(node, arena);
                    break;
                case TExpr:
                    emitExpression(node, arena);
                    break;
                default:
                    break;
                }
            }

            void CodeGen::emitFunction(const Node &funcNode, const NodeArena &arena)
            {
                // Function signature
                code << typeToC(funcNode.returnType) << " " << funcNode.funcName << "(";

                // Parameters
                for (size_t i = 0; i < funcNode.paramList.size(); ++i)
                {
                    if (i > 0)
                        code << ", ";
                    code << typeToC(funcNode.paramList[i].type) << " " << funcNode.paramList[i].name;
                }

                code << ")\n{\n";

                // Function body
                emitFunctionBody(funcNode, arena);

                code << "}\n\n";
            }

            void CodeGen::emitFunctionBody(const Node &funcNode, const NodeArena &arena)
            {
                for (NodeId childId : funcNode.body)
                {
                    if (childId < arena.size())
                    {
                        const Node &child = arena.get(childId);
                        if (child.nodeType == TRet)
                        {
                            emitReturnStatement(child, arena);
                        }
                        else if (child.nodeType == TLet)
                        {
                            // Handle variable declarations
                        }
                        else if (child.nodeType == TExpr)
                        {
                            emitExpression(child, arena);
                            code << ";\n";
                        }
                    }
                }
            }

            void CodeGen::emitReturnStatement(const Node &retNode, const NodeArena &arena)
            {
                code << "    return ";

                // Emit the return expression
                if (retNode.body.size() > 0 && retNode.body[0] < arena.size())
                {
                    emitExpression(arena.get(retNode.body[0]), arena);
                }
                else
                {
                    emitValue(retNode, arena);
                }

                code << ";\n";
            }

            void CodeGen::emitValue(const Node &valueNode, const NodeArena &arena)
            {
                switch (valueNode.exprType.base)
                {
                case PTString:
                    code << "\"" << valueNode.stringValue << "\"";
                    break;
                case PTI32:
                case PTF32:
                    code << valueNode.intValue;
                    break;
                case PTBool:
                    code << (valueNode.intValue ? "true" : "false");
                    break;
                case PTVoid:
                    // Don't emit anything for void
                    break;
                default:
                    code << valueNode.intValue;
                    break;
                }
            }

            void CodeGen::emitExpression(const Node &exprNode, const NodeArena &arena)
            {

                if (exprNode.exprKind == ExprFuncCall)
                {
                    code << exprNode.funcName << "(";
                    for (size_t i = 0; i < exprNode.exprArgs.size(); ++i)
                    {
                        if (i > 0)
                            code << ", ";
                        NodeId argId = exprNode.exprArgs[i];
                        if (argId < arena.size())
                        {
                            emitExpression(arena.get(argId), arena);
                        }
                    }
                    code << ")";
                }
                else
                {
                    // For literal values, use emitValue
                    emitValue(exprNode, arena);
                }
            }

            std::string CodeGen::typeToC(const IgnType &type)
            {
                std::string result = primTypeToC(type.base);

                if (type.isPtr)
                    result += "*";
                if (type.isArray)
                    result += "[]";
                if (type.isRef)
                    result += "&";

                return result;
            }

            std::string CodeGen::primTypeToC(PrimType type)
            {
                switch (type)
                {
                case PTI32:
                    return "int32_t";
                case PTF32:
                    return "float";
                case PTBool:
                    return "bool";
                case PTChar:
                    return "char";
                case PTVoid:
                    return "void";
                case PTString:
                    return "char*";
                case PTUserType:
                    return "struct";
                default:
                    return "void";
                }
            }

        }
    }
}

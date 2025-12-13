//
// Created by Funcieq on 29.11.2025.
//

#ifndef IGNIS_CODEGEN_H
#define IGNIS_CODEGEN_H

#include "../../frontend/ast/arena.h"
#include "../../frontend/ast/node.h"
#include <string>
#include <sstream>

namespace ignis
{
    namespace backend
    {
        namespace c
        {
            class CodeGen
            {
            public:
                CodeGen() = default;
                ~CodeGen() = default;

                void generate(const NodeArena &arena, const std::string &outputPath);

                std::string getCode() const { return code.str(); }

            private:
                std::stringstream code;
                std::string mangle(const std::string &name) const;

                // Helper methods
                void emitIncludes();
                void emitNode(const Node &node, const NodeArena &arena, const std::string &indent = "");
                void emitFunction(const Node &funcNode, const NodeArena &arena);
                void emitFunctionBody(const Node &funcNode, const NodeArena &arena);
                void emitExpression(const Node &exprNode, const NodeArena &arena);
                void emitValue(const Node &valueNode, const NodeArena &arena);
                std::string typeToC(const IgnType &type);
                std::string primTypeToC(PrimType type);
            };

        }
    }
}

#endif
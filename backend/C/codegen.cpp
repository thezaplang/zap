//
// Created by Funcieq on 29.11.2025.
//

#include "codegen.h"
#include <fstream>
#include <iostream>
#include <set>

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

        emitIncludes();

        code << "\n";

        std::set<std::string> emittedStructs;
        std::set<std::string> emittedFuncs;
        for (size_t i = 0; i < arena.size(); ++i)
        {
          const Node &node = arena.get(i);
          if (node.nodeType == TModule)
            continue;
          if (node.nodeType == TStruct)
          {
            if (emittedStructs.count(node.funcName))
              continue;
            emittedStructs.insert(node.funcName);
            emitNode(node, arena, "");
          }
        }

        for (size_t i = 0; i < arena.size(); ++i)
        {
          const Node &node = arena.get(i);
          if (node.nodeType == TModule)
            continue;
          if (node.nodeType == TFun && node.isExtern)
          {
            if (emittedFuncs.count(node.funcName))
              continue;
            emittedFuncs.insert(node.funcName);
            emitFunction(node, arena);
          }
        }

        for (size_t i = 0; i < arena.size(); ++i)
        {
          const Node &node = arena.get(i);
          if (node.nodeType == TModule)
            continue;
          if (node.nodeType == TFun && !node.isExtern && node.funcName != "main")
          {
            if (emittedFuncs.count(node.funcName))
              continue;
            emittedFuncs.insert(node.funcName);
            emitFunction(node, arena);
          }
        }

        for (size_t i = 0; i < arena.size(); ++i)
        {
          const Node &node = arena.get(i);
          if (node.nodeType == TModule)
            continue;
          if (node.nodeType == TFun && node.funcName == "main")
          {
            if (emittedFuncs.count(node.funcName))
              continue;
            emittedFuncs.insert(node.funcName);
            emitFunction(node, arena);
          }
        }

        std::ofstream outFile(outputPath);
        if (!outFile.is_open())
        {
          std::cerr << "Error: Could not open output file: " << outputPath
                    << std::endl;
          return;
        }

        outFile << code.str();
        outFile.close();

        std::cout << "Generated C code to: " << outputPath << std::endl;
      }

      void CodeGen::emitIncludes() { code << "#include \"ignis_std.h\"\n"; }

      void CodeGen::emitNode(const Node &node, const NodeArena &arena,
                             const std::string &indent)
      {
        switch (node.nodeType)
        {
        case TStruct:
        {
          {
            std::string mangled = mangle(node.funcName);
            code << "typedef struct " << mangled << " {\n";
            for (const auto &field : node.structDef.fields)
            {
              code << "    " << typeToC(field.type) << " " << field.name << ";\n";
            }
            code << "} " << mangled << ";\n\n";
          }
          break;
        }
        case TFun:
          emitFunction(node, arena);
          break;
        case TLet:
        {
          code << indent << typeToC(node.exprType) << " " << node.funcName;
          if (node.body.size() > 0 && node.body[0] < arena.size())
          {
            code << " = ";
            emitExpression(arena.get(node.body[0]), arena);
          }
          code << ";\n";
          break;
        }
        case TAssign:
        {
          code << indent;

          // Check if it's a field assignment (p.x = value)
          if (!node.fieldName.empty())
          {
            code << node.funcName << "." << node.fieldName;
          }
          else if (!node.funcName.empty())
          {
            code << node.funcName;
          }
          else if (node.body.size() > 0 && node.body[0] < arena.size())
          {
            emitExpression(arena.get(node.body[0]), arena);
          }
          code << " = ";
          if (node.body.size() > 0 && node.body[0] < arena.size())
          {
            emitExpression(arena.get(node.body[0]), arena);
          }
          code << ";\n";
          break;
        }
        case TRet:
        {
          code << indent << "return ";
          if (node.body.size() > 0 && node.body[0] < arena.size())
          {
            emitExpression(arena.get(node.body[0]), arena);
          }
          else
          {
            emitValue(node, arena);
          }
          code << ";\n";
          break;
        }
        case TExpr:
        {
          code << indent;
          emitExpression(node, arena);
          code << ";\n";
          break;
        }
        case TIf:
        {
          code << indent << "if (";
          if (node.body.size() > 0 && node.body[0] < arena.size())
          {
            emitExpression(arena.get(node.body[0]), arena);
          }
          code << ")\n"
               << indent << "{\n";

          std::string innerIndent = indent + "    ";
          for (size_t i = 1; i < node.body.size(); ++i)
          {
            if (node.body[i] < arena.size())
            {
              emitNode(arena.get(node.body[i]), arena, innerIndent);
            }
          }

          code << indent << "}\n";
          break;
        }
        case TWhile:
        {
          code << indent << "while (";
          if (node.body.size() > 0 && node.body[0] < arena.size())
          {
            emitExpression(arena.get(node.body[0]), arena);
          }
          code << ")\n"
               << indent << "{\n";

          std::string innerIndent = indent + "    ";
          for (size_t i = 1; i < node.body.size(); ++i)
          {
            if (node.body[i] < arena.size())
            {
              emitNode(arena.get(node.body[i]), arena, innerIndent);
            }
          }

          code << indent << "}\n";
          break;
        }
        default:
          break;
        }
      }

      void CodeGen::emitFunction(const Node &funcNode, const NodeArena &arena)
      {
        std::string funcName;
        if (funcNode.isExtern)
        {
          funcName = funcNode.funcName;
        }
        else
        {
          funcName =
              (funcNode.funcName == "main") ? "main" : mangle(funcNode.funcName);
        }
        code << typeToC(funcNode.returnType) << " " << funcName << "(";

        if (funcNode.paramList.size() == 0)
        {
          code << "void";
        }
        else
        {
          for (size_t i = 0; i < funcNode.paramList.size(); ++i)
          {
            if (i > 0)
              code << ", ";

            if (funcNode.paramList[i].isVarargs)
            {
              code << "...";
            }
            else
            {
              code << typeToC(funcNode.paramList[i].type) << " "
                   << funcNode.paramList[i].name;
            }
          }
        }

        if (funcNode.isDeclaration)
        {
          code << ");\n\n";
          return;
        }

        code << ")\n{\n";

        emitFunctionBody(funcNode, arena);

        code << "}\n\n";
      }

      void CodeGen::emitFunctionBody(const Node &funcNode, const NodeArena &arena)
      {
        for (NodeId childId : funcNode.body)
        {
          if (childId < arena.size())
          {
            emitNode(arena.get(childId), arena, "    ");
          }
        }
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
          break;
        default:
          code << valueNode.intValue;
          break;
        }
      }

      void CodeGen::emitExpression(const Node &exprNode, const NodeArena &arena)
      {
        if (exprNode.exprKind == ExprUnary)
        {
          if (exprNode.body.size() >= 1)
          {
            code << exprNode.op;
            emitExpression(arena.get(exprNode.body[0]), arena);
          }
        }
        else if (exprNode.exprKind == ExprBinary)
        {
          if (exprNode.body.size() >= 2)
          {
            code << "(";
            emitExpression(arena.get(exprNode.body[0]), arena);
            code << " " << exprNode.op << " ";
            emitExpression(arena.get(exprNode.body[1]), arena);
            code << ")";
          }
        }
        else if (exprNode.exprKind == ExprFuncCall)
        {
          // Don't mangle function names without :: (they are either extern C functions or local functions)
          // Only mangle qualified names (with ::)
          std::string funcNameToUse = exprNode.funcName;
          if (exprNode.funcName.find("::") != std::string::npos)
          {
            funcNameToUse = mangle(exprNode.funcName);
          }
          code << funcNameToUse << "(";
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
        else if (exprNode.exprKind == ExprStructConstructor)
        {
          code << "(struct " << mangle(exprNode.funcName) << ") {";
          bool first = true;
          for (const auto &field : exprNode.structFields)
          {
            if (!first)
              code << ", ";
            code << "." << field.first << " = ";
            if (field.second < arena.size())
            {
              emitExpression(arena.get(field.second), arena);
            }
            first = false;
          }
          code << "}";
        }
        else if (exprNode.exprKind == ExprFieldAccess)
        {
          // Field access: obj.field
          if (exprNode.fieldObject < arena.size())
          {
            emitExpression(arena.get(exprNode.fieldObject), arena);
          }
          code << "." << exprNode.fieldName;
        }
        else if (!exprNode.funcName.empty() && exprNode.exprKind != ExprFuncCall)
        {
          // Don't mangle variable names - they're local/parameters
          code << exprNode.funcName;
        }
        else
        {
          emitValue(exprNode, arena);
        }
      }

      std::string CodeGen::typeToC(const IgnType &type)
      {
        std::string result;

        if (type.base == PTUserType && type.isStruct)
        {
          result = "struct " + mangle(type.typeName);
        }
        else
        {
          result = primTypeToC(type.base);
        }

        if (type.isPtr)
          result += "*";
        if (type.isArray)
          result += "[]";

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
          return "const char*";
        case PTUserType:
          return "struct";
        default:
          return "void";
        }
      }

      std::string CodeGen::mangle(const std::string &name) const
      {
        if (name.empty())
          return name;
        std::string out = "__IGN_";
        for (size_t i = 0; i < name.size(); ++i)
        {
          if (i + 1 < name.size() && name[i] == ':' && name[i + 1] == ':')
          {
            out += '_';
            ++i; // skip next ':'
            continue;
          }
          char c = name[i];
          if (c == ':')
          {
            // should not happen, but replace with '_'
            out += '_';
          }
          else if (c == '_')
          {
            out += "__"; // avoid collisions with our separator
          }
          else
          {
            out += c;
          }
        }
        return out;
      }

    } // namespace c
  } // namespace backend
} // namespace ignis

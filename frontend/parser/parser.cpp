//
// Created by Funcieq on 28.11.2025.
//

#include "parser.h"
#include "../../utils/utils.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

NodeArena Parser::Parse(std::vector<Token> *tokensInput, std::string_view src,
                        const std::string &filePath)
{

  tokens = *tokensInput;
  pos = 0;
  arena = NodeArena();
  source = std::string(src);
  errors.clear();

  while (!IsAtEnd())
  {
    ParseStatement();
  }

  ReportErrors(filePath);

  return arena;
}

Node Parser::ParseStatement()
{
  if (Peek().type == TokenType::KFn)
  {
    Node func = ParseFunction(false);
    return func;
  }
  else if (Peek().type == TokenType::KStruct)
  {
    Node structNode = ParseStruct();
    definedStructs.insert(structNode.funcName);
    return structNode;
  }
  else if (Peek().type == TokenType::KModule)
  {
    Node moduleNode = ParseModule();
    return moduleNode;
  }
  else if (Peek().type == TokenType::KExtern)
  {
    Advance(); // consume 'extern'
    if (Peek().type == TokenType::KFn)
    {
      return ParseFunction(true);
    }
    else
    {
      AddError("Expected 'fn' after 'extern'", Peek());
      return Node();
    }
  }
  else if (Peek().type == TokenType::KImport)
  {
    Node importNode = ParseImport();
    return importNode;
  }
  else
  {
    AddError(std::string("statement of type: ") + std::string(Peek().value) +
                 " is not implemented yet",
             Peek());
    Advance();
    return Node();
  }
}

Node Parser::ParseFunction(bool isExtern)
{
  Advance();
  Token nameTok = Consume(TokenType::Id, "Expected function name");
  std::string_view funcName = nameTok.value;

  Token lparenTok = Consume(TokenType::LParen, "Expected '('");
  std::vector<Param> params;
  if (!lastConsumeSynthetic)
  {
    params = ParseParams();
    Token rparenTok = Consume(TokenType::RParen, "Expected ')'");
    if (lastConsumeSynthetic)
    {

      Synchronize(TokenType::LBrace);
    }
  }
  else
  {
    // missing '(', synchronize to the function body start
    Synchronize(TokenType::LBrace);
  }
  Consume(TokenType::Arrow, "Expected '->' after function parameters");

  IgnType rt = ParseType();

  std::string qualifiedName;
  if (!isExtern && !currentNamespace.empty())
  {
    qualifiedName = "";
    for (size_t i = 0; i < currentNamespace.size(); ++i)
    {
      if (i)
        qualifiedName += "::";
      qualifiedName += currentNamespace[i];
    }
    qualifiedName += "::";
    qualifiedName += std::string(funcName);
  }
  else
  {
    qualifiedName = std::string(funcName);
  }

  Node funcNode;
  funcNode.nodeType = NodeType::TFun;
  funcNode.funcName = qualifiedName;
  funcNode.paramList = params;
  funcNode.returnType = rt;

  if (Peek().type == TokenType::Semi || isExtern)
  {
    // Declaration only
    funcNode.isDeclaration = true;
    funcNode.isExtern = isExtern; // Set extern flag
    Consume(TokenType::Semi, "Expected ';' after function declaration");
  }
  else
  {
    Consume(TokenType::LBrace, "Expected '{'");
    NodeId funcId = arena.create(funcNode);
    ParseBody(funcId);
    Consume(TokenType::RBrace, "Expected '}'");
    return arena.get(funcId);
  }

  NodeId funcId = arena.create(funcNode);
  return arena.get(funcId);
}

Node Parser::ParseStruct()
{
  Consume(TokenType::KStruct, "Expected 'struct'");

  Token structNameTok = Consume(TokenType::Id, "Expected struct name");
  std::string structName = std::string(structNameTok.value);

  std::string qualifiedStructName;
  if (!currentNamespace.empty())
  {
    qualifiedStructName = "";
    for (size_t i = 0; i < currentNamespace.size(); ++i)
    {
      if (i)
        qualifiedStructName += "::";
      qualifiedStructName += currentNamespace[i];
    }
    qualifiedStructName += "::";
    qualifiedStructName += structName;
  }
  else
  {
    qualifiedStructName = structName;
  }

  Consume(TokenType::LBrace, "Expected '{' after struct name");

  Node structNode;
  structNode.nodeType = NodeType::TStruct;
  structNode.funcName = qualifiedStructName;

  std::vector<StructField> fields;

  while (Peek().type != TokenType::RBrace && !IsAtEnd())
  {
    Token fieldNameTok = Consume(TokenType::Id, "Expected field name");
    std::string fieldName = std::string(fieldNameTok.value);

    Consume(TokenType::Colon, "Expected ':' after field name");

    IgnType fieldType = ParseType();

    StructField field;
    field.name = fieldName;
    field.type = fieldType;
    fields.push_back(field);

    if (Peek().type == TokenType::Comma)
    {
      Advance();
    }
    else if (Peek().type != TokenType::RBrace)
    {
      AddError("Expected ',' or '}' after field definition", Peek());
      break;
    }
  }

  Consume(TokenType::RBrace, "Expected '}' after struct fields");

  structNode.structDef.name = qualifiedStructName;
  structNode.structDef.fields = fields;

  NodeId structId = arena.create(structNode);
  return arena.get(structId);
}

Node Parser::ParseModule()
{
  Consume(TokenType::KModule, "Expected 'module'");

  std::vector<std::string> namespaces;

  Token namespaceTok = Consume(TokenType::Id, "Expected namespace name");
  namespaces.push_back(std::string(namespaceTok.value));

  while (Peek().type == TokenType::DoubleColon)
  {
    Advance(); // consume '::'
    Token nextNamespaceTok =
        Consume(TokenType::Id, "Expected namespace name after '::'");
    namespaces.push_back(std::string(nextNamespaceTok.value));
  }

  Consume(TokenType::LBrace, "Expected '{' after module name");

  Node moduleNode;
  moduleNode.nodeType = NodeType::TModule;
  moduleNode.moduleDef.namespaces = namespaces;

  for (const auto &ns : namespaces)
  {
    currentNamespace.push_back(ns);
  }

  NodeId moduleId = arena.create(moduleNode);

  while (!IsAtEnd() && Peek().type != TokenType::RBrace)
  {
    if (Peek().type == TokenType::KFn)
    {
      Node func = ParseFunction();
      NodeId funcId = arena.create(func);
      arena.get(moduleId).body.push_back(funcId);
    }
    else if (Peek().type == TokenType::KStruct)
    {
      Node structNode = ParseStruct();
      definedStructs.insert(structNode.funcName);
      NodeId structId = arena.create(structNode);
      arena.get(moduleId).body.push_back(structId);
    }
    else if (Peek().type == TokenType::KModule)
    {
      Node nestedModule = ParseModule();
      NodeId nestedModuleId = arena.create(nestedModule);
      arena.get(moduleId).body.push_back(nestedModuleId);
    }
    else if (Peek().type == TokenType::KExtern)
    {
      Advance(); // consume 'extern'
      if (Peek().type == TokenType::KFn)
      {
        Node func = ParseFunction(true);
        NodeId funcId = arena.create(func);
        arena.get(moduleId).body.push_back(funcId);
      }
      else
      {
        AddError("Expected 'fn' after 'extern'", Peek());
      }
    }
    else
    {
      AddError(std::string("Statement type not yet supported in module: ") +
                   std::string(Peek().value),
               Peek());
      Synchronize(TokenType::RBrace);
    }
  }

  for (size_t i = 0; i < namespaces.size(); i++)
  {
    currentNamespace.pop_back();
  }

  Consume(TokenType::RBrace, "Expected '}' after module body");

  return arena.get(moduleId);
}

Node Parser::ParseImport()
{
  Consume(TokenType::KImport, "Expected 'import'");
  Token nameTok = Consume(TokenType::Id, "Expected module name");

  std::string alias;
  std::string fullName = std::string(nameTok.value);

  // Check if this is an aliased import: alias = full::path
  if (Peek().type == TokenType::Assign)
  {
    Advance(); // consume '='
    alias = fullName;

    // Parse the full module path
    Token moduleNameTok = Consume(TokenType::Id, "Expected module name after '='");
    fullName = std::string(moduleNameTok.value);

    while (Peek().type == TokenType::DoubleColon)
    {
      Advance(); // consume '::'
      Token nextNameTok = Consume(TokenType::Id, "Expected module name after '::'");
      fullName += "::";
      fullName += std::string(nextNameTok.value);
    }
  }
  else
  {
    // Build possibly qualified import name: std::io
    while (Peek().type == TokenType::DoubleColon)
    {
      Advance(); // consume '::'
      Token nextNameTok = Consume(TokenType::Id, "Expected module name after '::'");
      fullName += "::";
      fullName += std::string(nextNameTok.value);
    }
  }

  Node importNode;
  importNode.nodeType = NodeType::TImport;
  importNode.stringValue = fullName;
  importNode.funcName = alias; // Use funcName to store the alias (if any)

  Consume(TokenType::Semi, "Expected ';' after import statement");

  NodeId importId = arena.create(importNode);
  return arena.get(importId);
}

IgnType Parser::ParseType()
{
  IgnType type;
  type.isPtr = false;
  type.isStruct = false;
  type.isArray = false;
  type.isRef = false;

  if (Peek().type == TokenType::Ampersand)
  {
    Advance();
    type.isRef = true;
  }

  while (Peek().type == TokenType::Star)
  {
    Advance();
    type.isPtr = true;
  }

  // Parse base type, potentially with namespace: Name1::Name2::Type
  Token typeNameTok = Consume(TokenType::Id, "Expected type name");
  std::string typeName = std::string(typeNameTok.value);

  // Handle namespace-qualified types
  while (Peek().type == TokenType::DoubleColon)
  {
    Advance(); // consume '::'
    Token nextTok =
        Consume(TokenType::Id, "Expected type component after '::'");
    typeName += "::";
    typeName += std::string(nextTok.value);
  }

  type.base = stringToPrimType(typeName);
  type.typeName = typeName;

  // Check if it's a struct type
  if (definedStructs.find(typeName) != definedStructs.end())
  {
    type.isStruct = true;
    type.base = PTUserType;
  }

  return type;
}

std::vector<Param> Parser::ParseParams()
{
  std::vector<Param> params;
  while (Peek().type != TokenType::RParen)
  {

    if (Peek().type == TokenType::LBrace ||
        Peek().type == TokenType::EOF_TOKEN)
    {
      break;
    }

    if (Peek().type == TokenType::Ellipsis)
    {
      Advance(); // consume '...'
      Param varargParam;
      varargParam.isVarargs = true;
      varargParam.name = "...";
      IgnType varargType;
      varargType.isPtr = false;
      varargType.isStruct = false;
      varargType.isArray = false;
      varargType.isRef = false;
      varargType.base = PrimType::PTVoid;
      varargParam.type = varargType;
      params.push_back(varargParam);
      break; // varargs must be last
    }

    std::string paramName =
        std::string(Consume(TokenType::Id, "Expected parameter name").value);
    Consume(TokenType::Colon, "Expected ':' after parameter name");

    IgnType paramType = ParseType();

    Param param;
    param.name = paramName;
    param.type = paramType;
    param.isVarargs = false;

    params.push_back(param);

    if (Peek().type == TokenType::Comma)
    {
      Advance();
    }
    else
    {
      break;
    }
  }
  return params;
}

void Parser::ParseBody(NodeId funcId)
{
  while (!IsAtEnd() && Peek().type != TokenType::RBrace)
  {
    if (Peek().type == TokenType::KReturn)
    {
      Node returnNode = ParseReturn();
      NodeId childId = arena.create(returnNode);
      arena.get(funcId).body.push_back(childId);
    }
    else if (Peek().type == TokenType::Id)
    {
      // Check lookahead tokens to determine what kind of statement this is
      TokenType next = Peek(1).type;

      // Determine if this might be a qualified name (with ::)
      size_t lookahead = 1;
      while (lookahead < 10 && Peek(lookahead).type == TokenType::DoubleColon)
      {
        lookahead++; // skip ::
        if (lookahead < 10 && Peek(lookahead).type == TokenType::Id)
          lookahead++; // skip next identifier
        next = (lookahead < 10) ? Peek(lookahead).type : TokenType::EOF_TOKEN;
      }

      if (next == TokenType::Assign)
      {
        Node assignNode = ParseAssignment();
        NodeId assignId = arena.create(assignNode);
        arena.get(funcId).body.push_back(assignId);
        if (Peek().type == TokenType::Semi)
          Advance();
      }
      else if (next == TokenType::Dot)
      {
        Node assignNode = ParseAssignment();
        NodeId assignId = arena.create(assignNode);
        arena.get(funcId).body.push_back(assignId);
        if (Peek().type == TokenType::Semi)
          Advance();
      }
      else if (next == TokenType::LParen)
      {
        // This is a function call (possibly qualified), parse as expression
        Node exprNode = ParseExpr();
        NodeId exprId = arena.create(exprNode);
        arena.get(funcId).body.push_back(exprId);
        if (Peek().type == TokenType::Semi)
          Advance();
      }
      else
      {
        AddError("Only function call expressions and assignments are supported "
                 "as statements starting with identifier",
                 Peek());
        Advance();
      }
    }
    else if (Peek().type == TokenType::Star)
    {
      Node lhs = ParseExpr();
      if (Peek().type == TokenType::Assign)
      {
        Advance(); // consume =
        Node rhs = ParseExpr();

        Node assignNode;
        assignNode.nodeType = NodeType::TAssign;
        assignNode.exprKind = ExprType::ExprAssign;
        assignNode.body.push_back(arena.create(lhs));
        assignNode.body.push_back(arena.create(rhs));

        NodeId assignId = arena.create(assignNode);
        arena.get(funcId).body.push_back(assignId);
      }
      else
      {
        NodeId exprId = arena.create(lhs);
        arena.get(funcId).body.push_back(exprId);
      }
      if (Peek().type == TokenType::Semi)
        Advance();
    }
    else if (Peek().type == TokenType::KLet)
    {
      Node letNode = ParseLet();
      NodeId letId = arena.create(letNode);
      arena.get(funcId).body.push_back(letId);

      if (!letNode.funcName.empty())
      {
        Variable var;
        var.name = letNode.funcName;
        var.type = letNode.exprType;
        arena.get(funcId).vars.push_back(var);
      }
    }
    else if (Peek().type == TokenType::KIF)
    {
      Node ifNode = ParseIf();
      NodeId ifId = arena.create(ifNode);
      arena.get(funcId).body.push_back(ifId);
    }
    else if (Peek().type == TokenType::KWhile)
    {
      Node whileNode = ParseWhile();
      NodeId whileId = arena.create(whileNode);
      arena.get(funcId).body.push_back(whileId);
    }
    else
    {
      AddError(std::string("Statement type not yet supported in body: ") +
                   std::string(Peek().value),
               Peek());
      Synchronize(TokenType::RBrace);
      if (Peek().type == TokenType::Semi)
        Advance();
    }
  }
}

Node Parser::ParseIf()
{
  Consume(TokenType::KIF, "Expected 'if'");

  Node ifNode;
  ifNode.nodeType = NodeType::TIf;

  Node cond = ParseExpr();
  NodeId condId = arena.create(cond);
  ifNode.body.push_back(condId);

  Consume(TokenType::LBrace, "Expected '{' after if condition");

  NodeId ifId = arena.create(ifNode);
  ParseBody(ifId);

  Consume(TokenType::RBrace, "Expected '}' after if body");

  return arena.get(ifId);
}

Node Parser::ParseWhile()
{
  Consume(TokenType::KWhile, "Expected 'if'");

  Node whileNode;
  whileNode.nodeType = NodeType::TWhile;

  Node cond = ParseExpr();
  NodeId condId = arena.create(cond);
  whileNode.body.push_back(condId);

  Consume(TokenType::LBrace, "Expected '{' after while condition");

  NodeId whileId = arena.create(whileNode);
  ParseBody(whileId);

  Consume(TokenType::RBrace, "Expected '}' after while body");

  return arena.get(whileId);
}

Node Parser::ParseReturn()
{
  Consume(TokenType::KReturn, "Expected 'return'");

  Node returnNode;
  returnNode.nodeType = NodeType::TRet;

  Node returnValue = ParseExpr();
  if (returnValue.nodeType != NodeType::TExpr)
  {
    AddError("Expected return value expression", Peek());
  }

  NodeId exprId = arena.create(returnValue);
  returnNode.body.push_back(exprId);

  returnNode.exprType = returnValue.exprType;

  Consume(TokenType::Semi, "Expected ';' after return statement");

  return returnNode;
}

Node Parser::ParseLet()
{
  Consume(TokenType::KLet, "Expected 'let'");

  Token varNameTok = Consume(TokenType::Id, "Expected variable name");
  std::string varName = std::string(varNameTok.value);

  Consume(TokenType::Colon, "Expected ':' after variable name");

  IgnType varType = ParseType();

  Node letNode;
  letNode.nodeType = NodeType::TLet;
  letNode.funcName = varName; // Store variable name in funcName field
  letNode.exprType = varType;

  if (Peek().type == TokenType::Assign)
  {
    Advance(); // consume '='
    Node initValue = ParseExpr();
    NodeId exprId = arena.create(initValue);
    letNode.body.push_back(exprId);
  }

  Consume(TokenType::Semi, "Expected ';' after variable declaration");

  return letNode;
}

Node Parser::ParseAssignment()
{
  Token varNameTok = Consume(TokenType::Id, "Expected variable name");
  std::string varName = std::string(varNameTok.value);

  if (Peek().type == TokenType::Dot)
  {
    Advance(); // consume '.'
    Token fieldNameTok =
        Consume(TokenType::Id, "Expected field name after '.'");
    std::string fieldName = std::string(fieldNameTok.value);

    Consume(TokenType::Assign, "Expected '='");

    Node assignNode;
    assignNode.nodeType = NodeType::TAssign;
    assignNode.exprKind = ExprType::ExprAssign;
    assignNode.funcName = varName;    // Store object name
    assignNode.fieldName = fieldName; // Store field name

    Node rhsExpr = ParseExpr();
    NodeId exprId = arena.create(rhsExpr);
    assignNode.body.push_back(exprId);

    return assignNode;
  }

  Consume(TokenType::Assign, "Expected '='");

  Node assignNode;
  assignNode.nodeType = NodeType::TAssign;
  assignNode.exprKind = ExprType::ExprAssign;
  assignNode.funcName = varName; // Store variable name

  Node rhsExpr = ParseExpr();
  NodeId exprId = arena.create(rhsExpr);
  assignNode.body.push_back(exprId);

  return assignNode;
}

Node Parser::ParseExpr()
{
  Node left = ParseLogic();
  while (Peek().type == TokenType::Operator &&
         (Peek().value == "+" || Peek().value == "-"))
  {
    Token op = Advance();
    Node right = ParseLogic();
    Node binNode;
    binNode.nodeType = NodeType::TExpr;
    binNode.exprKind = ExprType::ExprBinary;
    binNode.op = std::string(op.value);
    binNode.exprType.base = PrimType::PTI32;
    binNode.body.push_back(arena.create(left));
    binNode.body.push_back(arena.create(right));
    left = binNode;
  }
  return left;
}

Node Parser::ParseLogic()
{
  Node left = ParseTerm();
  while (Peek().type == TokenType::Operator &&
         (Peek().value == "<" || Peek().value == ">" || Peek().value == "<=" ||
          Peek().value == ">=" || Peek().value == "==" ||
          Peek().value == "!="))
  {
    Token op = Advance();
    Node right = ParseTerm();
    Node binNode;
    binNode.nodeType = NodeType::TExpr;
    binNode.exprKind = ExprType::ExprBinary;
    binNode.op = std::string(op.value);
    binNode.exprType.base = PrimType::PTI32;
    binNode.body.push_back(arena.create(left));
    binNode.body.push_back(arena.create(right));
    left = binNode;
  }
  return left;
}

Node Parser::ParseTerm()
{
  Node left = ParseFactor();

  // Handle field access
  while (Peek().type == TokenType::Dot)
  {
    Advance(); // consume '.'
    Token fieldNameTok =
        Consume(TokenType::Id, "Expected field name after '.'");

    Node fieldAccess;
    fieldAccess.nodeType = NodeType::TExpr;
    fieldAccess.exprKind = ExprType::ExprFieldAccess;
    fieldAccess.fieldObject = arena.create(left);
    fieldAccess.fieldName = std::string(fieldNameTok.value);
    fieldAccess.exprType.base =
        PrimType::PTI32; // TODO: Look up actual field type

    left = fieldAccess;
  }

  while (Peek().type == TokenType::Operator &&
         (Peek().value == "*" || Peek().value == "/" || Peek().value == "%"))
  {
    Token op = Advance();
    Node right = ParseFactor();
    Node binNode;
    binNode.nodeType = NodeType::TExpr;
    binNode.exprKind = ExprType::ExprBinary;
    binNode.op = std::string(op.value);
    binNode.exprType.base = PrimType::PTI32;
    binNode.body.push_back(arena.create(left));
    binNode.body.push_back(arena.create(right));
    left = binNode;
  }
  return left;
}

Node Parser::ParseFactor()
{
  Node exprNode;
  exprNode.nodeType = NodeType::TExpr;
  exprNode.exprType.isPtr = false;
  exprNode.exprType.isStruct = false;
  exprNode.exprType.isArray = false;
  exprNode.exprType.isRef = false;

  Token current = Peek();

  if (current.type == TokenType::Ampersand)
  {
    Advance(); // consume &
    Node operand = ParseFactor();
    exprNode.exprKind = ExprType::ExprUnary;
    exprNode.op = "&";
    exprNode.body.push_back(arena.create(operand));
    exprNode.exprType = operand.exprType;
    exprNode.exprType.isRef = true;
    return exprNode;
  }
  else if (current.type == TokenType::Star)
  {
    Advance(); // consume *
    Node operand = ParseFactor();
    exprNode.exprKind = ExprType::ExprUnary;
    exprNode.op = "*";
    exprNode.body.push_back(arena.create(operand));
    exprNode.exprType = operand.exprType;
    exprNode.exprType.isPtr = false;
    return exprNode;
  }

  switch (current.type)
  {
  case TokenType::ConstInt:
  {
    Token tok =
        Consume(TokenType::ConstInt, "Expected integer literal in expression");
    exprNode.exprType.base = PrimType::PTI32;
    exprNode.exprKind = ExprType::ExprInt;
    exprNode.intValue = std::stoi(std::string(tok.value));
    break;
  }
  case TokenType::ConstString:
  {
    Token tok = Consume(TokenType::ConstString,
                        "Expected string literal in expression");
    exprNode.exprType.base = PrimType::PTString;
    exprNode.exprKind = ExprType::ExprString;
    exprNode.stringValue = std::string(tok.value);
    break;
  }
  case TokenType::Id:
  {
    // Build possibly qualified identifier: Name1::Name2::Ident
    Token firstTok = Consume(TokenType::Id, "Expected identifier");
    std::string fullName = std::string(firstTok.value);
    while (Peek().type == TokenType::DoubleColon)
    {
      Advance(); // consume '::'
      Token part = Consume(TokenType::Id, "Expected identifier after '::'");
      fullName += "::";
      fullName += std::string(part.value);
    }

    if (Peek().type == TokenType::LParen)
    {
      exprNode.exprKind = ExprType::ExprFuncCall;
      exprNode.funcName = fullName;
      exprNode.exprType.base =
          PrimType::PTI32; // Placeholder until sema resolves
      Consume(TokenType::LParen, "Expected '('");
      while (Peek().type != TokenType::RParen && !IsAtEnd())
      {
        Node argNode = ParseExpr();
        NodeId argId = arena.create(argNode);
        exprNode.exprArgs.push_back(argId);
        if (Peek().type == TokenType::Comma)
        {
          Advance();
        }
        else
        {
          break;
        }
      }
      Consume(TokenType::RParen, "Expected ')' after function arguments");
    }
    else if (Peek().type == TokenType::LBrace)
    {
      // Struct constructor: Name { field: value, ... }
      exprNode.exprKind = ExprType::ExprStructConstructor;
      exprNode.funcName = fullName;
      exprNode.exprType.base = PrimType::PTUserType;
      exprNode.exprType.isStruct = true;

      Consume(TokenType::LBrace, "Expected '{'");
      while (Peek().type != TokenType::RBrace && !IsAtEnd())
      {
        Token fieldNameTok = Consume(TokenType::Id, "Expected field name");
        std::string fieldName = std::string(fieldNameTok.value);

        Consume(TokenType::Colon, "Expected ':' after field name");

        Node fieldValue = ParseExpr();
        NodeId fieldId = arena.create(fieldValue);
        exprNode.structFields[fieldName] = fieldId;

        if (Peek().type == TokenType::Comma)
        {
          Advance();
        }
        else if (Peek().type != TokenType::RBrace)
        {
          AddError("Expected ',' or '}' in struct constructor", Peek());
          break;
        }
      }
      Consume(TokenType::RBrace, "Expected '}' after struct fields");
    }
    else
    {
      // bare identifier (variable access or similar)
      exprNode.exprKind = ExprType::ExprInt; // placeholder
      exprNode.funcName = fullName;
      exprNode.exprType.base =
          PrimType::PTI32; // TODO: resolve actual type in sema
    }
    break;
  }
  default:
    AddError("Unsupported expression starting with token: " +
                 std::string(current.value),
             current);
    Advance();
    break;
  }
  return exprNode;
}

Token Parser::Peek(int offset)
{
  if (IsAtEnd())
  {
    return tokens.back();
  }
  return tokens[pos + offset];
}

Token Parser::Advance()
{
  if (!IsAtEnd())
    pos++;
  return Previous();
}

Token Parser::Previous() { return tokens[pos - 1]; }

bool Parser::IsAtEnd()
{
  return pos >= tokens.size() || tokens[pos].type == TokenType::EOF_TOKEN;
}

Token Parser::Consume(TokenType expectedType, std::string errMsg)
{
  if (Peek().type == expectedType)
  {
    lastConsumeSynthetic = false;
    return Advance();
  }
  AddError(errMsg + " Found: " + std::string(Peek().value), Peek());

  // Attempt to synchronize: advance until we find the expected token or a safe
  // recovery token
  while (!IsAtEnd())
  {
    TokenType t = Peek().type;
    if (t == expectedType)
    {
      return Advance();
    }

    if (t == TokenType::Semi || t == TokenType::RBrace ||
        t == TokenType::RParen || t == TokenType::Comma ||
        t == TokenType::EOF_TOKEN ||
        (expectedType == TokenType::RParen && t == TokenType::LBrace))
    {
      // stop synchronizing here; return a synthetic token for the expected type
      break;
    }
    Advance();
  }

  unsigned long curPos =
      pos < tokens.size() ? tokens[pos].pos : (unsigned long)source.size();
  std::string_view emptyView = curPos <= source.size()
                                   ? std::string_view(source.data() + curPos, 0)
                                   : std::string_view();
  lastConsumeSynthetic = true;
  return Token(expectedType, curPos, emptyView);
}

void Parser::Synchronize(TokenType expectedType)
{

  while (!IsAtEnd() && Peek().type != expectedType)
  {
    TokenType t = Peek().type;
    if (t == TokenType::Semi || t == TokenType::RBrace ||
        t == TokenType::RParen || t == TokenType::EOF_TOKEN)
    {
      return;
    }
    Advance();
  }
}

void Parser::AddError(const std::string &msg, const Token &tok)
{
  ParseError e;
  e.message = msg;
  e.pos = tok.pos;
  e.tokenValue = std::string(tok.value);

  unsigned long line = 1;
  unsigned long column = 1;
  unsigned long maxPos = std::min<unsigned long>(tok.pos, source.size());
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
  e.line = line;
  e.column = column;

  errors.push_back(e);
}

void Parser::ReportErrors(const std::string &filePath)
{
  if (errors.empty())
    return;
  std::cerr << "Found " << errors.size() << " parse error(s):\n";
  for (auto &e : errors)
  {
    std::cerr << "Error: " << e.message;
    std::cerr << " in " << filePath << " at line " << e.line << ", column "
              << e.column;
    if (!e.tokenValue.empty())
      std::cerr << " (token: '" << e.tokenValue << "')";
    std::cerr << "\n";
  }
}

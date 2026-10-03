#pragma once

#include "macros/syntax_protocol.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class RootNode;
class FunDecl;
class FunCall;
class ExpressionNode;
namespace zap {
class DiagnosticEngine;
}

namespace zap::ctfe {

enum class ValueType {
  Void,
  Bool,
  Int,
  String,
  SyntaxTokens,
  SyntaxSource,
  SyntaxExpr,
  SyntaxItem,
  Never
};

struct FunctionType {
  std::vector<ValueType> parameters;
  ValueType result;
};

using FunctionTypes = std::map<std::string, FunctionType>;

struct FunctionDefinition {
  std::string id;
  const FunDecl *declaration;
};

using FunctionLookup = std::function<std::optional<FunctionDefinition>(
    const FunDecl &, const ExpressionNode &)>;

struct ValidatedFunctions {
  FunctionTypes types;
  std::map<std::string, const FunDecl *> declarations;
  std::map<const FunCall *, std::string> calls;
};

bool validateFunctions(const std::vector<FunctionDefinition> &entries,
                       const FunctionLookup &lookup,
                       zap::DiagnosticEngine &diagnostics,
                       ValidatedFunctions &functions);

const FunctionTypes &builtinTypes();
ValueType syntaxValueType(const SyntaxValue &value);
std::string typeName(ValueType type);
std::optional<int64_t> integerLiteral(std::string_view text,
                                      bool negated = false);

} // namespace zap::ctfe

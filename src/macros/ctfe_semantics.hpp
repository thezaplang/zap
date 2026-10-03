#pragma once

#include "macros/syntax_protocol.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class RootNode;
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

const FunctionTypes &builtinTypes();
ValueType syntaxValueType(const SyntaxValue &value);
std::string typeName(ValueType type);
std::optional<int64_t> integerLiteral(std::string_view text,
                                      bool negated = false);
bool validateDefinition(const RootNode &root,
                        zap::DiagnosticEngine &diagnostics,
                        FunctionTypes &functions);

} // namespace zap::ctfe

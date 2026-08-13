#pragma once

#include <string>

namespace sema {

class BoundExpression;

class ConstantEvaluator {
public:
  static bool isConstant(const BoundExpression &expression,
                         std::string *failureReason = nullptr);
};

} // namespace sema

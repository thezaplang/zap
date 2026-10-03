#pragma once

#include "macros/ctfe_interpreter.hpp"

namespace zap::ctfe {

struct CtfeFailure {
  const char *code;
  std::string message;
};

using SyntaxHandle = std::shared_ptr<const SyntaxValue>;
using CtfeValue =
    std::variant<std::monostate, bool, int64_t, std::string, SyntaxHandle>;

inline void chargeParseBudget(const CtfeLimits &limits, size_t &used,
                              size_t count, size_t bytesPerUnit) {
  if (bytesPerUnit == 0)
    return;
  if (used > limits.maxMemoryBytes ||
      count > (limits.maxMemoryBytes - used) / bytesPerUnit)
    throw CtfeFailure{"M3003", "CTFE memory limit exceeded."};
  used += count * bytesPerUnit;
}

struct CtfeSyntaxBudget {
  const CtfeLimits &limits;
  size_t &memoryUsed;
  bool &observesLocations;

  void reserve(size_t count, size_t bytesPerUnit = 1) {
    chargeParseBudget(limits, memoryUsed, count, bytesPerUnit);
  }
};

std::optional<CtfeValue> evaluateSyntaxBuiltin(
    const std::string &name, const std::vector<CtfeValue> &arguments,
    const SyntaxMacroRequest &request, CtfeSyntaxBudget &budget);

} // namespace zap::ctfe

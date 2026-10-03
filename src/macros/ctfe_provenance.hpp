#pragma once

#include "macros/ctfe_runtime.hpp"

namespace zap::ctfe {

// Cache stores input ordinals, never compiler-owned pointers or old marks.
inline constexpr uint32_t GeneratedTokenIndex = UINT32_MAX;
std::vector<uint32_t> syntaxOutputProvenance(const SyntaxValue &output,
                                             const SyntaxValue &input,
                                             CtfeSyntaxBudget &budget);
void restoreSyntaxOutput(SyntaxValue &output, const SyntaxMacroRequest &request,
                         const std::vector<uint32_t> &provenance,
                         CtfeSyntaxBudget &budget);

} // namespace zap::ctfe

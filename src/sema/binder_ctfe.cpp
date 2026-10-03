#include "binder.hpp"

namespace sema {

bool Binder::predeclareCtfeFunction(ModuleState &module, FunDecl &function) {
  if (!function.isCtfeOnly())
    return false;
  if (function.attributes_.size() != 1 ||
      function.attributes_.front().hasArguments()) {
    error(
        function.span,
        "@ctfe must be the only function attribute and accepts no arguments.");
    return true;
  }
  auto symbol = std::make_shared<CompileTimeFunctionSymbol>(
      function.name_, module.info->moduleName, function.visibility_);
  if (!module.scope->declare(function.name_, symbol)) {
    error(function.span, "Compile-time function '" + function.name_ +
                             "' conflicts with another declaration.");
    return true;
  }
  module.symbol->members[function.name_] = symbol;
  if (function.visibility_ == Visibility::Public)
    module.symbol->exports[function.name_] = symbol;
  if (semanticInfo_)
    semanticInfo_->recordDeclaration(&function, symbol);
  return true;
}

} // namespace sema

#include "symbol.hpp"
#include "../ir/type_identity.hpp"

namespace sema {

bool sameFunctionSignature(const FunctionSymbol &lhs,
                           const FunctionSymbol &rhs) {
  if (lhs.parameters.size() != rhs.parameters.size() ||
      lhs.isCVariadic != rhs.isCVariadic) {
    return false;
  }
  for (size_t i = 0; i < lhs.parameters.size(); ++i) {
    const auto &left = lhs.parameters[i];
    const auto &right = rhs.parameters[i];
    if (left->is_ref != right->is_ref ||
        left->is_variadic_pack != right->is_variadic_pack) {
      return false;
    }
    if (!left->type || !right->type ||
        !zir::sameType(left->type, right->type)) {
      return false;
    }
  }
  return true;
}

} // namespace sema

#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace sema {
class BoundRootNode;
class FunctionSymbol;
} // namespace sema

namespace zir {

struct FunctionReachability {
  std::unordered_set<const sema::FunctionSymbol *> functions;
  std::unordered_set<const sema::FunctionSymbol *> externalFunctions;
  std::unordered_set<std::string> referencedFunctionLinkNames;
  std::unordered_set<std::string> liveClassCodegenNames;
  std::unordered_map<std::string, std::unordered_set<int>> liveVtableSlots;
};

class FunctionReachabilityAnalyzer {
public:
  FunctionReachability analyze(sema::BoundRootNode &root);
};

} // namespace zir

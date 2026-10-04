#pragma once

#include "control_flow_graph.hpp"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zir {

class Module;

class BorrowProvenance {
public:
    using OwnerSet = std::unordered_set<const Value*>;

    const OwnerSet& ownersOf(const std::shared_ptr<Value>& value) const;
    OwnerSet ownersAtDefinition(const std::shared_ptr<Value>& value) const;
    OwnerSet ownersOnEdge(
        const std::shared_ptr<Value>& value,
        const BasicBlock& source,
        const BasicBlock& destination
    ) const;
    const OwnerSet& ownersStoredAtExit(
        const BasicBlock& block,
        const std::shared_ptr<Value>& storage
    ) const;
    bool isLocalStorage(const std::shared_ptr<Value>& value) const;
    bool isEntryLoad(const BasicBlock& block, const std::shared_ptr<Value>& result) const;

private:
    using OwnerMap = std::unordered_map<const Value*, OwnerSet>;
    using StorageState = std::unordered_map<const Value*, OwnerSet>;
    using StorageStates = std::unordered_map<const BasicBlock*, StorageState>;

    OwnerMap owners_;
    OwnerMap loadOwners_;
    std::unordered_map<const Value*, std::shared_ptr<Value>> derivedFrom_;
    std::unordered_map<const Value*, std::vector<std::shared_ptr<Value>>> phiIncoming_;
    OwnerSet localStorage_;
    StorageStates exitStorage_;
    std::unordered_map<const BasicBlock*, OwnerSet> entryLoads_;

    friend BorrowProvenance analyzeBorrowProvenance(
        const Module& module,
        const Function& function,
        const ControlFlowGraph& cfg
    );
};

BorrowProvenance analyzeBorrowProvenance(
    const Module& module,
    const Function& function,
    const ControlFlowGraph& cfg
);

} // namespace zir

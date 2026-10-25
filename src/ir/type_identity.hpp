#pragma once

#include "type.hpp"
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace zir {

class TypeId {
public:
    bool operator==(const TypeId& other) const { return atoms_ == other.atoms_; }

    bool operator!=(const TypeId& other) const { return !(*this == other); }

    std::string mangleKey() const;

private:
    enum class AtomKind : uint8_t {
        Tag,
        Number,
        Name
    };

    struct Atom {
        AtomKind kind;
        uint64_t number = 0;
        std::string name;

        bool operator==(const Atom& other) const {
            return kind == other.kind && number == other.number && name == other.name;
        }
    };

    explicit TypeId(std::vector<Atom> atoms)
        : atoms_(std::move(atoms)) {}

    std::vector<Atom> atoms_;

    friend class TypeInterner;
    friend class TypeIdentityBuilder;
    friend struct TypeIdHash;
};

struct TypeIdHash {
    size_t operator()(const TypeId& id) const;
};

class TypeInterner {
public:
    TypeId intern(const Type& type) const;
    TypeId intern(const std::shared_ptr<Type>& type) const;
    std::string mangleKey(const std::shared_ptr<Type>& type) const;

    bool same(const std::shared_ptr<Type>& lhs, const std::shared_ptr<Type>& rhs) const;

    size_t size() const { return identities_.size(); }

private:
    mutable std::unordered_set<TypeId, TypeIdHash> identities_;
};

bool sameType(const std::shared_ptr<Type>& lhs, const std::shared_ptr<Type>& rhs);
std::string typeMangleKey(const std::shared_ptr<Type>& type);

} // namespace zir

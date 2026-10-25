#include "type_identity.hpp"

#include <functional>
#include <stdexcept>

namespace zir {
namespace {

enum class IdentityTag : uint64_t {
    Intrinsic = 0,
    Primitive = 1,
    Pointer = 2,
    Record = 3,
    Class = 4,
    Array = 5,
    Enum = 6,
    TaggedUnion = 7,
    FunctionPointer = 8,
};

void hashCombine(size_t& seed, size_t value) {
    seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

} // namespace

class TypeIdentityBuilder {
public:
    static TypeId build(const Type& type) {
        std::vector<TypeId::Atom> atoms;
        append(type, atoms);
        return TypeId(std::move(atoms));
    }

private:
    static void append(const Type& type, std::vector<TypeId::Atom>& atoms) {
        auto tag = [&](IdentityTag value) {
            atoms.push_back({TypeId::AtomKind::Tag, static_cast<uint64_t>(value), {}});
        };
        auto number = [&](uint64_t value) {
            atoms.push_back({TypeId::AtomKind::Number, value, {}});
        };
        auto name = [&](const std::string& value) {
            atoms.push_back({TypeId::AtomKind::Name, 0, value});
        };

        if (type.getIntrinsicKind() != IntrinsicTypeKind::None) {
            tag(IdentityTag::Intrinsic);
            number(static_cast<uint64_t>(type.getIntrinsicKind()));
            return;
        }

        switch (type.getKind()) {
            case TypeKind::Pointer: {
                tag(IdentityTag::Pointer);
                const auto& pointer = static_cast<const PointerType&>(type);
                append(*pointer.getBaseType(), atoms);
                return;
            }
            case TypeKind::Array: {
                tag(IdentityTag::Array);
                const auto& array = static_cast<const ArrayType&>(type);
                number(array.getSize());
                append(*array.getBaseType(), atoms);
                return;
            }
            case TypeKind::FunctionPointer: {
                tag(IdentityTag::FunctionPointer);
                const auto& function = static_cast<const FunctionPointerType&>(type);
                number(function.getParams().size());
                for (size_t i = 0; i < function.getParams().size(); ++i) {
                    append(*function.getParams()[i], atoms);
                    number(static_cast<uint64_t>(function.getParameterOwnership()[i]));
                    number(static_cast<uint64_t>(function.getParameterEscapes()[i]));
                }
                append(*function.getReturnType(), atoms);
                number(function.returnsRef() ? 1 : 0);
                number(function.getResultBorrow().hasSource() ? 1 : 0);
                if (function.getResultBorrow().hasSource()) {
                    number(*function.getResultBorrow().sourceParameter());
                }
                return;
            }
            case TypeKind::Record: {
                tag(IdentityTag::Record);
                const auto& record = static_cast<const RecordType&>(type);
                number(static_cast<uint64_t>(record.getRole()));
                name(record.getCodegenName());
                return;
            }
            case TypeKind::Class: {
                tag(IdentityTag::Class);
                const auto& classType = static_cast<const ClassType&>(type);
                name(classType.getCodegenName());
                number(classType.isWeak() ? 1 : 0);
                return;
            }
            case TypeKind::Enum:
                tag(IdentityTag::Enum);
                name(static_cast<const EnumType&>(type).getCodegenName());
                return;
            case TypeKind::TaggedUnion:
                tag(IdentityTag::TaggedUnion);
                name(static_cast<const TaggedUnionType&>(type).getCodegenName());
                return;
            case TypeKind::Void:
            case TypeKind::Int8:
            case TypeKind::Int16:
            case TypeKind::Int32:
            case TypeKind::Int64:
            case TypeKind::UInt8:
            case TypeKind::UInt16:
            case TypeKind::UInt32:
            case TypeKind::UInt64:
            case TypeKind::Int:
            case TypeKind::UInt:
            case TypeKind::Float:
            case TypeKind::Float32:
            case TypeKind::Float64:
            case TypeKind::Bool:
            case TypeKind::Char:
            case TypeKind::NullPtr:
                tag(IdentityTag::Primitive);
                number(static_cast<uint64_t>(canonicalPrimitiveKind(type.getKind())));
                return;
        }
    }
};

size_t TypeIdHash::operator()(const TypeId& id) const {
    size_t result = 0;
    for (const auto& atom : id.atoms_) {
        hashCombine(result, std::hash<uint8_t>{}(static_cast<uint8_t>(atom.kind)));
        hashCombine(result, std::hash<uint64_t>{}(atom.number));
        hashCombine(result, std::hash<std::string>{}(atom.name));
    }
    return result;
}

std::string TypeId::mangleKey() const {
    static constexpr char hexDigits[] = "0123456789abcdef";
    std::string result = "z1";
    for (const auto& atom : atoms_) {
        switch (atom.kind) {
            case AtomKind::Tag:
                result += 't';
                result += std::to_string(atom.number);
                result += '_';
                break;
            case AtomKind::Number:
                result += 'n';
                result += std::to_string(atom.number);
                result += '_';
                break;
            case AtomKind::Name:
                result += 's';
                result += std::to_string(atom.name.size());
                result += '_';
                for (unsigned char byte : atom.name) {
                    result += hexDigits[byte >> 4U];
                    result += hexDigits[byte & 0x0fU];
                }
                result += '_';
                break;
        }
    }
    return result;
}

TypeId TypeInterner::intern(const Type& type) const {
    return *identities_.emplace(TypeIdentityBuilder::build(type)).first;
}

TypeId TypeInterner::intern(const std::shared_ptr<Type>& type) const {
    if (!type) {
        throw std::invalid_argument("cannot intern a null type");
    }
    return intern(*type);
}

std::string TypeInterner::mangleKey(const std::shared_ptr<Type>& type) const {
    return intern(type).mangleKey();
}

bool TypeInterner::same(const std::shared_ptr<Type>& lhs, const std::shared_ptr<Type>& rhs) const {
    if (lhs == rhs) {
        return true;
    }
    if (!lhs || !rhs) {
        return false;
    }
    return intern(lhs) == intern(rhs);
}

bool sameType(const std::shared_ptr<Type>& lhs, const std::shared_ptr<Type>& rhs) {
    TypeInterner interner;
    return interner.same(lhs, rhs);
}

std::string typeMangleKey(const std::shared_ptr<Type>& type) {
    TypeInterner interner;
    return interner.mangleKey(type);
}

} // namespace zir

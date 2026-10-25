#include "zir_verifier_internal.hpp"

namespace zir::verifier_detail {

bool isAssignable(
    const std::shared_ptr<Type>& actual,
    const std::shared_ptr<Type>& expected,
    TypeInterner& typeInterner
) {
    if (typeInterner.same(actual, expected)) {
        return true;
    }
    if (!actual || !expected) {
        return false;
    }
    if (actual->getKind() == TypeKind::NullPtr) {
        return expected->getKind() == TypeKind::Pointer
            || expected->getKind() == TypeKind::FunctionPointer
            || expected->getKind() == TypeKind::Class;
    }
    if (actual->getKind() != TypeKind::Class || expected->getKind() != TypeKind::Class) {
        return false;
    }

    const auto actualClass = std::static_pointer_cast<ClassType>(actual);
    const auto expectedClass = std::static_pointer_cast<ClassType>(expected);
    if (actualClass->isWeak() && !expectedClass->isWeak()) {
        return false;
    }
    for (auto current = actualClass; current; current = current->getBase()) {
        if (current->getCodegenName() == expectedClass->getCodegenName()) {
            return true;
        }
    }
    return false;
}

bool isTerminator(OpCode opcode) {
    return opcode == OpCode::Br || opcode == OpCode::CondBr || opcode == OpCode::Ret;
}

bool isStringType(const std::shared_ptr<Type>& type) {
    return type && type->getIntrinsicKind() != IntrinsicTypeKind::None;
}

std::shared_ptr<Value> instructionResult(const Instruction& instruction) {
    switch (instruction.getOpCode()) {
        case OpCode::Alloca:
            return static_cast<const AllocaInst&>(instruction).getResult();
        case OpCode::Load:
            return static_cast<const LoadInst&>(instruction).getResult();
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::Mul:
        case OpCode::SDiv:
        case OpCode::UDiv:
        case OpCode::SRem:
        case OpCode::URem:
        case OpCode::Shl:
        case OpCode::LShr:
        case OpCode::AShr:
        case OpCode::BitAnd:
        case OpCode::BitOr:
        case OpCode::BitXor:
            return static_cast<const BinaryInst&>(instruction).getResult();
        case OpCode::Cmp:
            return static_cast<const CmpInst&>(instruction).getResult();
        case OpCode::Call:
            return static_cast<const CallInst&>(instruction).getResult();
        case OpCode::Alloc:
            return static_cast<const AllocInst&>(instruction).getResult();
        case OpCode::GetElementPtr:
            return static_cast<const GetElementPtrInst&>(instruction).getResult();
        case OpCode::Phi:
            return static_cast<const PhiInst&>(instruction).getResult();
        case OpCode::Cast:
            return static_cast<const CastInst&>(instruction).getResult();
        case OpCode::Copy:
            return static_cast<const CopyInst&>(instruction).getResult();
        case OpCode::Move:
            return static_cast<const MoveInst&>(instruction).getResult();
        case OpCode::Borrow:
            return static_cast<const BorrowInst&>(instruction).getResult();
        case OpCode::WeakLock:
            return static_cast<const WeakLockInst&>(instruction).getResult();
        case OpCode::WeakAlive:
            return static_cast<const WeakAliveInst&>(instruction).getResult();
        case OpCode::ClassIs:
            return static_cast<const ClassIsInst&>(instruction).getResult();
        default:
            return nullptr;
    }
}

std::string typeName(const std::shared_ptr<Type>& type) {
    return type ? type->toString() : "<null type>";
}

} // namespace zir::verifier_detail

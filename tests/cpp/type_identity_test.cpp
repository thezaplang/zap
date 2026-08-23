#include "ir/failable_type.hpp"
#include "ir/string_type.hpp"
#include "ir/type_identity.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using zir::ArrayType;
using zir::ClassType;
using zir::FunctionPointerType;
using zir::ParameterEscape;
using zir::ParameterOwnership;
using zir::PointerType;
using zir::PrimitiveType;
using zir::RecordType;
using zir::ResultBorrowContract;
using zir::Type;
using zir::TypeInterner;
using zir::TypeKind;

std::shared_ptr<Type> primitive(TypeKind kind) {
  return std::make_shared<PrimitiveType>(kind);
}

bool expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

bool testDisplayNamesDoNotDefineIdentity() {
  TypeInterner types;
  return expect(
             !types.same(primitive(TypeKind::Int), primitive(TypeKind::Int32)),
             "native Int and fixed Int32 have equal identities") &&
         expect(
             !types.same(primitive(TypeKind::Int), primitive(TypeKind::Int64)),
             "native Int and fixed Int64 have equal identities") &&
         expect(!types.same(primitive(TypeKind::UInt),
                            primitive(TypeKind::UInt64)),
                "native UInt and fixed UInt64 have equal identities") &&
         expect(types.same(primitive(TypeKind::Float),
                           primitive(TypeKind::Float32)),
                "Float and its fixed Float32 spelling have different "
                "identities") &&
         expect(
             !types.same(primitive(TypeKind::Char), primitive(TypeKind::Int8)),
             "Char and Int8 were merged because both render as i8");
}

bool testStructuralTypes() {
  TypeInterner types;
  auto lhsPointer = std::make_shared<PointerType>(primitive(TypeKind::UInt16));
  auto rhsPointer = std::make_shared<PointerType>(primitive(TypeKind::UInt16));
  auto differentPointer =
      std::make_shared<PointerType>(primitive(TypeKind::Int16));

  auto lhsArray = std::make_shared<ArrayType>(lhsPointer, 4);
  auto rhsArray = std::make_shared<ArrayType>(rhsPointer, 4);
  auto differentArray = std::make_shared<ArrayType>(rhsPointer, 8);

  auto lhsFunction = std::make_shared<FunctionPointerType>(
      std::vector<std::shared_ptr<Type>>{lhsArray}, primitive(TypeKind::Bool));
  auto rhsFunction = std::make_shared<FunctionPointerType>(
      std::vector<std::shared_ptr<Type>>{rhsArray}, primitive(TypeKind::Bool));
  auto borrowedFunction = std::make_shared<FunctionPointerType>(
      std::vector<std::shared_ptr<Type>>{zir::makeStringType()},
      primitive(TypeKind::Void),
      std::vector<ParameterOwnership>{ParameterOwnership::Borrow});
  auto transferringFunction = std::make_shared<FunctionPointerType>(
      std::vector<std::shared_ptr<Type>>{zir::makeStringType()},
      primitive(TypeKind::Void),
      std::vector<ParameterOwnership>{ParameterOwnership::Transfer});
  auto sinkingFunction = std::make_shared<FunctionPointerType>(
      std::vector<std::shared_ptr<Type>>{zir::makeStringType()},
      primitive(TypeKind::Void),
      std::vector<ParameterOwnership>{ParameterOwnership::Sink});
  auto noescapeFunction = std::make_shared<FunctionPointerType>(
      std::vector<std::shared_ptr<Type>>{zir::makeStringViewType()},
      primitive(TypeKind::Void),
      std::vector<ParameterOwnership>{ParameterOwnership::Borrow},
      std::vector<ParameterEscape>{ParameterEscape::NoEscape});
  auto unspecifiedEscapeFunction = std::make_shared<FunctionPointerType>(
      std::vector<std::shared_ptr<Type>>{zir::makeStringViewType()},
      primitive(TypeKind::Void),
      std::vector<ParameterOwnership>{ParameterOwnership::Borrow},
      std::vector<ParameterEscape>{ParameterEscape::Unspecified});
  auto borrowedResultFunction = std::make_shared<FunctionPointerType>(
      std::vector<std::shared_ptr<Type>>{zir::makeStringViewType()},
      zir::makeStringViewType(),
      std::vector<ParameterOwnership>{ParameterOwnership::Borrow},
      std::vector<ParameterEscape>{ParameterEscape::Unspecified},
      ResultBorrowContract::fromParameter(0));
  auto unspecifiedResultFunction = std::make_shared<FunctionPointerType>(
      std::vector<std::shared_ptr<Type>>{zir::makeStringViewType()},
      zir::makeStringViewType(),
      std::vector<ParameterOwnership>{ParameterOwnership::Borrow},
      std::vector<ParameterEscape>{ParameterEscape::Unspecified});
  auto refResultFunction = std::make_shared<FunctionPointerType>(
      std::vector<std::shared_ptr<Type>>{}, primitive(TypeKind::Int),
      std::vector<ParameterOwnership>{}, std::vector<ParameterEscape>{},
      ResultBorrowContract{}, true);
  auto valueResultFunction = std::make_shared<FunctionPointerType>(
      std::vector<std::shared_ptr<Type>>{}, primitive(TypeKind::Int));

  return expect(types.same(lhsPointer, rhsPointer),
                "equal pointer types have different identities") &&
         expect(!types.same(lhsPointer, differentPointer),
                "different pointer base types have equal identities") &&
         expect(types.same(lhsArray, rhsArray),
                "equal array types have different identities") &&
         expect(!types.same(lhsArray, differentArray),
                "array size is absent from type identity") &&
         expect(types.same(lhsFunction, rhsFunction),
                "equal function types have different identities") &&
         expect(!types.same(borrowedFunction, transferringFunction),
                "function parameter ownership is absent from type identity") &&
         expect(!types.same(transferringFunction, sinkingFunction),
                "sink is absent from function parameter type identity") &&
         expect(!types.same(noescapeFunction, unspecifiedEscapeFunction),
                "escape contract is absent from function type identity") &&
         expect(
             !types.same(borrowedResultFunction, unspecifiedResultFunction),
             "result borrow contract is absent from function type identity") &&
         expect(!types.same(refResultFunction, valueResultFunction),
                "ref return is absent from function type identity");
}

bool testNominalAndQualifiedTypes() {
  TypeInterner types;
  auto first = std::make_shared<RecordType>("VisibleName", "module.Type");
  auto renamed = std::make_shared<RecordType>("Alias", "module.Type");
  auto other = std::make_shared<RecordType>("VisibleName", "other.Type");

  auto strong = std::make_shared<ClassType>("Node", "module.Node");
  auto weak = std::make_shared<ClassType>(*strong);
  weak->setWeak(true);

  return expect(types.same(first, renamed),
                "nominal identity does not use the declaration codegen name") &&
         expect(!types.same(first, other),
                "different nominal declarations have equal identities") &&
         expect(!types.same(strong, weak),
                "weak qualification is absent from class identity") &&
         expect(types.same(zir::makeStringType(), zir::makeStringType()),
                "intrinsic String identity is not canonical") &&
         expect(!types.same(zir::makeStringType(), zir::makeStringViewType()),
                "String and StringView have equal intrinsic identities");
}

bool testInternerDeduplicatesIdentity() {
  TypeInterner types;
  types.intern(primitive(TypeKind::UInt64));
  types.intern(primitive(TypeKind::UInt64));
  return expect(types.size() == 1,
                "interner retained duplicate canonical identities");
}

bool testSyntheticRecordRolesAreNotNameProtocols() {
  TypeInterner types;
  auto userRecord = std::make_shared<RecordType>("failable$legacy");
  auto failable = zir::makeFailableRecordType(primitive(TypeKind::Int),
                                              primitive(TypeKind::Int));
  auto userNamedLikeOldVariadic =
      std::make_shared<RecordType>("__zap_varargs_legacy");
  auto variadic = std::make_shared<RecordType>("slice", "slice",
                                               zir::IntrinsicTypeKind::None,
                                               zir::RecordRole::VariadicView);
  auto userT = std::make_shared<RecordType>("T");
  auto genericT = zir::makeGenericParameterType("T");

  return expect(!zir::getFailableTypeLayout(userRecord),
                "a user record was recognized as failable by its name") &&
         expect(zir::getFailableTypeLayout(failable).has_value(),
                "tagged failable record was not recognized") &&
         expect(userNamedLikeOldVariadic->getRole() == zir::RecordRole::User &&
                    variadic->getRole() == zir::RecordRole::VariadicView,
                "variadic view role depends on its generated name") &&
         expect(!types.same(userT, genericT),
                "user record and generic parameter have equal identities");
}

bool testMangleKeysAreCanonicalAndCollisionFree() {
  TypeInterner types;
  auto intType = primitive(TypeKind::Int);
  auto int32Type = primitive(TypeKind::Int32);
  auto floatType = primitive(TypeKind::Float);
  auto float32Type = primitive(TypeKind::Float32);
  auto dottedName = std::make_shared<RecordType>("Type", "module.a-b");
  auto dashedName = std::make_shared<RecordType>("Type", "module_a.b");
  auto firstPointer = std::make_shared<PointerType>(intType);
  auto secondPointer = std::make_shared<PointerType>(primitive(TypeKind::Int));

  return expect(types.mangleKey(intType) != types.mangleKey(int32Type),
                "Int and Int32 have colliding mangle keys") &&
         expect(types.mangleKey(floatType) == types.mangleKey(float32Type),
                "Float and Float32 aliases have different mangle keys") &&
         expect(types.mangleKey(dottedName) != types.mangleKey(dashedName),
                "nominal names collide after mangle encoding") &&
         expect(types.mangleKey(firstPointer) == types.mangleKey(secondPointer),
                "equal structural types have different mangle keys");
}

} // namespace

int main() {
  bool ok = true;
  ok = testDisplayNamesDoNotDefineIdentity() && ok;
  ok = testStructuralTypes() && ok;
  ok = testNominalAndQualifiedTypes() && ok;
  ok = testInternerDeduplicatesIdentity() && ok;
  ok = testSyntheticRecordRolesAreNotNameProtocols() && ok;
  ok = testMangleKeysAreCanonicalAndCollisionFree() && ok;
  return ok ? 0 : 1;
}

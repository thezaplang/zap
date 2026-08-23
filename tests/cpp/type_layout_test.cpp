#include "ir/string_type.hpp"
#include "sema/type_layout.hpp"

#include <iostream>
#include <memory>
#include <string>

namespace {

std::shared_ptr<zir::Type> primitive(zir::TypeKind kind) {
  return std::make_shared<zir::PrimitiveType>(kind);
}

bool expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

bool testNativePrimitiveLayouts() {
  sema::TargetInfo target32{32};
  sema::TargetInfo target64{64};

  auto nativeInt = primitive(zir::TypeKind::Int);
  auto fixedInt64 = primitive(zir::TypeKind::Int64);
  auto pointer = std::make_shared<zir::PointerType>(nativeInt);

  auto int32Layout = sema::computeTypeLayout(nativeInt, target32);
  auto int64Layout = sema::computeTypeLayout(nativeInt, target64);
  auto fixedOn32 = sema::computeTypeLayout(fixedInt64, target32);
  auto pointer32 = sema::computeTypeLayout(pointer, target32);
  auto pointer64 = sema::computeTypeLayout(pointer, target64);

  return expect(int32Layout.size == 4 && int32Layout.align == 4,
                "native Int is not 32-bit on a 32-bit target") &&
         expect(int64Layout.size == 8 && int64Layout.align == 8,
                "native Int is not 64-bit on a 64-bit target") &&
         expect(fixedOn32.size == 8 && fixedOn32.align == 4,
                "fixed Int64 changed size on a 32-bit target") &&
         expect(pointer32.size == 4 && pointer64.size == 8,
                "pointer layout does not follow the target word size");
}

bool testTargetDependentAggregateLayouts() {
  sema::TargetInfo target32{32};
  sema::TargetInfo target64{64};
  auto record = std::make_shared<zir::RecordType>("NativeRecord");
  record->addField("tag", primitive(zir::TypeKind::UInt8));
  record->addField("value", primitive(zir::TypeKind::Int));

  auto record32 = sema::computeTypeLayout(record, target32);
  auto record64 = sema::computeTypeLayout(record, target64);
  auto string32 = sema::computeTypeLayout(zir::makeStringType(), target32);
  auto string64 = sema::computeTypeLayout(zir::makeStringType(), target64);

  return expect(record32.size == 8 && record32.align == 4,
                "record layout did not use a 32-bit native Int") &&
         expect(record64.size == 16 && record64.align == 8,
                "record layout did not use a 64-bit native Int") &&
         expect(
             string32.size == 12 && string32.align == 4,
             "32-bit String layout disagrees with pointer plus i64 length") &&
         expect(string64.size == 16 && string64.align == 8,
                "64-bit String layout changed unexpectedly");
}

} // namespace

int main() {
  bool ok = true;
  ok = testNativePrimitiveLayouts() && ok;
  ok = testTargetDependentAggregateLayouts() && ok;
  return ok ? 0 : 1;
}

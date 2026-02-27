#pragma once

#include <cstdint>
#include <llvm/Support/raw_ostream.h>
#include <ostream>

namespace zap {
///
/// @brief Base class for the version.
///
template <typename T> class _version_base {
private:
  T maj;   ///< Major version. X.y.z
  T min;   ///< Minor version. x.Y.z
  T patch; ///< Patch version. x.y.Z
public:
  constexpr _version_base(T major_, T minor_, T patch_) noexcept
      : maj(major_), min(minor_), patch(patch_) {}

  constexpr T get_major() const noexcept { return maj; }

  constexpr T get_minor() const noexcept { return min; }

  constexpr T get_patch() const noexcept { return patch; }

  friend std::ostream &operator<<(std::ostream &os, const _version_base &v) {
    return os << v.get_major() << '.' << v.get_minor() << '.' << v.get_patch();
  }

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                                       const _version_base &v) {
    return os << v.get_major() << '.' << v.get_minor() << '.' << v.get_patch();
  }
};

///
/// @brief Can be changed to any integer or float type.
///
using version = _version_base<uint16_t>;
} // namespace zap
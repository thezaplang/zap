#pragma once

#include <string_view>

enum class BindingKind { Mutable, Immutable, CompileTimeConstant };

inline std::string_view bindingKindKeyword(BindingKind kind) noexcept {
  switch (kind) {
  case BindingKind::Mutable:
    return "var";
  case BindingKind::Immutable:
    return "let";
  case BindingKind::CompileTimeConstant:
    return "const";
  }
  return "var";
}

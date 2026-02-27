#pragma once

#include "driver/version.hpp"

#define ZAP_NAME_MACRO "zapc"

namespace zap {

///
/// @brief Name that displays when --help is used.
///
constexpr const char *ZAP_NAME = ZAP_NAME_MACRO;
///
/// @brief Zap's version, displayed when --version is used.
///
constexpr version ZAP_VERSION(0, 1, 0);

} // namespace zap
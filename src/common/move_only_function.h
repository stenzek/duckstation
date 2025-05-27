// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <functional>
#include <version>

#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L

template<class... Signature>
using MoveOnlyFunction = std::move_only_function<Signature...>;

#else

// folly::Function only implements a subset of std::move_only_function. Stick to signatures of the form R(Args...),
// optionally const and/or noexcept, without ref-qualifiers. std::in_place_type construction is not supported, and the
// callable must be move-constructible.
#include "thirdparty/FollyFunction.h"

template<class... Signature>
using MoveOnlyFunction = folly::Function<Signature...>;

#endif

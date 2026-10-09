//
// BitUtils.hpp
// Small bitfield and array helpers used by register code.
// Mask callers must supply bit indices within a 32-bit word. These helpers
// do not validate hardware register definitions.
// See LICENSE for distribution terms.
//

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cirrus::support {

constexpr uint32_t bit(uint32_t index) {
    return 1U << index;
}

constexpr uint32_t genMask(uint32_t high, uint32_t low) {
    return (~0U >> (31U - high)) & (~0U << low);
}

template <typename T, size_t N>
constexpr size_t arraySize(const T (&)[N]) {
    return N;
}

static_assert(genMask(3, 0) == 0x0000000FU, "genMask must match Linux GENMASK");
static_assert(genMask(31, 0) == 0xFFFFFFFFU, "genMask must match Linux GENMASK");
static_assert(genMask(15, 8) == 0x0000FF00U, "genMask must match Linux GENMASK");

template <typename E>
struct EnableBitmaskOperators {
    static constexpr bool kEnabled = false;
};

template <bool Condition, typename T = void>
struct EnableIf {};

template <typename T>
struct EnableIf<true, T> {
    using Type = T;
};

template <typename E>
using BitmaskEnum = typename EnableIf<EnableBitmaskOperators<E>::kEnabled, E>::Type;

template <typename E>
constexpr auto toUnderlying(E value) {
    return static_cast<__underlying_type(E)>(value);
}

}

#define CIRRUS_ENABLE_BITMASK_OPERATORS(Enum)                                                                                              \
    template <>                                                                                                                            \
    struct cirrus::support::EnableBitmaskOperators<Enum> {                                                                                 \
        static constexpr bool kEnabled = true;                                                                                             \
    };

template <typename E>
constexpr cirrus::support::BitmaskEnum<E> operator|(E lhs, E rhs) {
    return static_cast<E>(cirrus::support::toUnderlying(lhs) | cirrus::support::toUnderlying(rhs));
}

template <typename E>
constexpr cirrus::support::BitmaskEnum<E> operator&(E lhs, E rhs) {
    return static_cast<E>(cirrus::support::toUnderlying(lhs) & cirrus::support::toUnderlying(rhs));
}

#pragma once

#include <type_traits>
#include "q16_16.h"
#include "q32_32.h"

namespace math {

template<typename T>
struct is_fixed_point : std::false_type {};

template<>
struct is_fixed_point<q16_16> : std::true_type {};

template<>
struct is_fixed_point<q32_32> : std::true_type {};

template<typename T>
inline constexpr bool is_fixed_point_v = is_fixed_point<T>::value;

template<typename T>
T to_fixed(float v);

template<>
inline q16_16 to_fixed<q16_16>(float v) { return q16_16(v); }

template<>
inline q32_32 to_fixed<q32_32>(float v) { return q32_32(v); }

template<typename T>
float to_float(T v);

template<>
inline float to_float<q16_16>(q16_16 v) { return v.to_float(); }

template<>
inline float to_float<q32_32>(q32_32 v) { return v.to_float(); }

} // namespace math

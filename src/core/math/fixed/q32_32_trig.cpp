#include "q32_32_trig.h"

#include <array>
#include <cstdint>
#include <limits>

namespace math {
namespace {

constexpr q32_32 kOne{int32_t{1}};
constexpr q32_32 kMinusOne{int32_t{-1}};
constexpr q32_32 kPi = q32_32::from_raw(13493037705ll);
constexpr q32_32 kHalfPi = q32_32::from_raw(6746518852ll);
constexpr q32_32 kTwoPi = q32_32::from_raw(26986075409ll);

constexpr std::array<int64_t, 32> kCordicAnglesRaw{
    3373259426ll, 1991351318ll, 1052175346ll, 534100635ll,
    268086748ll, 134174063ll, 67103403ll, 33553749ll,
    16777131ll, 8388597ll, 4194303ll, 2097152ll,
    1048576ll, 524288ll, 262144ll, 131072ll,
    65536ll, 32768ll, 16384ll, 8192ll,
    4096ll, 2048ll, 1024ll, 512ll,
    256ll, 128ll, 64ll, 32ll,
    16ll, 8ll, 4ll, 2ll,
};

[[nodiscard]] constexpr uint64_t unsignedMagnitude(int64_t value) noexcept {
    return value < 0
        ? static_cast<uint64_t>(-(value + 1)) + uint64_t{1}
        : static_cast<uint64_t>(value);
}

[[nodiscard]] q32_32 clampUnit(q32_32 value) noexcept {
    return q32_32::clamp(value, kMinusOne, kOne);
}

} // namespace

q32_32_sincos fixed_sincos(q32_32 radians) noexcept {
    int64_t raw = radians.raw() % kTwoPi.raw();
    if (raw > kPi.raw()) raw -= kTwoPi.raw();
    if (raw < -kPi.raw()) raw += kTwoPi.raw();
    q32_32 value = q32_32::from_raw(raw);
    q32_32 cosineSign = kOne;
    if (value > kHalfPi) {
        value = kPi - value;
        cosineSign = kMinusOne;
    } else if (value < -kHalfPi) {
        value = -kPi - value;
        cosineSign = kMinusOne;
    }

    const q32_32 squared = value * value;
    const q32_32 sinPolynomial = kOne + squared * (
        -q32_32::from_fraction(1, 6) + squared * (
        q32_32::from_fraction(1, 120) + squared * (
        -q32_32::from_fraction(1, 5040) + squared *
        q32_32::from_fraction(1, 362880))));
    const q32_32 cosPolynomial = kOne + squared * (
        -q32_32::from_fraction(1, 2) + squared * (
        q32_32::from_fraction(1, 24) + squared * (
        -q32_32::from_fraction(1, 720) + squared *
        q32_32::from_fraction(1, 40320))));
    return {
        .sine = clampUnit(value * sinPolynomial),
        .cosine = clampUnit(cosineSign * cosPolynomial),
    };
}

q32_32 fixed_sin(q32_32 radians) noexcept {
    return fixed_sincos(radians).sine;
}

q32_32 fixed_cos(q32_32 radians) noexcept {
    return fixed_sincos(radians).cosine;
}

q32_32 fixed_atan2(q32_32 y, q32_32 x) noexcept {
    int64_t vectorX = x.raw();
    int64_t vectorY = y.raw();
    if (vectorX == 0 && vectorY == 0) return q32_32{};

    // CORDIC vectoring grows the working vector by about 1.647. Normalize
    // only by powers of two so the angle remains unchanged and every target
    // uses identical integer operations without overflowing int64_t.
    constexpr uint64_t kSafeMagnitude =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / 4u;
    while (unsignedMagnitude(vectorX) > kSafeMagnitude ||
           unsignedMagnitude(vectorY) > kSafeMagnitude) {
        vectorX >>= 1;
        vectorY >>= 1;
    }

    int64_t angleRaw = 0;
    if (vectorX < 0) {
        angleRaw = vectorY >= 0 ? kPi.raw() : -kPi.raw();
        vectorX = -vectorX;
        vectorY = -vectorY;
    }
    for (size_t index = 0; index < kCordicAnglesRaw.size(); ++index) {
        if (vectorY == 0) break;
        const int64_t oldX = vectorX;
        if (vectorY > 0) {
            vectorX += vectorY >> index;
            vectorY -= oldX >> index;
            angleRaw += kCordicAnglesRaw[index];
        } else {
            vectorX -= vectorY >> index;
            vectorY += oldX >> index;
            angleRaw -= kCordicAnglesRaw[index];
        }
    }
    return q32_32::from_raw(angleRaw);
}

} // namespace math

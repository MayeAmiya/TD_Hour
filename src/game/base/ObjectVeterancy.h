#pragma once

#include <cstdint>

namespace game {

// 跨数据目录、对象配方和模拟层共享的中立值协议。
enum class ObjectVeterancyLevel : uint8_t {
    Regular,
    Veteran,
    Elite,
    Heroic,
};

using ObjectVeterancyMask = uint8_t;

[[nodiscard]] constexpr ObjectVeterancyMask
objectVeterancyBit(ObjectVeterancyLevel level) noexcept {
    return static_cast<ObjectVeterancyMask>(ObjectVeterancyMask{1} <<
                                             static_cast<uint8_t>(level));
}

} // namespace game

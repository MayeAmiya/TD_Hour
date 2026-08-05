#pragma once
#include <compare>
#include <cstddef>
#include <cstdint>

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "game/object/ai/contracts/AIOrderIdentity.h"
namespace engine::ai
{

// Signed 32.32 raw coordinates keep this staging protocol independent from
// the SIMD/compiler details of the concrete fixed-point implementation. The
// future ECS adapter converts these values to and from LogicFixedVec3.
struct AIFixedPosition final
{
    int64_t xRaw = 0;
    int64_t yRaw = 0;
    int64_t zRaw = 0;
    constexpr bool operator==(const AIFixedPosition&) const noexcept = default;
};

struct AIStateRequestId final
{
    uint64_t issuedTick = 0;
    uint32_t sequence = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return sequence != 0;
    }
    constexpr auto operator<=>(const AIStateRequestId&) const noexcept = default;
};

enum class AIStateCommandKind : uint8_t
{
    FaceObject,
    FacePosition,
};

struct AIStateCommand final
{
    AIStateCommandKind kind = AIStateCommandKind::FaceObject;
    ObjectId subject = INVALID_OBJECT_ID;
    AIStateRequestId request;
    ObjectId targetObject = INVALID_OBJECT_ID;
    AIFixedPosition targetPosition;
    bool canTurnInPlace = false;
    uint64_t confirmedTick = 0;
    AIAsyncOrderIdentity orderIdentity;

    constexpr bool operator==(const AIStateCommand&) const noexcept = default;
};

struct AIStateCommandBuffer final
{
    static constexpr size_t Capacity = 4;

    // This can stay stack-bound per executor call or be shared by a bounded
    // batch. Subject and request identity make either use unambiguous.
    container::Array<AIStateCommand, Capacity> commands{};
    size_t count = 0;
    bool overflowed = false;

    [[nodiscard]] bool push(const AIStateCommand& command) noexcept
    {
        if (count >= commands.size())
        {
            overflowed = true;
            return false;
        }
        commands[count++] = command;
        return true;
    }

    void clear() noexcept
    {
        count = 0;
        overflowed = false;
    }
};

} // namespace engine::ai

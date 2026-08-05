#pragma once

#include "core/ecs/ObjectId.h"

namespace engine {

// Local presentation state for a script-owned camera lock. This is not
// simulation authority: it is consumed by the camera phase independently of
// world ticks, while the target ObjectId is resolved against the latest
// confirmed world state.
class GameSessionScriptCameraState final {
public:
    enum class LockMode : uint8_t {
        Follow,
        Tether,
    };

    void reset() noexcept { *this = {}; }

    void follow(ObjectId object, bool snap) noexcept {
        m_object = object;
        m_snapPending = snap;
        m_mode = LockMode::Follow;
        m_tetherPlay = 0.0f;
        m_tetherPartitionCellSize = 100.0f;
    }

    void tether(
        ObjectId object, bool snap, float play,
        float partitionCellSize) noexcept {
        m_object = object;
        m_snapPending = snap;
        m_mode = LockMode::Tether;
        m_tetherPlay = play;
        m_tetherPartitionCellSize = partitionCellSize;
    }

    void clearLock() noexcept {
        m_object = INVALID_OBJECT_ID;
        m_snapPending = false;
        m_mode = LockMode::Follow;
        m_tetherPlay = 0.0f;
        m_tetherPartitionCellSize = 100.0f;
    }

    [[nodiscard]] ObjectId object() const noexcept { return m_object; }
    [[nodiscard]] bool snapPending() const noexcept { return m_snapPending; }
    void consumeSnap() noexcept { m_snapPending = false; }
    [[nodiscard]] bool isTether() const noexcept {
        return m_mode == LockMode::Tether;
    }
    [[nodiscard]] float tetherPlay() const noexcept { return m_tetherPlay; }
    [[nodiscard]] float tetherPartitionCellSize() const noexcept {
        return m_tetherPartitionCellSize;
    }

    void armTimeFreeze() noexcept { m_timeFreezeArmed = true; }
    void disarmTimeFreeze() noexcept { m_timeFreezeArmed = false; }
    [[nodiscard]] bool timeFreezeArmed() const noexcept {
        return m_timeFreezeArmed;
    }

private:
    ObjectId m_object = INVALID_OBJECT_ID;
    bool m_snapPending = false;
    LockMode m_mode = LockMode::Follow;
    float m_tetherPlay = 0.0f;
    float m_tetherPartitionCellSize = 100.0f;
    bool m_timeFreezeArmed = false;
};

} // namespace engine

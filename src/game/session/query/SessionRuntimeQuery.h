#pragma once

#include "game/base/GameSettings.h"
#include "game/scenario/runtime/MissionState.h"
#include "game/player/MatchSetup.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "core/ecs/ObjectId.h"

#include <optional>

namespace engine {

class GameSessionRuntimeQueryPort final {
public:
    explicit GameSessionRuntimeQueryPort(
        const GameStartInfo& startInfo,
        const std::optional<ResolvedMatchSetup>& resolvedMatchSetup,
        const scenario::MissionState& mission,
        const ObjectLifecycle& objects) noexcept
        : m_startInfo(&startInfo),
          m_resolvedMatchSetup(&resolvedMatchSetup),
          m_mission(&mission),
          m_objects(&objects) {}

    [[nodiscard]] const GameStartInfo& startInfo() const noexcept {
        return *m_startInfo;
    }
    [[nodiscard]] std::optional<scenario::MissionOutcome>
    missionOutcome() const noexcept {
        return m_mission->outcome();
    }
    [[nodiscard]] bool isLiveObject(ObjectId object) const noexcept {
        return object && m_objects->entityFromId(object).has_value() &&
            !m_objects->isPendingDestroy(object);
    }

    [[nodiscard]] std::optional<ResolvedMatchSetup>
    resolvedMatchSetup() const {
        return *m_resolvedMatchSetup;
    }

private:
    const GameStartInfo* m_startInfo = nullptr;
    const std::optional<ResolvedMatchSetup>* m_resolvedMatchSetup = nullptr;
    const scenario::MissionState* m_mission = nullptr;
    const ObjectLifecycle* m_objects = nullptr;
};

} // namespace engine

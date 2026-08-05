#include "core/container/container_types.h"
#include "core/container/hash_containers.h"
#include "core/debug/td_assert.h"
#include "GameRenderExtraction.h"

#include "presentation/render/TrackMarksPerformanceSettings.h"
#include "presentation/render/HeatVisionVisualSettings.h"
#include "game/render/ClientTerrainObjectStore.h"
#include "game/render/LocalPlacementPreviewPresentation.h"
#include "presentation/render/ProjectileStreamJoinPresentation.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/simulation/movement/ObjectFloat.h"
#include "game/object/simulation/structure/ObjectBridge.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/world/ObjectDynamicShroud.h"
#include "game/object/simulation/world/ObjectRadiusDecal.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/status/ObjectEmpUpdate.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/combat/ObjectNeutronMissileSlowDeath.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/lifecycle/ObjectStructureDestruction.h"
#include "game/object/simulation/status/ObjectStealth.h"
#include "game/object/simulation/combat/ObjectStickyBomb.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"
#include "game/object/simulation/structure/ObjectTechBuilding.h"
#include "game/object/simulation/combat/ObjectWeaponBonusUpdate.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/player/FactionTemplate.h"
#include "game/script/runtime/ScriptProgram.h"
#include "game/terrain/MapHeightfieldLoader.h"
#include "game/terrain/TerrainLogic.h"
#include "presentation/render/SupportDrawPresentation.h"
#include "core/config/GlobalData.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include "GameRenderExtractionDetail.h"

namespace engine::render_extraction_detail {

[[nodiscard]] bool hasObjectKind(const ObjectKindOfComponent* kinds,
                                 game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] bool equalsInsensitive(container::StringView left,
                                     container::StringView right) noexcept {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}


[[nodiscard]] uint64_t radarEventIdentity(
    uint64_t producer, uint64_t createTick, int32_t eventType) noexcept {
    uint64_t value = 14695981039346656037ull;
    const auto mix = [&value](uint64_t part) noexcept {
        value ^= part;
        value *= 1099511628211ull;
    };
    mix(producer);
    mix(createTick);
    mix(static_cast<uint32_t>(eventType));
    return value == 0 ? 1 : value;
}

struct TacticalRadarDedupBucketKey final {
    int32_t eventType = 0;
    double cellX = 0.0;
    double cellY = 0.0;

    [[nodiscard]] bool operator<(
        const TacticalRadarDedupBucketKey& other) const noexcept {
        if (eventType != other.eventType) return eventType < other.eventType;
        if (cellX != other.cellX) return cellX < other.cellX;
        return cellY < other.cellY;
    }
};

void appendAcceptedGameplayRadarEvents(
    container::Vector<render::TacticalRadarEventRenderSnapshot>& candidates,
    container::Vector<render::TacticalRadarEventRenderSnapshot>& destination,
    uint64_t simulationFrame, uint32_t logicFramesPerSecond) {
    std::sort(candidates.begin(), candidates.end(),
        [](const render::TacticalRadarEventRenderSnapshot& left,
           const render::TacticalRadarEventRenderSnapshot& right) {
            if (left.createTick != right.createTick)
                return left.createTick < right.createTick;
            if (left.eventType != right.eventType)
                return left.eventType < right.eventType;
            return left.sourceObjectId < right.sourceObjectId;
        });

    constexpr float kDuplicateDistance = 250.0f;
    constexpr float kDuplicateDistanceSquared =
        kDuplicateDistance * kDuplicateDistance;
    const uint64_t duplicateWindow =
        static_cast<uint64_t>(logicFramesPerSecond) * 10u;
    using Bucket = container::Deque<
        const render::TacticalRadarEventRenderSnapshot*>;
    container::TreeMap<TacticalRadarDedupBucketKey, Bucket> buckets;

    for (const render::TacticalRadarEventRenderSnapshot& candidate :
         candidates) {
        const float candidateX = candidate.worldPosition.x();
        const float candidateY = candidate.worldPosition.y();
        const bool hasFinitePosition =
            std::isfinite(candidateX) && std::isfinite(candidateY);
        const double cellX = hasFinitePosition
            ? std::floor(static_cast<double>(candidateX) /
                         static_cast<double>(kDuplicateDistance))
            : 0.0;
        const double cellY = hasFinitePosition
            ? std::floor(static_cast<double>(candidateY) /
                         static_cast<double>(kDuplicateDistance))
            : 0.0;

        bool duplicate = false;
        if (hasFinitePosition) {
            for (int32_t offsetX = -1; offsetX <= 1 && !duplicate;
                 ++offsetX) {
                for (int32_t offsetY = -1; offsetY <= 1 && !duplicate;
                     ++offsetY) {
                    const TacticalRadarDedupBucketKey key{
                        candidate.eventType,
                        cellX + static_cast<double>(offsetX),
                        cellY + static_cast<double>(offsetY)};
                    auto found = buckets.find(key);
                    if (found == buckets.end()) continue;

                    Bucket& bucket = found->second;
                    while (!bucket.empty()) {
                        const auto& prior = *bucket.front();
                        if (candidate.createTick < prior.createTick ||
                            candidate.createTick - prior.createTick <
                                duplicateWindow) {
                            break;
                        }
                        bucket.pop_front();
                    }
                    duplicate = std::any_of(
                        bucket.begin(), bucket.end(),
                        [&candidate](const auto* prior) {
                            if (candidate.createTick < prior->createTick)
                                return false;
                            const float dx = prior->worldPosition.x() -
                                candidate.worldPosition.x();
                            const float dy = prior->worldPosition.y() -
                                candidate.worldPosition.y();
                            return dx * dx + dy * dy <=
                                kDuplicateDistanceSquared;
                        });
                }
            }
        }
        if (duplicate) continue;

        if (hasFinitePosition) {
            buckets[{candidate.eventType, cellX, cellY}].push_back(
                &candidate);
        }
        if (simulationFrame <= candidate.dieTick)
            destination.push_back(candidate);
    }
}

void updateGameplayRadarHistoryAndAppend(
    container::Vector<render::TacticalRadarEventRenderSnapshot>& history,
    uint64_t& historyEpoch,
    uint64_t presentationEpoch,
    container::Vector<render::TacticalRadarEventRenderSnapshot> candidates,
    container::Vector<render::TacticalRadarEventRenderSnapshot>& destination,
    uint64_t simulationFrame,
    uint32_t logicFramesPerSecond) {
    if (historyEpoch != presentationEpoch) {
        history.clear();
        historyEpoch = presentationEpoch;
    }
    const uint64_t duplicateWindow =
        static_cast<uint64_t>(logicFramesPerSecond) * 10u;
    std::erase_if(history, [simulationFrame, duplicateWindow](const auto& value) {
        return simulationFrame >= value.createTick &&
            simulationFrame - value.createTick > duplicateWindow;
    });
    for (auto& candidate : candidates) {
        const auto existing = std::find_if(
            history.begin(), history.end(), [&candidate](const auto& value) {
                return value.eventIdentity == candidate.eventIdentity;
            });
        if (existing == history.end()) {
            history.push_back(std::move(candidate));
        } else {
            *existing = std::move(candidate);
        }
    }
    appendAcceptedGameplayRadarEvents(
        history, destination, simulationFrame, logicFramesPerSecond);
}

[[nodiscard]] render::RenderEntityId renderInstanceId(
    uint32_t objectId, size_t channelIndex) noexcept {
    // ObjectId is 32-bit and zero is invalid. Preserve the historical id for
    // channel zero so existing object-level consumers keep working, while
    // every additional Draw module receives a collision-free 64-bit key.
    TD_ASSERT(channelIndex <= kClientTerrainMaximumChannelIndex);
    return channelIndex == 0
        ? static_cast<render::RenderEntityId>(objectId)
        : (static_cast<render::RenderEntityId>(channelIndex) << 32u) |
              static_cast<render::RenderEntityId>(objectId);
}

[[nodiscard]] render::RenderEntityId clientTerrainObjectId(
    uint32_t clientObjectId) noexcept {
    return 0x8000000000000000ull |
        static_cast<render::RenderEntityId>(clientObjectId);
}

[[nodiscard]] render::RenderEntityId clientTerrainInstanceId(
    uint32_t clientObjectId, uint32_t channelIndex) noexcept {
    TD_ASSERT(channelIndex <= kClientTerrainMaximumChannelIndex);
    return clientTerrainObjectId(clientObjectId) |
        (static_cast<render::RenderEntityId>(channelIndex) << 32u);
}

[[nodiscard]] uint64_t modelParticleEmitterIdentity(
    render::RenderEntityId instanceId, uint32_t phaseIdentity,
    size_t declarationOrdinal) noexcept {
    uint64_t key = instanceId;
    key ^= (static_cast<uint64_t>(phaseIdentity) + 0x9E3779B97F4A7C15ull) +
        (key << 6u) + (key >> 2u);
    key ^= (static_cast<uint64_t>(declarationOrdinal) +
            0x517CC1B727220A95ull) +
        (key << 6u) + (key >> 2u);
    return key != 0 ? key : 1u;
}

render::RenderAnimationMode toRenderAnimationMode(game::ModelAnimationMode mode) {
    using Source = game::ModelAnimationMode;
    using Target = render::RenderAnimationMode;
    switch (mode) {
    case Source::Manual: return Target::Manual;
    case Source::Loop: return Target::Loop;
    case Source::Once: return Target::Once;
    case Source::LoopPingPong: return Target::LoopPingPong;
    case Source::LoopBackwards: return Target::LoopBackwards;
    case Source::OnceBackwards: return Target::OnceBackwards;
    }
    return Target::Loop;
}

render::RenderAnimationStartKind toRenderAnimationStartKind(
    VisualAnimationStartKind kind) noexcept {
    using Source = VisualAnimationStartKind;
    using Target = render::RenderAnimationStartKind;
    switch (kind) {
    case Source::FirstFrame: return Target::FirstFrame;
    case Source::LastFrame: return Target::LastFrame;
    case Source::RandomFrame: return Target::RandomFrame;
    case Source::MaintainFraction: return Target::MaintainFraction;
    case Source::Default: return Target::Default;
    }
    return Target::Default;
}


} // namespace engine::render_extraction_detail

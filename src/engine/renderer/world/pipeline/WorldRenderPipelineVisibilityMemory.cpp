#include "engine/renderer/world/pipeline/WorldRenderPipeline.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace engine::render {
namespace {

[[nodiscard]] uint64_t visibilityDeadline(
    uint64_t start, uint32_t duration) noexcept {
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    return start > maximum - duration ? maximum : start + duration;
}


void prepareStaticGhost(RenderEntitySnapshot& value) {
    value.hiddenByLocalVisibility = false;
    value.visual.hidden = false;
    value.visual.animationPaused = true;
    value.visual.receivesLocalVisibility = true;
    value.visual.particleSystemBones.clear();
    value.visual.weaponImpulses.clear();
    value.animationCompletionTarget.reset();
    value.animationFinalTarget.reset();
    value.animationCompletionFeedbackEnabled = false;
    value.weaponLaunchBones = {};
    value.weaponLaunchBoneSequenceOrdinals = {};
    value.shadow = {};
    // The retail ghost clone disables texture animation and every MUZZLEFX
    // subobject. Texture mapper state is not mutable in this renderer; hide
    // the corresponding detached subobjects explicitly.
    for (RenderSubObjectVisibility& subObject :
         value.visual.subObjectVisibility) {
        container::String upper = subObject.name;
        std::transform(upper.begin(), upper.end(), upper.begin(),
            [](unsigned char character) {
                return static_cast<char>(std::toupper(character));
            });
        if (upper.find("MUZZLEFX") != container::String::npos) {
            subObject.visible = false;
        }
    }
}

} // namespace

void WorldRenderPipeline::applyLocalVisibilityObjectMemory(
    WorldRenderSnapshot& snapshot) {
    const bool epochChanged =
        m_localVisibilityMemoryEpoch != snapshot.presentationEpoch;
    const bool frameRewound = !epochChanged && !m_localVisibilityMemory.empty() &&
        snapshot.simulationFrame < m_localVisibilityMemoryFrame;
    if (epochChanged || frameRewound || !snapshot.localVisibility.enabled) {
        m_localVisibilityMemory.clear();
        m_localVisibilityMemoryEpoch = snapshot.presentationEpoch;
    }
    m_localVisibilityMemoryFrame = snapshot.simulationFrame;
    // Fog memory can be disabled independently from the active map border.
    // Keep processing HardHidden entries so friendly/off-map objects cannot
    // bypass extraction merely because the session has no shroud texture.
    if (!snapshot.localVisibility.enabled &&
        !snapshot.localVisibility.hasPlayableBounds()) {
        return;
    }

    const size_t visibilityBucketsBefore =
        m_currentVisibilityEntities.bucket_count();
    m_currentVisibilityEntities.clear();
    m_currentVisibilityEntities.reserve(snapshot.entities.size());
    if (m_currentVisibilityEntities.bucket_count() >
            visibilityBucketsBefore &&
        m_containerCapacityGrowths !=
            std::numeric_limits<uint32_t>::max()) {
        ++m_containerCapacityGrowths;
    }
    SharedSnapshotVector<RenderEntitySnapshot> patchedEntities;
    for (size_t entityIndex = 0; entityIndex < snapshot.entities.size();
         ++entityIndex) {
        const RenderEntitySnapshot& entity = snapshot.entities[entityIndex];
        m_currentVisibilityEntities.insert(entity.id);
        if (entity.localVisibilityMemoryPolicy ==
            RenderLocalVisibilityMemoryPolicy::HardHidden) {
            m_localVisibilityMemory.erase(entity.id);
            RenderEntitySnapshot hidden = entity;
            hidden.visual.hidden = true;
            patchedEntities.appendHandle(
                SharedSnapshotVector<RenderEntitySnapshot>::ElementHandle::own(
                    std::move(hidden)));
            continue;
        }
        if (entity.localVisibilityMemoryPolicy ==
            RenderLocalVisibilityMemoryPolicy::None) {
            m_localVisibilityMemory.erase(entity.id);
            patchedEntities.appendSharedSlice(
                snapshot.entities, entityIndex, 1u);
            continue;
        }

        auto memory = m_localVisibilityMemory.find(entity.id);
        if (!entity.hiddenByLocalVisibility) {
            // A hidden Draw module was not visible enough for RefCode's
            // GhostObject::snapShot either. Do not replace a useful prior
            // memory with a deliberately invisible authored state.
            if (!entity.visual.hidden) {
                LocalVisibilityMemoryRecord& record =
                    m_localVisibilityMemory[entity.id];
                record.snapshot = snapshot.entities.elementHandle(entityIndex);
                record.lastClearFrame = snapshot.simulationFrame;
                record.persistenceTicks =
                    entity.localVisibilityPersistenceTicks;
                record.policy = entity.localVisibilityMemoryPolicy;
            }
            patchedEntities.appendSharedSlice(
                snapshot.entities, entityIndex, 1u);
            continue;
        }

        if (memory == m_localVisibilityMemory.end() ||
            memory->second.policy != entity.localVisibilityMemoryPolicy) {
            RenderEntitySnapshot hidden = entity;
            hidden.visual.hidden = true;
            patchedEntities.appendHandle(
                SharedSnapshotVector<RenderEntitySnapshot>::ElementHandle::own(
                    std::move(hidden)));
            continue;
        }

        LocalVisibilityMemoryRecord& record = memory->second;
        if (entity.localVisibilityMemoryPolicy ==
            RenderLocalVisibilityMemoryPolicy::StaticGhost) {
            if (entity.localVisibilityState ==
                LocalVisibilityRenderCellState::Explored) {
                RenderEntitySnapshot ghost = *record.snapshot;
                ghost.localVisibilityState =
                    LocalVisibilityRenderCellState::Explored;
                prepareStaticGhost(ghost);
                patchedEntities.appendHandle(
                    SharedSnapshotVector<RenderEntitySnapshot>::ElementHandle::own(
                        std::move(ghost)));
            } else {
                RenderEntitySnapshot hidden = entity;
                hidden.visual.hidden = true;
                m_localVisibilityMemory.erase(memory);
                patchedEntities.appendHandle(
                    SharedSnapshotVector<RenderEntitySnapshot>::ElementHandle::own(
                        std::move(hidden)));
            }
            continue;
        }

        // The ordinary path remains a live drawable while inside its grace
        // period. This intentionally follows the current transform/animation
        // rather than freezing the last clear frame.
        record.persistenceTicks = std::max(
            record.persistenceTicks, entity.localVisibilityPersistenceTicks);
        if (snapshot.simulationFrame < visibilityDeadline(
                record.lastClearFrame, record.persistenceTicks)) {
            RenderEntitySnapshot retained = entity;
            retained.hiddenByLocalVisibility = false;
            auto retainedHandle =
                SharedSnapshotVector<RenderEntitySnapshot>::ElementHandle::own(
                    std::move(retained));
            record.snapshot = retainedHandle;
            patchedEntities.appendHandle(retainedHandle);
        } else {
            RenderEntitySnapshot hidden = entity;
            hidden.visual.hidden = true;
            m_localVisibilityMemory.erase(memory);
            patchedEntities.appendHandle(
                SharedSnapshotVector<RenderEntitySnapshot>::ElementHandle::own(
                    std::move(hidden)));
        }
    }

    // A destroyed object no longer appears in extraction. Static building
    // ghosts remain in explored territory until clear sight confirms the
    // absence; timed drawables may finish only their already-established
    // grace period. Iterate the memory map after current entities so an
    // appended frozen snapshot cannot be mistaken for current input.
    for (auto memory = m_localVisibilityMemory.begin();
         memory != m_localVisibilityMemory.end();) {
        if (m_currentVisibilityEntities.contains(memory->first)) {
            ++memory;
            continue;
        }

        LocalVisibilityMemoryRecord& record = memory->second;
        if (!record.snapshot) {
            memory = m_localVisibilityMemory.erase(memory);
            continue;
        }
        const RenderEntitySnapshot& remembered = *record.snapshot;
        const LocalVisibilityRenderCellState currentState =
            snapshot.localVisibility.worldStateSphere(
                remembered.transform.position +
                    remembered.cullingCenterOffset,
                remembered.boundingRadius);
        bool retain = false;
        RenderEntitySnapshot retained = remembered;
        if (record.policy == RenderLocalVisibilityMemoryPolicy::StaticGhost) {
            retain = currentState == LocalVisibilityRenderCellState::Explored;
            if (retain) {
                retained.localVisibilityState = currentState;
                prepareStaticGhost(retained);
            }
        } else if (currentState != LocalVisibilityRenderCellState::Visible) {
            retain = snapshot.simulationFrame < visibilityDeadline(
                record.lastClearFrame, record.persistenceTicks);
            if (retain) retained.hiddenByLocalVisibility = false;
        }

        if (retain) {
            patchedEntities.appendHandle(
                SharedSnapshotVector<RenderEntitySnapshot>::ElementHandle::own(
                    std::move(retained)));
            ++memory;
        } else {
            memory = m_localVisibilityMemory.erase(memory);
        }
    }
    patchedEntities.seal();
    snapshot.entities = std::move(patchedEntities);
}

} // namespace engine::render

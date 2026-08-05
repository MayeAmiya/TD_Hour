#include "game/object/simulation/economy/ObjectEconomy.h"

#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace engine {

namespace {

class ObjectEconomyDigestWriter final {
public:
    void boolean(bool value) noexcept { u8(value ? uint8_t{1} : uint8_t{0}); }

    void u8(uint8_t value) noexcept {
        m_value ^= value;
        m_value *= FnvPrime;
    }

    void u32(uint32_t value) noexcept {
        for (uint32_t shift = 0; shift < 32; shift += 8)
            u8(static_cast<uint8_t>((value >> shift) & uint32_t{0xff}));
    }

    void i32(int32_t value) noexcept { u32(static_cast<uint32_t>(value)); }

    void u64(uint64_t value) noexcept {
        for (uint32_t shift = 0; shift < 64; shift += 8)
            u8(static_cast<uint8_t>((value >> shift) & uint64_t{0xff}));
    }

    void count(size_t value) noexcept { u64(static_cast<uint64_t>(value)); }

    void object(ObjectId value) noexcept { u32(value.value); }

    [[nodiscard]] uint64_t finish() const noexcept { return m_value; }

private:
    static constexpr uint64_t FnvOffsetBasis = 14695981039346656037ull;
    static constexpr uint64_t FnvPrime = 1099511628211ull;

    uint64_t m_value = FnvOffsetBasis;
};

[[nodiscard]] bool dockShapeValid(
    const ObjectSupplyDockRuntime& dock) noexcept {
    return dock.approachOwners.size() == dock.approachReached.size() &&
        dock.approachOwners.size() == dock.approachPositionsLocal.size() &&
        dock.approachOwners.size() == dock.approachPositionValid.size();
}

void encodeFixedPosition(ObjectEconomyDigestWriter& writer,
                         const LogicFixedVec3& value) noexcept {
    writer.u64(static_cast<uint64_t>(value.x.raw()));
    writer.u64(static_cast<uint64_t>(value.y.raw()));
    writer.u64(static_cast<uint64_t>(value.z.raw()));
}

void cacheDockPristineBones(
    ObjectSupplyDockRuntime& runtime,
    const game::W3dPristineBoneCatalog* catalog,
    container::StringView archetypeName, size_t visualRuleIndex,
    int32_t authoredApproachCount) {
    runtime.approachPositionsLocal.resize(runtime.approachOwners.size());
    runtime.approachPositionValid.assign(runtime.approachOwners.size(), false);
    if (!catalog || !catalog->isLoaded() || archetypeName.empty()) return;

    const auto cachePoint = [&](container::StringView name,
                                LogicFixedVec3& position) {
        const auto bone = catalog->find(archetypeName, visualRuleIndex, name);
        if (!bone) return false;
        position = {
            bone->translation.x,
            bone->translation.y,
            bone->translation.z,
        };
        return true;
    };
    runtime.enterPositionValid =
        cachePoint("DockStart", runtime.enterPositionLocal);
    runtime.actionPositionValid =
        cachePoint("DockAction", runtime.actionPositionLocal);
    runtime.exitPositionValid =
        cachePoint("DockEnd", runtime.exitPositionLocal);

    // NumberApproachPositions == -1 denotes RefCode's boneless dynamic dock.
    // It deliberately uses findPositionAround rather than DockWaiting bones.
    if (authoredApproachCount <= 0) return;
    const size_t count = std::min(
        runtime.approachPositionsLocal.size(),
        static_cast<size_t>(authoredApproachCount));
    for (size_t index = 0; index < count && index < 99; ++index) {
        const uint32_t ordinal = static_cast<uint32_t>(index + 1);
        container::String boneName{"DockWaiting"};
        boneName.push_back(static_cast<char>('0' + ordinal / 10));
        boneName.push_back(static_cast<char>('0' + ordinal % 10));
        runtime.approachPositionValid[index] = cachePoint(
            boneName, runtime.approachPositionsLocal[index]);
    }
}

template <typename ComponentOrSnapshot>
[[nodiscard]] bool economyShapeMatchesPlan(
    const ComponentOrSnapshot& value,
    const game::ObjectEconomyPlan& plan) noexcept {
    if (value.autoFindHealing.size() != plan.autoFindHealing.size() ||
        value.repairDocks.size() != plan.repairDocks.size() ||
        value.supplyTrucks.size() != plan.supplyTrucks.size() ||
        value.hackInternet.size() != plan.hackInternet.size() ||
        value.supplyCenterDocks.size() != plan.supplyCenterDocks.size() ||
        value.supplyWarehouseDocks.size() !=
            plan.supplyWarehouseDocks.size()) {
        return false;
    }
    return std::all_of(
               value.repairDocks.begin(), value.repairDocks.end(),
               [](const ObjectRepairDockRuntime& runtime) {
                   return dockShapeValid(runtime.dock) &&
                       runtime.approachOwnerPlayers.size() ==
                           runtime.dock.approachOwners.size();
               }) &&
        std::all_of(
               value.supplyCenterDocks.begin(),
               value.supplyCenterDocks.end(),
               [](const ObjectSupplyCenterDockRuntime& runtime) {
                   return dockShapeValid(runtime.dock);
               }) &&
        std::all_of(
               value.supplyWarehouseDocks.begin(),
               value.supplyWarehouseDocks.end(),
               [](const ObjectSupplyWarehouseDockRuntime& runtime) {
                   return dockShapeValid(runtime.dock);
               });
}

void encode(ObjectEconomyDigestWriter& writer,
            const ObjectAutoFindHealingRuntime& value) noexcept {
    writer.u64(value.nextScanTick);
    writer.u64(value.observedExternalOrderRevision);
    writer.u32(value.nextCommandSequence);
    writer.object(value.targetDock);
}

void encode(ObjectEconomyDigestWriter& writer,
            const ObjectSupplyDockRuntime& value) noexcept;

void encode(ObjectEconomyDigestWriter& writer,
            const ObjectRepairDockRuntime& value) noexcept {
    encode(writer, value.dock);
    writer.count(value.approachOwnerPlayers.size());
    for (PlayerId owner : value.approachOwnerPlayers)
        writer.u8(owner.value);
    writer.object(value.repairSubject);
    writer.object(value.pendingDrone);
    writer.u8(value.reservationDockOwner.value);
    writer.u8(value.dockOwnerAtAdmission.value);
    writer.u8(value.dockerOwnerAtAdmission.value);
    writer.u64(static_cast<uint64_t>(value.healthToAddPerTick.raw()));
    writer.u64(value.pendingActionTick);
    writer.u64(value.revision);
    writer.boolean(value.activeDockerAtActionPoint);
    writer.boolean(value.actionPending);
}

void encode(ObjectEconomyDigestWriter& writer,
            const ObjectSupplyTruckRuntime& value) noexcept {
    writer.u32(value.boxes);
    writer.u32(value.nextCommandSequence);
    writer.u64(value.nextActionTick);
    writer.u64(value.observedExternalOrderRevision);
    writer.object(value.targetDock);
    writer.object(value.preferredDock);
    writer.u32(value.targetDockModule);
    writer.i32(value.approachPosition);
    writer.boolean(value.targetIsCenter);
    writer.u8(static_cast<uint8_t>(value.state));
    writer.boolean(value.scriptIdleSuppressed);
    writer.boolean(value.externalIdleSuppressed);
    writer.boolean(value.regroupMoveIssued);
    writer.boolean(value.workerSupplyActive);
}

void encode(ObjectEconomyDigestWriter& writer,
            const ObjectHackInternetRuntime& value) noexcept {
    writer.u64(value.phaseEndTick);
    writer.u64(value.nextCashTick);
    writer.u64(value.observedExternalOrderRevision);
    writer.u64(value.revision);
    writer.object(value.internetCenter);
    writer.u8(static_cast<uint8_t>(value.phase));
    writer.boolean(value.autoStartedByContainment);
}

void encode(ObjectEconomyDigestWriter& writer,
             const ObjectSupplyDockRuntime& value) noexcept {
    writer.count(value.approachOwners.size());
    for (ObjectId owner : value.approachOwners) writer.object(owner);
    writer.count(value.approachReached.size());
    for (bool reached : value.approachReached) writer.boolean(reached);
    writer.count(value.approachPositionsLocal.size());
    for (const LogicFixedVec3& position : value.approachPositionsLocal)
        encodeFixedPosition(writer, position);
    writer.count(value.approachPositionValid.size());
    for (bool valid : value.approachPositionValid) writer.boolean(valid);
    encodeFixedPosition(writer, value.enterPositionLocal);
    encodeFixedPosition(writer, value.actionPositionLocal);
    encodeFixedPosition(writer, value.exitPositionLocal);
    writer.object(value.activeDocker);
    writer.boolean(value.activeDockerInside);
    writer.boolean(value.open);
    writer.boolean(value.enterPositionValid);
    writer.boolean(value.actionPositionValid);
    writer.boolean(value.exitPositionValid);
    writer.u64(value.revision);
}

void encode(ObjectEconomyDigestWriter& writer,
            const ObjectSupplyCenterDockRuntime& value) noexcept {
    encode(writer, value.dock);
    writer.u64(value.revision);
}

void encode(ObjectEconomyDigestWriter& writer,
            const ObjectSupplyWarehouseDockRuntime& value) noexcept {
    encode(writer, value.dock);
    writer.u32(value.boxesStored);
    writer.u64(value.revision);
}

template <typename Value, typename Encoder>
void encodeVector(ObjectEconomyDigestWriter& writer,
                  const container::Vector<Value>& values,
                  Encoder encoder) noexcept {
    writer.count(values.size());
    for (const Value& value : values) encoder(writer, value);
}

} // namespace

ObjectEconomySnapshotStatus captureSnapshot(
    const ObjectEconomyComponent& component,
    ObjectEconomyRuntimeSnapshot& outSnapshot) {
    if (!component.plan) return ObjectEconomySnapshotStatus::MissingPlan;
    if (!economyShapeMatchesPlan(component, *component.plan))
        return ObjectEconomySnapshotStatus::ShapeMismatch;

    ObjectEconomyRuntimeSnapshot candidate;
    candidate.autoFindHealing = component.autoFindHealing;
    candidate.repairDocks = component.repairDocks;
    candidate.supplyTrucks = component.supplyTrucks;
    candidate.hackInternet = component.hackInternet;
    candidate.supplyCenterDocks = component.supplyCenterDocks;
    candidate.supplyWarehouseDocks = component.supplyWarehouseDocks;
    outSnapshot = std::move(candidate);
    return ObjectEconomySnapshotStatus::Success;
}

ObjectEconomySnapshotStatus restoreSnapshot(
    ObjectEconomyComponent& component,
    const ObjectEconomyRuntimeSnapshot& snapshot) {
    if (!component.plan) return ObjectEconomySnapshotStatus::MissingPlan;
    if (snapshot.schemaVersion != ObjectEconomyRuntimeSnapshot::SchemaVersion)
        return ObjectEconomySnapshotStatus::SchemaMismatch;
    if (!economyShapeMatchesPlan(component, *component.plan) ||
        !economyShapeMatchesPlan(snapshot, *component.plan)) {
        return ObjectEconomySnapshotStatus::ShapeMismatch;
    }

    ObjectEconomyComponent candidate;
    candidate.plan = component.plan;
    candidate.autoFindHealing = snapshot.autoFindHealing;
    candidate.repairDocks = snapshot.repairDocks;
    candidate.supplyTrucks = snapshot.supplyTrucks;
    candidate.hackInternet = snapshot.hackInternet;
    candidate.supplyCenterDocks = snapshot.supplyCenterDocks;
    candidate.supplyWarehouseDocks = snapshot.supplyWarehouseDocks;
    component = std::move(candidate);
    return ObjectEconomySnapshotStatus::Success;
}

uint64_t stableDigest(
    const ObjectEconomyRuntimeSnapshot& snapshot) noexcept {
    ObjectEconomyDigestWriter writer;
    writer.u32(snapshot.schemaVersion);
    encodeVector(writer, snapshot.autoFindHealing,
                 [](ObjectEconomyDigestWriter& output,
                    const ObjectAutoFindHealingRuntime& value) noexcept {
                     encode(output, value);
                 });
    encodeVector(writer, snapshot.repairDocks,
                 [](ObjectEconomyDigestWriter& output,
                    const ObjectRepairDockRuntime& value) noexcept {
                     encode(output, value);
                 });
    encodeVector(writer, snapshot.supplyTrucks,
                 [](ObjectEconomyDigestWriter& output,
                    const ObjectSupplyTruckRuntime& value) noexcept {
                     encode(output, value);
                 });
    encodeVector(writer, snapshot.hackInternet,
                 [](ObjectEconomyDigestWriter& output,
                    const ObjectHackInternetRuntime& value) noexcept {
                     encode(output, value);
                 });
    encodeVector(writer, snapshot.supplyCenterDocks,
                 [](ObjectEconomyDigestWriter& output,
                    const ObjectSupplyCenterDockRuntime& value) noexcept {
                     encode(output, value);
                 });
    encodeVector(writer, snapshot.supplyWarehouseDocks,
                 [](ObjectEconomyDigestWriter& output,
                    const ObjectSupplyWarehouseDockRuntime& value) noexcept {
                     encode(output, value);
                 });
    return writer.finish();
}

bool ObjectEconomySystem::setDockOpen(
    ecs::registry& registry, ecs::entity entity,
    ObjectEconomyDockKind kind, uint32_t moduleIndex, bool open) const {
    ObjectEconomyComponent* economy =
        ecs::try_get<ObjectEconomyComponent>(registry, entity);
    if (!economy) return false;

    ObjectSupplyDockRuntime* dock = nullptr;
    uint64_t* ownerRevision = nullptr;
    switch (kind) {
    case ObjectEconomyDockKind::Repair:
        if (moduleIndex < economy->repairDocks.size()) {
            dock = &economy->repairDocks[moduleIndex].dock;
            ownerRevision = &economy->repairDocks[moduleIndex].revision;
        }
        break;
    case ObjectEconomyDockKind::SupplyCenter:
        if (moduleIndex < economy->supplyCenterDocks.size()) {
            dock = &economy->supplyCenterDocks[moduleIndex].dock;
            ownerRevision = &economy->supplyCenterDocks[moduleIndex].revision;
        }
        break;
    case ObjectEconomyDockKind::SupplyWarehouse:
        if (moduleIndex < economy->supplyWarehouseDocks.size()) {
            dock = &economy->supplyWarehouseDocks[moduleIndex].dock;
            ownerRevision = &economy->supplyWarehouseDocks[moduleIndex].revision;
        }
        break;
    }
    if (!dock || dock->open == open) return false;
    dock->open = open;
    ++dock->revision;
    if (ownerRevision) ++*ownerRevision;
    return true;
}

void ObjectEconomySystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot* content,
    const ObjectSimulationRules& rules, uint64_t confirmedTick) const {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!type || !type->archetype || !type->archetype->economyPlan) return;

    ObjectEconomyComponent component;
    component.plan = type->archetype->economyPlan;
    component.autoFindHealing.resize(component.plan->autoFindHealing.size());
    component.repairDocks.resize(component.plan->repairDocks.size());
    component.supplyTrucks.resize(component.plan->supplyTrucks.size());
    for (size_t index = 0; index < component.supplyTrucks.size(); ++index) {
        component.supplyTrucks[index].workerSupplyActive =
            !component.plan->supplyTrucks[index].workerMode;
    }
    component.hackInternet.resize(component.plan->hackInternet.size());
    component.supplyCenterDocks.resize(component.plan->supplyCenterDocks.size());
    component.supplyWarehouseDocks.resize(
        component.plan->supplyWarehouseDocks.size());
    const game::W3dPristineBoneCatalog* pristineBones = content
        ? content->pristineBoneCatalog() : nullptr;
    const RenderModelComponent* visual =
        ecs::try_get<RenderModelComponent>(registry, entity);
    const game::ThingTemplate& templateData = type->archetype->templateData;
    const size_t visualRuleIndex = !templateData.modelConditionVisuals.empty()
        ? game::selectModelConditionVisualRuleIndex(
              templateData, visual ? visual->modelConditionFlags
                                   : game::ModelConditionMask{})
        : 0;
    for (size_t index = 0; index < component.autoFindHealing.size(); ++index) {
        component.autoFindHealing[index].nextScanTick = confirmedTick;
    }
    for (size_t index = 0; index < component.repairDocks.size(); ++index) {
        const game::ObjectRepairDockRule& rule =
            component.plan->repairDocks[index];
        const int32_t approachCount = rule.dock.numberApproachPositions;
        component.repairDocks[index].dock.approachOwners.resize(
            approachCount < 0 ? 10u : static_cast<size_t>(approachCount),
            INVALID_OBJECT_ID);
        component.repairDocks[index].dock.approachReached.resize(
            component.repairDocks[index].dock.approachOwners.size(), false);
        component.repairDocks[index].approachOwnerPlayers.resize(
            component.repairDocks[index].dock.approachOwners.size(),
            INVALID_PLAYER_ID);
        cacheDockPristineBones(
            component.repairDocks[index].dock, pristineBones,
            type->archetype->name, visualRuleIndex, approachCount);
    }
    for (size_t index = 0; index < component.supplyWarehouseDocks.size();
         ++index) {
        const int32_t approachCount = component.plan->supplyWarehouseDocks[index]
            .dock.numberApproachPositions;
        component.supplyWarehouseDocks[index].dock.approachOwners.resize(
            approachCount < 0 ? 10u : static_cast<size_t>(approachCount),
            INVALID_OBJECT_ID);
        component.supplyWarehouseDocks[index].dock.approachReached.resize(
            component.supplyWarehouseDocks[index].dock.approachOwners.size(),
            false);
        cacheDockPristineBones(
            component.supplyWarehouseDocks[index].dock, pristineBones,
            type->archetype->name, visualRuleIndex, approachCount);
        component.supplyWarehouseDocks[index].boxesStored =
            component.plan->supplyWarehouseDocks[index].startingBoxes;
    }
    for (size_t index = 0; index < component.supplyCenterDocks.size(); ++index) {
        const int32_t approachCount = component.plan->supplyCenterDocks[index]
            .dock.numberApproachPositions;
        component.supplyCenterDocks[index].dock.approachOwners.resize(
            approachCount < 0 ? 10u : static_cast<size_t>(approachCount),
            INVALID_OBJECT_ID);
        component.supplyCenterDocks[index].dock.approachReached.resize(
            component.supplyCenterDocks[index].dock.approachOwners.size(),
            false);
        cacheDockPristineBones(
            component.supplyCenterDocks[index].dock, pristineBones,
            type->archetype->name, visualRuleIndex, approachCount);
    }

    if (ObjectEconomyComponent* existing =
            ecs::try_get<ObjectEconomyComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectEconomyComponent>(registry, entity,
                                             std::move(component));
    }
}

} // namespace engine

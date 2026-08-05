#include "core/container/container_types.h"
#include "game/object/contracts/ObjectTeamRegistry.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace engine {

void ObjectTeamRegistry::reset() noexcept {
    m_teams.clear();
    m_defaultTeams.fill(INVALID_OBJECT_TEAM_ID);
    m_scenarioTeams.clear();
    m_teamByObject.clear();
}

bool ObjectTeamRegistry::initializePlayerDefaults(container::Span<const PlayerId> players) {
    if (!m_teams.empty() || !m_scenarioTeams.empty() || !m_teamByObject.empty()) return false;
    for (const PlayerId player : players) {
        if (!player.isValid() || player.value >= m_defaultTeams.size() || m_defaultTeams[player.value]) {
            reset();
            return false;
        }
        const std::optional<ObjectTeamId> team = appendTeam(
            ObjectTeamKind::PlayerDefault, player, INVALID_SCRIPT_TEAM_ID,
            "DefaultTeam_Player_" + std::to_string(player.value), true, 0, false, true);
        if (!team) {
            reset();
            return false;
        }
        m_defaultTeams[player.value] = *team;
    }
    return true;
}

void ObjectTeamRegistry::beginConfirmedTick(uint64_t confirmedTick) noexcept {
    for (ObjectTeamRecord& record : m_teams) {
        if (record.pendingInitialCreationPulse) {
            record.pendingInitialCreationPulse = false;
            record.hasCreationPulse = true;
            record.createdAtConfirmedTick = confirmedTick;
        }
        if (record.pendingProductionStartPulse) {
            record.pendingProductionStartPulse = false;
            record.hasProductionStartPulse = true;
            record.productionStartedAtConfirmedTick = confirmedTick;
            record.productionActionPulseCount =
                record.pendingProductionActionPulseCount;
            record.pendingProductionActionPulseCount = 0;
            record.productionActionWithoutTeamPulseCount =
                record.pendingProductionActionWithoutTeamPulseCount;
            record.pendingProductionActionWithoutTeamPulseCount = 0;
        }
    }
}

bool ObjectTeamRegistry::bindScenarioTeamAlias(ScriptTeamId definition, ObjectTeamId team) {
    if (!definition || !find(team)) return false;
    auto found = findScenarioTeam(definition);
    if (found != m_scenarioTeams.end()) {
        if (!found->instances.empty()) return false;
        found->instances.push_back(team);
        if (ObjectTeamRecord* record = mutableFind(team))
            record->productionPriority = found->productionPriority;
        return true;
    }
    const auto position = lowerBoundScenarioTeam(definition);
    m_scenarioTeams.insert(position, {.definition = definition, .instances = {team}});
    return true;
}

bool ObjectTeamRegistry::configureScenarioTeamProductionPolicy(
    ScriptTeamId definition, int32_t priority,
    int32_t successIncrease, int32_t failureDecrease) {
    if (!definition) return false;
    auto binding = findScenarioTeam(definition);
    if (binding == m_scenarioTeams.end()) {
        binding = m_scenarioTeams.insert(lowerBoundScenarioTeam(definition), {
            .definition = definition,
        });
    }
    binding->productionPriority = priority;
    binding->productionPrioritySuccessIncrease =
        std::max(0, successIncrease);
    binding->productionPriorityFailureDecrease =
        std::max(0, failureDecrease);
    binding->productionPolicyConfigured = true;
    for (const ObjectTeamId instance : binding->instances) {
        if (ObjectTeamRecord* record = mutableFind(instance)) {
            record->productionPriority = priority;
            ++record->policyRevision;
            if (record->policyRevision == 0) ++record->policyRevision;
        }
    }
    return true;
}

std::optional<ObjectTeamId> ObjectTeamRegistry::createScenarioTeamInstance(
    ScriptTeamId definition, container::String name, PlayerId owner, bool active,
    uint64_t createdAtConfirmedTick) {
    if (!definition || !owner.isValid() || name.empty()) return std::nullopt;

    const std::optional<ObjectTeamId> team = appendTeam(
        ObjectTeamKind::Scenario, owner, definition, std::move(name), active,
        createdAtConfirmedTick, active, false);
    if (!team) return std::nullopt;
    if (active && createdAtConfirmedTick != 0) {
        // A script-created active instance is normally materialized after the
        // current tick's Team-hook scan. Keep the edge observable at the next
        // confirmed boundary just like inactive->active transitions.
        mutableFind(*team)->pendingInitialCreationPulse = true;
    }

    auto binding = findScenarioTeam(definition);
    if (binding == m_scenarioTeams.end()) {
        binding = m_scenarioTeams.insert(lowerBoundScenarioTeam(definition), {
            .definition = definition,
        });
    }
    binding->instances.push_back(*team);
    if (ObjectTeamRecord* record = mutableFind(*team))
        record->productionPriority = binding->productionPriority;
    return team;
}

std::optional<ObjectTeamId> ObjectTeamRegistry::createScenarioTeam(
    ScriptTeamId definition, container::String name, PlayerId owner,
    uint64_t createdAtConfirmedTick) {
    return createScenarioTeamInstance(definition, std::move(name), owner, true,
                                      createdAtConfirmedTick);
}

std::optional<ObjectTeamId> ObjectTeamRegistry::defaultTeam(PlayerId owner) const noexcept {
    if (!owner.isValid() || owner.value >= m_defaultTeams.size() || !m_defaultTeams[owner.value]) {
        return std::nullopt;
    }
    return m_defaultTeams[owner.value];
}

std::optional<ObjectTeamId> ObjectTeamRegistry::scenarioTeam(ScriptTeamId definition) const noexcept {
    const auto found = findScenarioTeam(definition);
    if (found == m_scenarioTeams.end() || found->instances.empty()) return std::nullopt;
    return found->instances.front();
}

container::Span<const ObjectTeamId> ObjectTeamRegistry::scenarioTeamInstances(
    ScriptTeamId definition) const noexcept {
    const auto found = findScenarioTeam(definition);
    return found == m_scenarioTeams.end()
        ? container::Span<const ObjectTeamId>{}
        : container::Span<const ObjectTeamId>{found->instances};
}

std::optional<ObjectTeamId> ObjectTeamRegistry::teamOf(ObjectId object) const noexcept {
    const auto found = m_teamByObject.find(object);
    return found == m_teamByObject.end() ? std::nullopt : std::optional<ObjectTeamId>{found->second};
}

std::optional<PlayerId> ObjectTeamRegistry::teamOwner(ObjectTeamId team) const noexcept {
    const ObjectTeamRecord* record = find(team);
    return record ? std::optional<PlayerId>{record->owner} : std::nullopt;
}

std::optional<container::StringView> ObjectTeamRegistry::scriptState(
    ObjectTeamId team) const noexcept {
    const ObjectTeamRecord* record = find(team);
    return record
        ? std::optional<container::StringView>{record->scriptState}
        : std::nullopt;
}

ObjectId ObjectTeamRegistry::commonTarget(ObjectTeamId team) const noexcept {
    const ObjectTeamRecord* record = find(team);
    return record ? record->commonTarget : INVALID_OBJECT_ID;
}

const ObjectTeamRecord* ObjectTeamRegistry::find(ObjectTeamId team) const noexcept {
    if (!team || team.value > m_teams.size()) return nullptr;
    const ObjectTeamRecord& record = m_teams[team.value - 1];
    return record.id == team ? &record : nullptr;
}

bool ObjectTeamRegistry::isOwnedBy(ObjectTeamId team, PlayerId owner) const noexcept {
    const ObjectTeamRecord* record = find(team);
    return record && record->owner == owner;
}

bool ObjectTeamRegistry::isActive(ObjectTeamId team) const noexcept {
    const ObjectTeamRecord* record = find(team);
    return record && record->active;
}

bool ObjectTeamRegistry::activateAtStartup(ObjectTeamId team) noexcept {
    ObjectTeamRecord* record = mutableFind(team);
    if (!record) return false;
    if (record->active) return true;
    record->active = true;
    record->assemblyKind = ObjectTeamAssemblyKind::None;
    record->assemblyDeadlineTick = 0;
    record->assemblyStartedTick = 0;
    record->assemblyStartTickKnown = false;
    record->assemblySourceSequence = 0;
    record->productionMayUseBusyFactory = false;
    record->productionCompletedByUnit.clear();
    record->productionWorkOrders.clear();
    record->hasCreationPulse = false;
    record->createdAtConfirmedTick = 0;
    record->pendingInitialCreationPulse = true;
    return true;
}

bool ObjectTeamRegistry::activate(ObjectTeamId team, uint64_t confirmedTick) noexcept {
    ObjectTeamRecord* record = mutableFind(team);
    if (!record) return false;
    if (record->active) return true;
    record->active = true;
    // Activation commonly happens after this tick's ScriptRuntime hook scan
    // (script effects, transport arrival, production completion). Preserve the
    // pulse for the next confirmed script boundary as well; otherwise
    // TEAM_CREATED/OnCreate would expire before any script could observe it.
    record->pendingInitialCreationPulse = true;
    record->hasCreationPulse = true;
    record->createdAtConfirmedTick = confirmedTick;
    record->assemblyKind = ObjectTeamAssemblyKind::None;
    record->assemblyDeadlineTick = 0;
    record->assemblyStartedTick = 0;
    record->assemblyStartTickKnown = false;
    record->assemblySourceSequence = 0;
    record->productionMayUseBusyFactory = false;
    record->productionCompletedByUnit.clear();
    record->productionWorkOrders.clear();
    return true;
}

bool ObjectTeamRegistry::deactivate(ObjectTeamId team) noexcept {
    ObjectTeamRecord* record = mutableFind(team);
    if (!record || !record->active) return false;
    record->active = false;
    record->hasCreationPulse = false;
    record->pendingInitialCreationPulse = false;
    record->hasProductionStartPulse = false;
    record->pendingProductionStartPulse = false;
    record->productionStartedAtConfirmedTick = 0;
    record->productionActionPulseCount = 0;
    record->pendingProductionActionPulseCount = 0;
    record->productionActionWithoutTeamPulseCount = 0;
    record->pendingProductionActionWithoutTeamPulseCount = 0;
    record->commonTarget = INVALID_OBJECT_ID;
    record->pendingReinforcements.clear();
    return true;
}

bool ObjectTeamRegistry::beginAssembly(
    ObjectTeamId team, ObjectTeamAssemblyKind kind,
    uint64_t deadlineTick, uint32_t sourceSequence,
    std::optional<uint64_t> startedTick,
    bool productionMayUseBusyFactory) noexcept {
    ObjectTeamRecord* record = mutableFind(team);
    if (!record || kind == ObjectTeamAssemblyKind::None || record->active ||
        deadlineTick == 0) return false;
    record->assemblyKind = kind;
    record->assemblyDeadlineTick = deadlineTick;
    if (startedTick) {
        record->assemblyStartedTick = *startedTick;
        record->assemblyStartTickKnown = true;
    }
    record->assemblySourceSequence = sourceSequence;
    record->productionMayUseBusyFactory =
        kind == ObjectTeamAssemblyKind::Production &&
        productionMayUseBusyFactory;
    ++record->policyRevision;
    if (record->policyRevision == 0) ++record->policyRevision;
    return true;
}

bool ObjectTeamRegistry::clearAssembly(ObjectTeamId team) noexcept {
    ObjectTeamRecord* record = mutableFind(team);
    if (!record) return false;
    if (record->assemblyKind == ObjectTeamAssemblyKind::None &&
        record->assemblyDeadlineTick == 0 &&
        !record->assemblyStartTickKnown &&
        record->assemblySourceSequence == 0 &&
        !record->productionMayUseBusyFactory &&
        record->productionCompletedByUnit.empty() &&
        record->productionWorkOrders.empty()) return true;
    record->assemblyKind = ObjectTeamAssemblyKind::None;
    record->assemblyDeadlineTick = 0;
    record->assemblyStartedTick = 0;
    record->assemblyStartTickKnown = false;
    record->assemblySourceSequence = 0;
    record->productionMayUseBusyFactory = false;
    record->productionCompletedByUnit.clear();
    record->productionWorkOrders.clear();
    ++record->policyRevision;
    if (record->policyRevision == 0) ++record->policyRevision;
    return true;
}

bool ObjectTeamRegistry::updateAssemblySourceSequence(
    ObjectTeamId team, uint32_t sourceSequence) noexcept {
    ObjectTeamRecord* record = mutableFind(team);
    if (!record || record->assemblyKind == ObjectTeamAssemblyKind::None ||
        sourceSequence == 0 ||
        record->assemblySourceSequence == sourceSequence) return false;
    record->assemblySourceSequence = sourceSequence;
    ++record->policyRevision;
    if (record->policyRevision == 0) ++record->policyRevision;
    return true;
}

bool ObjectTeamRegistry::initializeProductionProgress(
    ObjectTeamId team, size_t rosterEntryCount) {
    ObjectTeamRecord* record = mutableFind(team);
    if (!record || record->active ||
        record->assemblyKind != ObjectTeamAssemblyKind::Production)
        return false;
    try {
        record->productionCompletedByUnit.assign(rosterEntryCount, 0u);
        record->productionWorkOrders.assign(
            rosterEntryCount, ObjectTeamProductionWorkOrder{});
    } catch (const std::bad_alloc&) {
        record->productionCompletedByUnit.clear();
        record->productionWorkOrders.clear();
        return false;
    }
    ++record->policyRevision;
    if (record->policyRevision == 0) ++record->policyRevision;
    return true;
}

bool ObjectTeamRegistry::recordProductionUnitCompleted(
    ObjectTeamId team, uint32_t rosterIndex) noexcept {
    ObjectTeamRecord* record = mutableFind(team);
    if (!record ||
        (record->assemblyKind != ObjectTeamAssemblyKind::Production &&
         record->assemblyKind != ObjectTeamAssemblyKind::ProductionReady) ||
        rosterIndex >= record->productionCompletedByUnit.size()) {
        return false;
    }
    uint32_t& completed = record->productionCompletedByUnit[rosterIndex];
    if (completed == std::numeric_limits<uint32_t>::max()) return false;
    ++completed;
    if (rosterIndex < record->productionWorkOrders.size())
        record->productionWorkOrders[rosterIndex] = {};
    ++record->policyRevision;
    if (record->policyRevision == 0) ++record->policyRevision;
    return true;
}

uint32_t ObjectTeamRegistry::productionUnitCompleted(
    ObjectTeamId team, uint32_t rosterIndex) const noexcept {
    const ObjectTeamRecord* record = find(team);
    return record && rosterIndex < record->productionCompletedByUnit.size()
        ? record->productionCompletedByUnit[rosterIndex] : 0u;
}

ObjectTeamProductionWorkOrder ObjectTeamRegistry::productionWorkOrder(
    ObjectTeamId team, uint32_t rosterIndex) const noexcept {
    const ObjectTeamRecord* record = find(team);
    return record && rosterIndex < record->productionWorkOrders.size()
        ? record->productionWorkOrders[rosterIndex]
        : ObjectTeamProductionWorkOrder{};
}

bool ObjectTeamRegistry::updateProductionWorkOrder(
    ObjectTeamId team, uint32_t rosterIndex, ObjectId producer,
    uint64_t nextAttemptTick, bool failed) noexcept {
    ObjectTeamRecord* record = mutableFind(team);
    if (!record || rosterIndex >= record->productionWorkOrders.size())
        return false;
    ObjectTeamProductionWorkOrder& order =
        record->productionWorkOrders[rosterIndex];
    order.producer = producer;
    order.nextAttemptTick = nextAttemptTick;
    if (failed && order.failureCount !=
            std::numeric_limits<uint32_t>::max()) {
        ++order.failureCount;
    } else if (!failed) {
        order.failureCount = 0;
    }
    ++record->policyRevision;
    if (record->policyRevision == 0) ++record->policyRevision;
    return true;
}

bool ObjectTeamRegistry::markProductionStarted(
    ObjectTeamId team, uint64_t confirmedTick,
    uint32_t actionCount, bool bindTeamContext) noexcept {
    ObjectTeamRecord* record = mutableFind(team);
    if (!record || record->active ||
        record->assemblyKind != ObjectTeamAssemblyKind::Production ||
        actionCount == 0)
        return false;
    record->pendingProductionStartPulse = true;
    uint32_t& pending = bindTeamContext
        ? record->pendingProductionActionPulseCount
        : record->pendingProductionActionWithoutTeamPulseCount;
    pending = actionCount > std::numeric_limits<uint32_t>::max() - pending
        ? std::numeric_limits<uint32_t>::max()
        : pending + actionCount;
    // The hook scan normally precedes BUILD_TEAM/assembly planning. Keep the
    // supplied tick only as diagnostic provenance; beginConfirmedTick stamps
    // the actual observable hook boundary.
    if (record->productionStartedAtConfirmedTick == 0)
        record->productionStartedAtConfirmedTick = confirmedTick;
    return true;
}

bool ObjectTeamRegistry::retireEmptyScenarioTeam(ObjectTeamId team) noexcept {
    ObjectTeamRecord* record = mutableFind(team);
    if (!record || record->kind != ObjectTeamKind::Scenario ||
        record->active || !record->members.empty() ||
        !record->scenarioDefinition) return false;
    auto binding = findScenarioTeam(record->scenarioDefinition);
    if (binding != m_scenarioTeams.end()) {
        binding->instances.erase(
            std::remove(binding->instances.begin(), binding->instances.end(),
                        team),
            binding->instances.end());
    }
    record->id = INVALID_OBJECT_TEAM_ID;
    record->scenarioDefinition = INVALID_SCRIPT_TEAM_ID;
    record->name.clear();
    record->assemblyKind = ObjectTeamAssemblyKind::None;
    record->assemblyDeadlineTick = 0;
    record->assemblyStartedTick = 0;
    record->assemblyStartTickKnown = false;
    record->assemblySourceSequence = 0;
    record->productionCompletedByUnit.clear();
    record->productionWorkOrders.clear();
    record->hasProductionStartPulse = false;
    record->pendingProductionStartPulse = false;
    record->productionStartedAtConfirmedTick = 0;
    record->productionActionPulseCount = 0;
    record->pendingProductionActionPulseCount = 0;
    record->productionActionWithoutTeamPulseCount = 0;
    record->pendingProductionActionWithoutTeamPulseCount = 0;
    record->relationshipPolicy.reset();
    return true;
}

bool ObjectTeamRegistry::wasCreatedAt(ObjectTeamId team, uint64_t confirmedTick) const noexcept {
    const ObjectTeamRecord* record = find(team);
    return record && record->hasCreationPulse &&
        record->createdAtConfirmedTick == confirmedTick;
}

uint32_t ObjectTeamRegistry::productionActionCountAt(
    ObjectTeamId team, uint64_t confirmedTick) const noexcept {
    const ObjectTeamRecord* record = find(team);
    return record && record->hasProductionStartPulse &&
            record->productionStartedAtConfirmedTick == confirmedTick
        ? record->productionActionPulseCount : 0u;
}

uint32_t ObjectTeamRegistry::productionActionWithoutTeamCountAt(
    ObjectTeamId team, uint64_t confirmedTick) const noexcept {
    const ObjectTeamRecord* record = find(team);
    return record && record->hasProductionStartPulse &&
            record->productionStartedAtConfirmedTick == confirmedTick
        ? record->productionActionWithoutTeamPulseCount : 0u;
}

container::Span<const ObjectId> ObjectTeamRegistry::members(ObjectTeamId team) const noexcept {
    const ObjectTeamRecord* record = find(team);
    return record ? record->members.values() : container::Span<const ObjectId>{};
}

uint64_t ObjectTeamRegistry::membershipRevision(
    ObjectTeamId team) const noexcept {
    const ObjectTeamRecord* record = find(team);
    if (!record) return 0;
    uint64_t hash = 1469598103934665603ull;
    const auto byte = [&hash](uint8_t value) noexcept {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    for (uint32_t shift = 0; shift < 32; shift += 8)
        byte(static_cast<uint8_t>(team.value >> shift));
    byte(record->active ? uint8_t{1} : uint8_t{0});
    for (const ObjectId member : record->members.values()) {
        for (uint32_t shift = 0; shift < 32; shift += 8)
            byte(static_cast<uint8_t>(member.value >> shift));
    }
    return hash == 0 ? 1 : hash;
}

container::Span<const ObjectId> ObjectTeamRegistry::legacyMembers(
    ObjectTeamId team) const noexcept {
    const ObjectTeamRecord* record = find(team);
    return record ? container::Span<const ObjectId>{record->legacyMemberOrder}
                  : container::Span<const ObjectId>{};
}

bool ObjectTeamRegistry::setCommonTarget(
    ObjectTeamId team, ObjectId target) noexcept {
    ObjectTeamRecord* record = mutableFind(team);
    if (!record) return false;
    if (record->commonTarget == target) return true;
    record->commonTarget = target;
    ++record->policyRevision;
    if (record->policyRevision == 0) ++record->policyRevision;
    return true;
}

bool ObjectTeamRegistry::clearCommonTargetIf(
    ObjectTeamId team, ObjectId target) noexcept {
    ObjectTeamRecord* record = mutableFind(team);
    if (!record || !target || record->commonTarget != target)
        return false;
    record->commonTarget = INVALID_OBJECT_ID;
    ++record->policyRevision;
    if (record->policyRevision == 0) ++record->policyRevision;
    return true;
}

bool ObjectTeamRegistry::addPendingReinforcement(
    ObjectTeamId team, ObjectId object) {
    ObjectTeamRecord* record = mutableFind(team);
    if (!record || !record->active || !object ||
        !record->members.contains(object)) return false;
    const auto position = std::lower_bound(
        record->pendingReinforcements.begin(),
        record->pendingReinforcements.end(), object);
    if (position != record->pendingReinforcements.end() &&
        *position == object) return true;
    record->pendingReinforcements.insert(position, object);
    ++record->policyRevision;
    if (record->policyRevision == 0) ++record->policyRevision;
    return true;
}

bool ObjectTeamRegistry::removePendingReinforcement(
    ObjectTeamId team, ObjectId object) noexcept {
    ObjectTeamRecord* record = mutableFind(team);
    if (!record || !object) return false;
    const auto position = std::lower_bound(
        record->pendingReinforcements.begin(),
        record->pendingReinforcements.end(), object);
    if (position == record->pendingReinforcements.end() ||
        *position != object) return true;
    record->pendingReinforcements.erase(position);
    ++record->policyRevision;
    if (record->policyRevision == 0) ++record->policyRevision;
    return true;
}

bool ObjectTeamRegistry::assignObject(ObjectTeamId team, ObjectId object) {
    if (!object) return false;
    ObjectTeamRecord* destination = mutableFind(team);
    if (!destination) return false;

    const auto previous = m_teamByObject.find(object);
    if (previous != m_teamByObject.end()) {
        if (previous->second == team) {
            return destination->members.contains(object) &&
                std::count(destination->legacyMemberOrder.begin(),
                           destination->legacyMemberOrder.end(), object) == 1;
        }
        // Reject an already-corrupt destination before removing the old
        // membership. Under normal operation this cannot occur, but keeping
        // the mutation all-or-nothing prevents a bad caller from turning a
        // unique primary Team into zero Teams.
        if (destination->members.contains(object) ||
            std::find(destination->legacyMemberOrder.begin(),
                      destination->legacyMemberOrder.end(), object) !=
                destination->legacyMemberOrder.end()) {
            return false;
        }
        ObjectTeamRecord* oldTeam = mutableFind(previous->second);
        if (!oldTeam || !oldTeam->members.contains(object)) return false;
        const auto oldLegacy = std::find(oldTeam->legacyMemberOrder.begin(),
                                         oldTeam->legacyMemberOrder.end(), object);
        if (oldLegacy == oldTeam->legacyMemberOrder.end()) return false;

        // Add to the destination before removing the old membership. If a
        // caller ever violates an index invariant, the object remains owned
        // by its original Team instead of becoming unteamed.
        if (!destination->members.insert(object)) return false;
        try {
            destination->legacyMemberOrder.insert(
                destination->legacyMemberOrder.begin(), object);
        } catch (...) {
            destination->members.erase(object);
            throw;
        }
        oldTeam->members.erase(object);
        oldTeam->legacyMemberOrder.erase(oldLegacy);
        const auto pending = std::lower_bound(
            oldTeam->pendingReinforcements.begin(),
            oldTeam->pendingReinforcements.end(), object);
        if (pending != oldTeam->pendingReinforcements.end() &&
            *pending == object) {
            oldTeam->pendingReinforcements.erase(pending);
            ++oldTeam->policyRevision;
            if (oldTeam->policyRevision == 0) ++oldTeam->policyRevision;
        }
        m_teamByObject.insert_or_assign(object, team);
        return true;
    }
    if (!destination->members.insert(object)) return false;
    try {
        destination->legacyMemberOrder.insert(
            destination->legacyMemberOrder.begin(), object);
    } catch (...) {
        destination->members.erase(object);
        throw;
    }
    m_teamByObject.insert_or_assign(object, team);
    return true;
}

void ObjectTeamRegistry::removeObject(ObjectId object) noexcept {
    for (ObjectTeamRecord& team : m_teams) {
        bool changed = false;
        if (team.commonTarget == object) {
            team.commonTarget = INVALID_OBJECT_ID;
            changed = true;
        }
        const auto pending = std::lower_bound(
            team.pendingReinforcements.begin(),
            team.pendingReinforcements.end(), object);
        if (pending != team.pendingReinforcements.end() &&
            *pending == object) {
            team.pendingReinforcements.erase(pending);
            changed = true;
        }
        if (changed) {
            ++team.policyRevision;
            if (team.policyRevision == 0) ++team.policyRevision;
        }
    }
    const auto found = m_teamByObject.find(object);
    if (found == m_teamByObject.end()) return;
    if (ObjectTeamRecord* team = mutableFind(found->second)) {
        team->members.erase(object);
        const auto legacy = std::find(team->legacyMemberOrder.begin(),
                                      team->legacyMemberOrder.end(), object);
        if (legacy != team->legacyMemberOrder.end()) {
            team->legacyMemberOrder.erase(legacy);
        }
    }
    m_teamByObject.erase(found);
}

bool ObjectTeamRegistry::setTeamOwner(ObjectTeamId team, PlayerId owner) noexcept {
    if (!owner.isValid()) return false;
    ObjectTeamRecord* record = mutableFind(team);
    if (!record) return false;
    record->owner = owner;
    return true;
}

bool ObjectTeamRegistry::setScriptState(ObjectTeamId team,
                                        container::String state) {
    ObjectTeamRecord* record = mutableFind(team);
    if (!record) return false;
    record->scriptState = std::move(state);
    return true;
}

bool ObjectTeamRegistry::setRecruitableOverride(
    ObjectTeamId team, bool recruitable) noexcept {
    ObjectTeamRecord* record = mutableFind(team);
    if (!record || (record->recruitableOverride &&
                    *record->recruitableOverride == recruitable)) {
        return false;
    }
    record->recruitableOverride = recruitable;
    ++record->policyRevision;
    if (record->policyRevision == 0) ++record->policyRevision;
    return true;
}

bool ObjectTeamRegistry::adjustProductionPriority(
    ObjectTeamId team, int32_t delta) noexcept {
    ObjectTeamRecord* record = mutableFind(team);
    if (!record || delta == 0) return false;
    auto binding = record->scenarioDefinition
        ? findScenarioTeam(record->scenarioDefinition)
        : m_scenarioTeams.end();
    const int32_t authoredDelta = binding == m_scenarioTeams.end()
        ? delta
        : delta > 0
            ? binding->productionPrioritySuccessIncrease
            : -binding->productionPriorityFailureDecrease;
    if (authoredDelta == 0) return false;
    const int64_t base = binding == m_scenarioTeams.end()
        ? record->productionPriority : binding->productionPriority;
    const int64_t adjusted = base + static_cast<int64_t>(authoredDelta);
    const int32_t next = static_cast<int32_t>(std::clamp(
        adjusted, static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
        static_cast<int64_t>(std::numeric_limits<int32_t>::max())));
    if (binding == m_scenarioTeams.end()) {
        record->productionPriority = next;
        ++record->policyRevision;
        if (record->policyRevision == 0) ++record->policyRevision;
    } else {
        binding->productionPriority = next;
        for (const ObjectTeamId instance : binding->instances) {
            if (ObjectTeamRecord* current = mutableFind(instance)) {
                current->productionPriority = next;
                ++current->policyRevision;
                if (current->policyRevision == 0)
                    ++current->policyRevision;
            }
        }
    }
    return true;
}

std::optional<int32_t> ObjectTeamRegistry::productionPriority(
    ObjectTeamId team) const noexcept {
    const ObjectTeamRecord* record = find(team);
    if (!record) return std::nullopt;
    const auto binding = record->scenarioDefinition
        ? findScenarioTeam(record->scenarioDefinition)
        : m_scenarioTeams.end();
    return binding == m_scenarioTeams.end()
        ? std::optional<int32_t>{record->productionPriority}
        : std::optional<int32_t>{binding->productionPriority};
}

bool ObjectTeamRegistry::setAttackPrioritySet(
    ObjectTeamId team, container::String setName) {
    if (!find(team)) return false;
    auto binding = std::find_if(
        m_scenarioTeams.begin(), m_scenarioTeams.end(),
        [team](const ScenarioTeamBinding& candidate) {
            return std::find(candidate.instances.begin(),
                             candidate.instances.end(), team) !=
                candidate.instances.end();
        });
    if (binding == m_scenarioTeams.end() ||
        binding->attackPrioritySet == setName) return false;
    binding->attackPrioritySet = std::move(setName);
    for (const ObjectTeamId instance : binding->instances) {
        if (ObjectTeamRecord* record = mutableFind(instance)) {
            ++record->policyRevision;
            if (record->policyRevision == 0) ++record->policyRevision;
        }
    }
    return true;
}

std::optional<container::StringView>
ObjectTeamRegistry::attackPrioritySet(ObjectTeamId team) const noexcept {
    if (!find(team)) return std::nullopt;
    const auto binding = std::find_if(
        m_scenarioTeams.begin(), m_scenarioTeams.end(),
        [team](const ScenarioTeamBinding& candidate) {
            return std::find(candidate.instances.begin(),
                             candidate.instances.end(), team) !=
                candidate.instances.end();
        });
    if (binding == m_scenarioTeams.end() ||
        binding->attackPrioritySet.empty()) return std::nullopt;
    return container::StringView{binding->attackPrioritySet};
}

bool ObjectTeamRegistry::setTeamRelationshipOverride(
    ObjectTeamId source, ObjectTeamId target,
    PlayerRelationship relationship) {
    ObjectTeamRecord* record = mutableFind(source);
    if (!record || !find(target)) return false;
    auto next = record->relationshipPolicy
        ? std::make_shared<ObjectRelationshipOverridePolicy>(
              *record->relationshipPolicy)
        : std::make_shared<ObjectRelationshipOverridePolicy>();
    const auto position = std::lower_bound(
        next->teams.begin(), next->teams.end(), target,
        [](const ObjectTeamRelationshipOverride& entry,
           ObjectTeamId value) { return entry.target < value; });
    if (position != next->teams.end() && position->target == target) {
        if (position->relationship == relationship) return false;
        position->relationship = relationship;
    } else {
        next->teams.insert(position, {
            .target = target,
            .relationship = relationship,
        });
    }
    next->revision = record->relationshipPolicy
        ? record->relationshipPolicy->revision + 1 : 1;
    if (next->revision == 0) ++next->revision;
    record->relationshipPolicy = std::move(next);
    ++record->policyRevision;
    if (record->policyRevision == 0) ++record->policyRevision;
    return true;
}

bool ObjectTeamRegistry::removeTeamRelationshipOverride(
    ObjectTeamId source, ObjectTeamId target) {
    ObjectTeamRecord* record = mutableFind(source);
    if (!record || !record->relationshipPolicy || !target) return false;
    auto next = std::make_shared<ObjectRelationshipOverridePolicy>(
        *record->relationshipPolicy);
    const auto position = std::lower_bound(
        next->teams.begin(), next->teams.end(), target,
        [](const ObjectTeamRelationshipOverride& entry,
           ObjectTeamId value) { return entry.target < value; });
    if (position == next->teams.end() || position->target != target)
        return false;
    next->teams.erase(position);
    next->revision = record->relationshipPolicy->revision + 1;
    if (next->revision == 0) ++next->revision;
    record->relationshipPolicy = std::move(next);
    ++record->policyRevision;
    if (record->policyRevision == 0) ++record->policyRevision;
    return true;
}

bool ObjectTeamRegistry::setPlayerRelationshipOverride(
    ObjectTeamId source, PlayerId target,
    PlayerRelationship relationship) {
    ObjectTeamRecord* record = mutableFind(source);
    if (!record || !target.isValid()) return false;
    auto next = record->relationshipPolicy
        ? std::make_shared<ObjectRelationshipOverridePolicy>(
              *record->relationshipPolicy)
        : std::make_shared<ObjectRelationshipOverridePolicy>();
    const auto position = std::lower_bound(
        next->players.begin(), next->players.end(), target,
        [](const ObjectPlayerRelationshipOverride& entry,
           PlayerId value) { return entry.target < value; });
    if (position != next->players.end() && position->target == target) {
        if (position->relationship == relationship) return false;
        position->relationship = relationship;
    } else {
        next->players.insert(position, {
            .target = target,
            .relationship = relationship,
        });
    }
    next->revision = record->relationshipPolicy
        ? record->relationshipPolicy->revision + 1 : 1;
    if (next->revision == 0) ++next->revision;
    record->relationshipPolicy = std::move(next);
    ++record->policyRevision;
    if (record->policyRevision == 0) ++record->policyRevision;
    return true;
}

bool ObjectTeamRegistry::removePlayerRelationshipOverride(
    ObjectTeamId source, PlayerId target) {
    ObjectTeamRecord* record = mutableFind(source);
    if (!record || !record->relationshipPolicy || !target.isValid())
        return false;
    auto next = std::make_shared<ObjectRelationshipOverridePolicy>(
        *record->relationshipPolicy);
    const auto position = std::lower_bound(
        next->players.begin(), next->players.end(), target,
        [](const ObjectPlayerRelationshipOverride& entry,
           PlayerId value) { return entry.target < value; });
    if (position == next->players.end() || position->target != target)
        return false;
    next->players.erase(position);
    next->revision = record->relationshipPolicy->revision + 1;
    if (next->revision == 0) ++next->revision;
    record->relationshipPolicy = std::move(next);
    ++record->policyRevision;
    if (record->policyRevision == 0) ++record->policyRevision;
    return true;
}

bool ObjectTeamRegistry::clearRelationshipOverrides(ObjectTeamId source) {
    ObjectTeamRecord* record = mutableFind(source);
    if (!record || !record->relationshipPolicy ||
        (record->relationshipPolicy->teams.empty() &&
         record->relationshipPolicy->players.empty())) return false;
    auto next = std::make_shared<ObjectRelationshipOverridePolicy>();
    next->revision = record->relationshipPolicy->revision + 1;
    if (next->revision == 0) ++next->revision;
    record->relationshipPolicy = std::move(next);
    ++record->policyRevision;
    if (record->policyRevision == 0) ++record->policyRevision;
    return true;
}

container::SharedPtr<const ObjectRelationshipOverridePolicy>
ObjectTeamRegistry::relationshipPolicy(ObjectTeamId team) const noexcept {
    const ObjectTeamRecord* record = find(team);
    return record ? record->relationshipPolicy
                  : container::SharedPtr<const ObjectRelationshipOverridePolicy>{};
}

bool ObjectTeamRegistry::captureSnapshot(
    ObjectTeamRegistrySnapshot& output) const {
    try {
        ObjectTeamRegistrySnapshot candidate;
        candidate.teams = m_teams;
        candidate.defaultTeams = m_defaultTeams;
        candidate.scenarioTeams.reserve(m_scenarioTeams.size());
        for (const ScenarioTeamBinding& binding : m_scenarioTeams) {
            candidate.scenarioTeams.push_back({
                .definition = binding.definition,
                .instances = binding.instances,
                .productionPriority = binding.productionPriority,
                .productionPrioritySuccessIncrease =
                    binding.productionPrioritySuccessIncrease,
                .productionPriorityFailureDecrease =
                    binding.productionPriorityFailureDecrease,
                .productionPolicyConfigured =
                    binding.productionPolicyConfigured,
                .attackPrioritySet = binding.attackPrioritySet,
            });
        }
        output = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

bool ObjectTeamRegistry::restoreSnapshot(
    const ObjectTeamRegistrySnapshot& snapshot) {
    if (snapshot.schemaVersion != ObjectTeamRegistrySnapshot::SchemaVersion ||
        snapshot.teams.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    try {
        ObjectTeamRegistry candidate;
        candidate.m_teams = snapshot.teams;
        candidate.m_defaultTeams = snapshot.defaultTeams;
        for (size_t index = 0; index < candidate.m_teams.size(); ++index) {
            const ObjectTeamRecord& record = candidate.m_teams[index];
            const ObjectTeamId expected{
                static_cast<uint32_t>(index + 1u)};
            if (!record.id) {
                if (!record.members.empty() ||
                    !record.legacyMemberOrder.empty() ||
                    record.commonTarget ||
                    !record.pendingReinforcements.empty()) return false;
                continue;
            }
            if (record.id != expected || !record.owner.isValid() ||
                record.name.empty() ||
                static_cast<uint8_t>(record.kind) >
                    static_cast<uint8_t>(ObjectTeamKind::Scenario) ||
                (record.kind == ObjectTeamKind::PlayerDefault
                     ? static_cast<bool>(record.scenarioDefinition)
                     : !static_cast<bool>(record.scenarioDefinition)) ||
                static_cast<uint8_t>(record.assemblyKind) >
                    static_cast<uint8_t>(
                        ObjectTeamAssemblyKind::ProductionReady) ||
                record.policyRevision == 0 ||
                record.productionCompletedByUnit.size() !=
                    record.productionWorkOrders.size() ||
                (record.active &&
                 (record.assemblyKind != ObjectTeamAssemblyKind::None ||
                  !record.productionCompletedByUnit.empty())) ||
                (!record.active && record.commonTarget) ||
                (!record.active && !record.pendingReinforcements.empty()) ||
                !std::is_sorted(record.pendingReinforcements.begin(),
                                record.pendingReinforcements.end()) ||
                std::adjacent_find(record.pendingReinforcements.begin(),
                                   record.pendingReinforcements.end()) !=
                    record.pendingReinforcements.end() ||
                (record.assemblyKind == ObjectTeamAssemblyKind::None &&
                 (record.assemblyDeadlineTick != 0 ||
                  record.assemblyStartedTick != 0 ||
                  record.assemblyStartTickKnown ||
                  record.assemblySourceSequence != 0 ||
                  record.productionMayUseBusyFactory ||
                  !record.productionCompletedByUnit.empty())) ||
                (record.assemblyKind != ObjectTeamAssemblyKind::None &&
                 record.assemblyDeadlineTick == 0) ||
                (record.assemblyKind != ObjectTeamAssemblyKind::Production &&
                 record.productionMayUseBusyFactory) ||
                ((record.assemblyKind != ObjectTeamAssemblyKind::Production &&
                  record.assemblyKind !=
                      ObjectTeamAssemblyKind::ProductionReady) &&
                 !record.productionCompletedByUnit.empty())) {
                return false;
            }
            if (record.kind == ObjectTeamKind::PlayerDefault &&
                std::count(candidate.m_defaultTeams.begin(),
                           candidate.m_defaultTeams.end(), record.id) != 1) {
                return false;
            }
            for (const ObjectId member : record.members.values()) {
                if (!member || std::count(
                        record.legacyMemberOrder.begin(),
                        record.legacyMemberOrder.end(), member) != 1 ||
                    !candidate.m_teamByObject.emplace(
                        member, record.id).second) {
                    return false;
                }
            }
            for (const ObjectId reinforcement :
                 record.pendingReinforcements) {
                if (!record.members.contains(reinforcement)) return false;
            }
            if (record.members.size() != record.legacyMemberOrder.size())
                return false;
            if (record.relationshipPolicy) {
                if (record.relationshipPolicy->revision == 0 ||
                    !std::is_sorted(
                        record.relationshipPolicy->teams.begin(),
                        record.relationshipPolicy->teams.end(),
                        [](const ObjectTeamRelationshipOverride& left,
                           const ObjectTeamRelationshipOverride& right) {
                            return left.target < right.target;
                        }) ||
                    !std::is_sorted(
                        record.relationshipPolicy->players.begin(),
                        record.relationshipPolicy->players.end(),
                        [](const ObjectPlayerRelationshipOverride& left,
                           const ObjectPlayerRelationshipOverride& right) {
                            return left.target < right.target;
                        }) ||
                    std::adjacent_find(
                        record.relationshipPolicy->teams.begin(),
                        record.relationshipPolicy->teams.end(),
                        [](const ObjectTeamRelationshipOverride& left,
                           const ObjectTeamRelationshipOverride& right) {
                            return left.target == right.target;
                        }) != record.relationshipPolicy->teams.end() ||
                    std::adjacent_find(
                        record.relationshipPolicy->players.begin(),
                        record.relationshipPolicy->players.end(),
                        [](const ObjectPlayerRelationshipOverride& left,
                           const ObjectPlayerRelationshipOverride& right) {
                            return left.target == right.target;
                        }) != record.relationshipPolicy->players.end()) {
                    return false;
                }
                for (const ObjectTeamRelationshipOverride& value :
                     record.relationshipPolicy->teams) {
                    if (!candidate.find(value.target) ||
                        static_cast<uint8_t>(value.relationship) >
                            static_cast<uint8_t>(
                                PlayerRelationship::Neutral)) {
                        return false;
                    }
                }
                for (const ObjectPlayerRelationshipOverride& value :
                     record.relationshipPolicy->players) {
                    if (!value.target.isValid() ||
                        static_cast<uint8_t>(value.relationship) >
                            static_cast<uint8_t>(
                                PlayerRelationship::Neutral)) {
                        return false;
                    }
                }
            }
        }
        for (size_t player = 0;
             player < candidate.m_defaultTeams.size(); ++player) {
            const ObjectTeamId team = candidate.m_defaultTeams[player];
            if (!team) continue;
            const ObjectTeamRecord* record = candidate.find(team);
            if (!record || record->kind != ObjectTeamKind::PlayerDefault)
                return false;
        }
        ScriptTeamId previousDefinition = INVALID_SCRIPT_TEAM_ID;
        container::Vector<uint8_t> boundInstances(
            candidate.m_teams.size() + 1u, uint8_t{0});
        candidate.m_scenarioTeams.reserve(snapshot.scenarioTeams.size());
        for (const ObjectTeamScenarioBindingSnapshot& source :
             snapshot.scenarioTeams) {
            if (!source.definition ||
                (previousDefinition &&
                 !(previousDefinition < source.definition)) ||
                source.productionPrioritySuccessIncrease < 0 ||
                source.productionPriorityFailureDecrease < 0) {
                return false;
            }
            previousDefinition = source.definition;
            if (!std::is_sorted(source.instances.begin(),
                                source.instances.end()) ||
                std::adjacent_find(source.instances.begin(),
                                   source.instances.end()) !=
                    source.instances.end()) {
                return false;
            }
            for (const ObjectTeamId instance : source.instances) {
                const ObjectTeamRecord* record = candidate.find(instance);
                if (!record || instance.value >= boundInstances.size() ||
                    boundInstances[instance.value] != 0 ||
                    record->productionPriority !=
                        source.productionPriority ||
                    (record->kind == ObjectTeamKind::Scenario
                        ? record->scenarioDefinition != source.definition
                        : record->kind != ObjectTeamKind::PlayerDefault)) {
                    return false;
                }
                boundInstances[instance.value] = 1;
            }
            candidate.m_scenarioTeams.push_back({
                .definition = source.definition,
                .instances = source.instances,
                .productionPriority = source.productionPriority,
                .productionPrioritySuccessIncrease =
                    source.productionPrioritySuccessIncrease,
                .productionPriorityFailureDecrease =
                    source.productionPriorityFailureDecrease,
                .productionPolicyConfigured =
                    source.productionPolicyConfigured,
                .attackPrioritySet = source.attackPrioritySet,
            });
        }
        for (const ObjectTeamRecord& record : candidate.m_teams) {
            if (record.id && record.kind == ObjectTeamKind::Scenario &&
                boundInstances[record.id.value] == 0) {
                return false;
            }
        }
        *this = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

uint64_t ObjectTeamRegistry::stableHash() const noexcept {
    uint64_t result = 14695981039346656037ull;
    const auto byte = [&result](uint8_t value) noexcept {
        result ^= value;
        result *= 1099511628211ull;
    };
    const auto u32 = [&byte](uint32_t value) noexcept {
        for (uint32_t shift = 0; shift < 32; shift += 8)
            byte(static_cast<uint8_t>((value >> shift) & 0xffu));
    };
    const auto u64 = [&byte](uint64_t value) noexcept {
        for (uint32_t shift = 0; shift < 64; shift += 8)
            byte(static_cast<uint8_t>((value >> shift) & 0xffu));
    };
    const auto string = [&u64, &byte](container::StringView value) noexcept {
        u64(static_cast<uint64_t>(value.size()));
        for (const char character : value)
            byte(static_cast<uint8_t>(character));
    };
    u32(1);
    u64(static_cast<uint64_t>(m_teams.size()));
    for (const ObjectTeamRecord& record : m_teams) {
        u32(record.id.value);
        byte(static_cast<uint8_t>(record.kind));
        byte(record.owner.value);
        byte(record.active ? 1u : 0u);
        byte(record.hasCreationPulse ? 1u : 0u);
        byte(record.pendingInitialCreationPulse ? 1u : 0u);
        u64(record.createdAtConfirmedTick);
        byte(record.hasProductionStartPulse ? 1u : 0u);
        byte(record.pendingProductionStartPulse ? 1u : 0u);
        u64(record.productionStartedAtConfirmedTick);
        u32(record.productionActionPulseCount);
        u32(record.pendingProductionActionPulseCount);
        u32(record.productionActionWithoutTeamPulseCount);
        u32(record.pendingProductionActionWithoutTeamPulseCount);
        u32(record.scenarioDefinition.value);
        string(record.name);
        string(record.scriptState);
        byte(record.recruitableOverride.has_value() ? 1u : 0u);
        if (record.recruitableOverride)
            byte(*record.recruitableOverride ? 1u : 0u);
        u32(static_cast<uint32_t>(record.productionPriority));
        u32(record.commonTarget.value);
        u64(static_cast<uint64_t>(record.pendingReinforcements.size()));
        for (ObjectId reinforcement : record.pendingReinforcements)
            u32(reinforcement.value);
        byte(static_cast<uint8_t>(record.assemblyKind));
        u64(record.assemblyDeadlineTick);
        u64(record.assemblyStartedTick);
        byte(record.assemblyStartTickKnown ? 1u : 0u);
        u32(record.assemblySourceSequence);
        byte(record.productionMayUseBusyFactory ? 1u : 0u);
        u64(static_cast<uint64_t>(record.productionCompletedByUnit.size()));
        for (uint32_t completed : record.productionCompletedByUnit)
            u32(completed);
        u64(static_cast<uint64_t>(record.productionWorkOrders.size()));
        for (const ObjectTeamProductionWorkOrder& order :
             record.productionWorkOrders) {
            u32(order.producer.value);
            u64(order.nextAttemptTick);
            u32(order.failureCount);
        }
        u64(record.policyRevision);
        u64(static_cast<uint64_t>(record.legacyMemberOrder.size()));
        for (ObjectId member : record.legacyMemberOrder)
            u32(member.value);
        u64(static_cast<uint64_t>(record.members.size()));
        for (ObjectId member : record.members.values()) u32(member.value);
        byte(record.relationshipPolicy ? 1u : 0u);
        if (record.relationshipPolicy) {
            u64(record.relationshipPolicy->revision);
            u64(static_cast<uint64_t>(
                record.relationshipPolicy->teams.size()));
            for (const ObjectTeamRelationshipOverride& value :
                 record.relationshipPolicy->teams) {
                u32(value.target.value);
                byte(static_cast<uint8_t>(value.relationship));
            }
            u64(static_cast<uint64_t>(
                record.relationshipPolicy->players.size()));
            for (const ObjectPlayerRelationshipOverride& value :
                 record.relationshipPolicy->players) {
                byte(value.target.value);
                byte(static_cast<uint8_t>(value.relationship));
            }
        }
    }
    for (ObjectTeamId team : m_defaultTeams) u32(team.value);
    u64(static_cast<uint64_t>(m_scenarioTeams.size()));
    for (const ScenarioTeamBinding& binding : m_scenarioTeams) {
        u32(binding.definition.value);
        u64(static_cast<uint64_t>(binding.instances.size()));
        for (ObjectTeamId instance : binding.instances)
            u32(instance.value);
        u32(static_cast<uint32_t>(binding.productionPriority));
        u32(static_cast<uint32_t>(
            binding.productionPrioritySuccessIncrease));
        u32(static_cast<uint32_t>(
            binding.productionPriorityFailureDecrease));
        byte(binding.productionPolicyConfigured ? 1u : 0u);
        string(binding.attackPrioritySet);
    }
    return result;
}

ObjectTeamRecord* ObjectTeamRegistry::mutableFind(ObjectTeamId team) noexcept {
    return const_cast<ObjectTeamRecord*>(std::as_const(*this).find(team));
}

std::optional<ObjectTeamId> ObjectTeamRegistry::appendTeam(
    ObjectTeamKind kind, PlayerId owner, ScriptTeamId definition, container::String name,
    bool active, uint64_t createdAtConfirmedTick, bool hasCreationPulse,
    bool pendingInitialCreationPulse) {
    if (!owner.isValid() || name.empty() || m_teams.size() >= std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }
    const ObjectTeamId id{static_cast<uint32_t>(m_teams.size() + 1)};
    m_teams.push_back({
        .id = id,
        .kind = kind,
        .owner = owner,
        .active = active,
        .hasCreationPulse = hasCreationPulse,
        .pendingInitialCreationPulse = pendingInitialCreationPulse,
        .createdAtConfirmedTick = createdAtConfirmedTick,
        .scenarioDefinition = definition,
        .name = std::move(name),
    });
    return id;
}

container::Vector<ObjectTeamRegistry::ScenarioTeamBinding>::iterator ObjectTeamRegistry::lowerBoundScenarioTeam(
    ScriptTeamId definition) {
    return std::lower_bound(m_scenarioTeams.begin(), m_scenarioTeams.end(), definition,
        [](const ScenarioTeamBinding& binding, ScriptTeamId needle) {
            return binding.definition < needle;
        });
}

container::Vector<ObjectTeamRegistry::ScenarioTeamBinding>::const_iterator
ObjectTeamRegistry::lowerBoundScenarioTeam(
    ScriptTeamId definition) const {
    return std::lower_bound(m_scenarioTeams.begin(), m_scenarioTeams.end(), definition,
        [](const ScenarioTeamBinding& binding, ScriptTeamId needle) {
            return binding.definition < needle;
        });
}

container::Vector<ObjectTeamRegistry::ScenarioTeamBinding>::iterator ObjectTeamRegistry::findScenarioTeam(
    ScriptTeamId definition) {
    const auto found = lowerBoundScenarioTeam(definition);
    return found != m_scenarioTeams.end() && found->definition == definition
        ? found
        : m_scenarioTeams.end();
}

container::Vector<ObjectTeamRegistry::ScenarioTeamBinding>::const_iterator ObjectTeamRegistry::findScenarioTeam(
    ScriptTeamId definition) const {
    const auto found = lowerBoundScenarioTeam(definition);
    return found != m_scenarioTeams.end() && found->definition == definition
        ? found
        : m_scenarioTeams.end();
}

} // namespace engine

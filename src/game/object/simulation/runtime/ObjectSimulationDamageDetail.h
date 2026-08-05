#pragma once

#include "game/object/simulation/runtime/ObjectSimulation.h"

namespace engine::object_simulation_detail {

using HealthScalar = ObjectHealthComponent::Scalar;

// Concrete synchronous Body reaction executor. These three callbacks are a
// fixed part of ActiveBody's contract, not caller-selected policy, so keeping
// them as type-erased function fields hid the authored order and allowed a
// caller to omit gameplay. The executor owns no data and never crosses a
// DeathWalk suspension boundary.
class ObjectBodyReactionExecutor final {
public:
    ObjectBodyReactionExecutor(
        ObjectSimulation& simulation, ecs::registry& registry,
        ObjectLifecycle& lifecycle, ObjectDamageRequest request,
        ObjectUpgradeExecutionContext context, PlayerId victimPlayer,
        PlayerId sourcePlayer, int32_t experienceValue,
        bool experienceEligibleKiller, uint64_t confirmedTick) noexcept;

    void dispatchDamage(const ObjectHealthEvent& event);
    void dispatchDamageStateChange(const ObjectHealthEvent& event);
    void awardLethalExperience();

private:
    ObjectSimulation& m_simulation;
    ecs::registry& m_registry;
    ObjectLifecycle& m_lifecycle;
    ObjectDamageRequest m_request;
    ObjectUpgradeExecutionContext m_context;
    PlayerId m_victimPlayer = INVALID_PLAYER_ID;
    PlayerId m_sourcePlayer = INVALID_PLAYER_ID;
    int32_t m_experienceValue = 0;
    bool m_experienceEligibleKiller = false;
    uint64_t m_confirmedTick = 0;
};

[[nodiscard]] bool hasKind(const ObjectKindOfComponent* kinds,
                           game::ObjectKindOf sought) noexcept;
[[nodiscard]] ObjectBodyDamageState damageStateFor(
    HealthScalar health, HealthScalar maximum,
    const ObjectSimulationRules& rules) noexcept;
[[nodiscard]] bool isSubdualDamage(game::DamageType type) noexcept;
[[nodiscard]] bool hasDedicatedDamageBehaviour(game::DamageType type) noexcept;
[[nodiscard]] bool isHealthDamagingDamage(game::DamageType type) noexcept;
[[nodiscard]] bool shouldStartSecondLife(
    const ObjectHealthComponent& health, const ObjectDamageRequest& request,
    HealthScalar estimatedAmount) noexcept;
void applyStructureRubbleGameplayState(
    ecs::registry& registry, ecs::entity entity,
    const ObjectSimulationRules& rules, uint64_t confirmedTick);
void projectBodyDamageVisual(
    ObjectBodyDamageState state, RenderModelComponent& visual) noexcept;
void updateBodyDamageVisuals(ecs::registry& registry);
[[nodiscard]] HealthScalar armorMultiplierFor(
    const ecs::registry& registry, ecs::entity entity,
    game::DamageType type) noexcept;
[[nodiscard]] HealthScalar damageMultiplierFor(
    const ecs::registry& registry, ecs::entity entity,
    game::DamageType type) noexcept;
void appendIgnoredEvent(
    container::Vector<ObjectHealthEvent>& events,
    const ObjectDamageRequest& request,
    const ObjectHealthComponent* health);
void rememberPreferredBodyDamageInfo(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectHealthComponent& health, const ObjectDamageRequest& request);
void applySubdualDamage(
    ecs::registry& registry, ecs::entity entity,
    ObjectHealthComponent& health, const ObjectDamageRequest& request,
    HealthScalar requestedAmount, const ObjectSimulationRules& rules,
    container::Vector<ObjectHealthEvent>& events);
void applyTimedStatusDamage(
    ecs::registry& registry, ecs::entity entity,
    ObjectHealthComponent& health, const ObjectDamageRequest& request,
    HealthScalar requestedAmount, const ObjectSimulationRules& rules,
    container::Vector<ObjectHealthEvent>& events);
void requestDeath(
    ObjectSimulation& simulation, ecs::registry& registry,
    ObjectLifecycle& lifecycle, ecs::entity entity,
    ObjectHealthComponent& health, const ObjectDamageRequest& request,
    HealthScalar resolvedDamage, HealthScalar clippedDamage,
    ObjectUpgradeExecutionContext context, uint64_t sessionSeed,
    bool scoreTheKillPath,
    ObjectDamageTransactionResult* transactionResult);
void applyDamageRequest(
    ObjectSimulation& simulation, ecs::registry& registry,
    ObjectLifecycle& lifecycle,
    const ObjectDamageRequest& request, HealthScalar requestedAmount,
    const ObjectSimulationRules& rules, ObjectUpgradeExecutionContext context,
    uint64_t sessionSeed, container::Vector<ObjectHealthEvent>& events,
    ObjectBodyReactionExecutor& reactions,
    ObjectDamageTransactionResult* transactionResult);

} // namespace engine::object_simulation_detail

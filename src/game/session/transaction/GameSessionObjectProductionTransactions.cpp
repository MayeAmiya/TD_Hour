#include "game/session/transaction/GameSessionObjectProductionTransactions.h"

#include <algorithm>

#include "game/data/base/UpgradeCatalog.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/session/transaction/GameSessionObjectDamageTransactions.h"
#include "game/session/state/GameSessionDomainState.h"

namespace engine
{
namespace
{

[[nodiscard]] GameSessionProductionCommandResult productionCommandResult(const ObjectProductionRequestResult& result)
{
    GameSessionProductionCommandResult output{
        .accepted = result.accepted,
        .productionId = result.productionId,
    };
    if (!result.accepted)
    {
        output.insufficientFunds = result.rejection == ObjectProductionRejectionReason::InsufficientFunds;
        output.message = container::String{objectProductionRejectionMessage(result.rejection)};
        if (output.message.empty())
            output.message = "production request was rejected";
    }
    return output;
}

} // namespace

GameSessionObjectProductionTransactions::GameSessionObjectProductionTransactions(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionScriptPresentationState& presentation,
    GameSessionProductionPolicyPort policy,
    GameSessionLifecycleTransactionPort lifecycle) noexcept
    : m_content(content)
    , m_world(world)
    , m_presentation(presentation)
    , m_policy(policy)
    , m_lifecycle(lifecycle)
{
}

GameSessionProductionCommandResult GameSessionObjectProductionTransactions::queueProduction(
    ObjectId producer,
    PlayerId player,
    container::StringView productTemplate,
    uint32_t sourceSequence,
    uint64_t confirmedTick)
{
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame || confirmedTick != m_presentation.m_confirmedTick)
    {
        return productionCommandResult({
            .accepted = false,
            .rejection = ObjectProductionRejectionReason::InvalidConfirmedTick,
        });
    }
    const container::SharedPtr<const game::ObjectArchetype> product =
        m_content.m_contentSnapshot.findObjectArchetype(productTemplate);
    if (!product)
    {
        return productionCommandResult({
            .accepted = false,
            .rejection = ObjectProductionRejectionReason::ProductNotFound,
        });
    }
    bool ignoreBuildPrerequisites = false;
    if (!m_policy || !m_policy.admitsObjectBuildability(player, *product, ignoreBuildPrerequisites))
    {
        return productionCommandResult({
            .accepted = false,
            .rejection = ObjectProductionRejectionReason::ProductNotAvailable,
        });
    }
    return productionCommandResult(
        m_world.m_objectProduction.queueUnit(m_world.m_registry,
                                             m_world.m_objects,
                                             m_content.m_players,
                                             m_content.m_contentSnapshot,
                                             m_presentation.m_scriptCommandBarOverrides,
                                             producer,
                                             player,
                                             product,
                                             confirmedTick,
                                             sourceSequence,
                                             static_cast<uint32_t>(std::max(1, m_content.m_startInfo.gameSpeedFPS)),
                                             m_content.m_objectSimulationRules.energy,
                                             ignoreBuildPrerequisites));
}

bool GameSessionObjectProductionTransactions::admitsBuildability(PlayerId player,
                                                                 const game::ObjectArchetype& product,
                                                                 bool& ignorePrerequisites) const noexcept
{
    return m_policy && m_policy.admitsObjectBuildability(player, product, ignorePrerequisites);
}

GameSessionProductionCommandResult GameSessionObjectProductionTransactions::queuePlayerUpgrade(
    ObjectId producer,
    PlayerId player,
    container::StringView upgradeName,
    uint32_t sourceSequence,
    uint64_t confirmedTick,
    ObjectUpgradeProductionAdmission admission)
{
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame || confirmedTick != m_presentation.m_confirmedTick)
    {
        return productionCommandResult({
            .accepted = false,
            .rejection = ObjectProductionRejectionReason::InvalidConfirmedTick,
        });
    }
    const UpgradeCatalog* catalog = m_content.m_contentSnapshot.upgradeCatalog();
    const UpgradeDefinition* upgrade = catalog ? catalog->find(upgradeName) : nullptr;
    if (!upgrade)
    {
        return productionCommandResult({
            .accepted = false,
            .rejection = ObjectProductionRejectionReason::UpgradeNotFound,
        });
    }
    return productionCommandResult(m_world.m_objectProduction.queuePlayerUpgrade(
        m_world.m_registry,
        m_world.m_objects,
        m_content.m_players,
        m_content.m_contentSnapshot,
        m_presentation.m_scriptCommandBarOverrides,
        producer,
        player,
        *upgrade,
        confirmedTick,
        sourceSequence,
        static_cast<uint32_t>(std::max(1, m_content.m_startInfo.gameSpeedFPS)),
        admission));
}

GameSessionProductionCommandResult GameSessionObjectProductionTransactions::cancelProduction(ObjectId producer,
                                                                                             PlayerId player,
                                                                                             uint32_t productionId,
                                                                                             uint64_t confirmedTick)
{
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame || confirmedTick != m_presentation.m_confirmedTick)
    {
        return productionCommandResult({
            .accepted = false,
            .rejection = ObjectProductionRejectionReason::InvalidConfirmedTick,
        });
    }
    return productionCommandResult(m_world.m_objectProduction.cancelUnit(
        m_world.m_registry, m_world.m_objects, m_content.m_players, producer, player, productionId));
}

GameSessionProductionCommandResult GameSessionObjectProductionTransactions::cancelPlayerUpgrade(
    ObjectId producer, PlayerId player, container::StringView upgradeName, uint64_t confirmedTick)
{
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame || confirmedTick != m_presentation.m_confirmedTick)
    {
        return productionCommandResult({
            .accepted = false,
            .rejection = ObjectProductionRejectionReason::InvalidConfirmedTick,
        });
    }
    const UpgradeCatalog* catalog = m_content.m_contentSnapshot.upgradeCatalog();
    const UpgradeDefinition* upgrade = catalog ? catalog->find(upgradeName) : nullptr;
    if (!upgrade)
    {
        return productionCommandResult({
            .accepted = false,
            .rejection = ObjectProductionRejectionReason::UpgradeNotFound,
        });
    }
    return productionCommandResult(m_world.m_objectProduction.cancelPlayerUpgrade(
        m_world.m_registry, m_world.m_objects, m_content.m_players, producer, player, upgrade->id));
}

GameSessionProductionCommandResult GameSessionObjectProductionTransactions::setFactoryRallyPoint(
    ObjectId producer, PlayerId player, LogicFixedVec3 position, uint64_t confirmedTick)
{
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame || confirmedTick != m_presentation.m_confirmedTick)
    {
        return productionCommandResult({
            .accepted = false,
            .rejection = ObjectProductionRejectionReason::InvalidConfirmedTick,
        });
    }
    return productionCommandResult(
        m_world.m_objectProduction.setRallyPoint(m_world.m_registry,
                                                 m_world.m_objects,
                                                 producer,
                                                 player,
                                                 {.x = position.x, .y = position.y, .z = position.z, .exists = true}));
}

bool GameSessionObjectProductionTransactions::cancelConstruction(
    ObjectId object, PlayerId player, uint64_t confirmedTick) {
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick || !object ||
        m_world.m_objects.isPendingDestroy(object) ||
        m_world.m_ownership.ownerOf(object) !=
            std::optional<PlayerId>{player}) {
        return false;
    }
    const std::optional<ecs::entity> entity =
        m_world.m_objects.entityFromId(object);
    if (!entity) return false;
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(m_world.m_registry, *entity);
    const game::ObjectStatusMask underConstruction =
        game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction);
    if (!status || !status->hasAny(underConstruction)) return false;
    const bool reconstructing = status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::Reconstructing));
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity);
    const PlayerState* payer = m_content.m_players.get(player);
    if (!type || !type->archetype || !payer) return false;
    const int64_t refund = reconstructing
        ? 0
        : std::max<int64_t>(0, calculateObjectBuildCost(
              *type->archetype, *payer, m_world.m_registry,
              m_world.m_objects));
    // RefCode implements cancellation as Object::kill(), not as a silent
    // lifecycle removal. Submit the equivalent authoritative kill through the
    // normal Body/Die walk so authored DestroyDie/CreateObjectDie/FX handlers
    // still produce the construction's destruction presentation. Under-
    // construction guards in those handlers suppress effects that should not
    // fire for a cancelled foundation (for example death weapons).
    GameSessionObjectDamageTransactions damageTransactions{
        m_content, m_world, m_presentation, m_lifecycle};
    if (!damageTransactions.queueObjectDamage({
            .target = object,
            .source = INVALID_OBJECT_ID,
            .damageType = game::DamageType::UNRESISTABLE,
            .deathType = game::DeathType::NORMAL,
            .forceKill = true,
            .confirmedTick = confirmedTick})) {
        return false;
    }
    if (refund != 0) {
        static_cast<void>(m_content.m_players.adjustCash(player, refund));
    }
    damageTransactions.resolveQueuedObjectDamage();
    return true;
}

bool GameSessionObjectProductionTransactions::advanceConfirmedProduction() {
    container::Vector<ObjectProductionSpawnIntent> productionSpawns;
    container::Vector<ObjectProductionUpgradeCompletionIntent>
        productionUpgrades;
    m_world.m_objectProduction.update(
        m_world.m_registry, m_world.m_objects, m_content.m_players,
        m_content.m_contentSnapshot, m_content.m_terrain,
        m_presentation.m_confirmedTick,
        static_cast<uint32_t>(
            std::max(1, m_content.m_startInfo.gameSpeedFPS)),
        m_content.m_objectSimulationRules.energy,
        productionSpawns, productionUpgrades);

    const bool staged =
        !productionSpawns.empty() || !productionUpgrades.empty();
    auto& pendingSpawns = m_world.m_pendingProductionSpawns;
    auto& pendingUpgrades = m_world.m_pendingProductionUpgrades;
    pendingSpawns.reserve(pendingSpawns.size() + productionSpawns.size());
    pendingUpgrades.reserve(
        pendingUpgrades.size() + productionUpgrades.size());

    size_t spawnIndex = 0;
    size_t upgradeIndex = 0;
    while (spawnIndex < productionSpawns.size() ||
           upgradeIndex < productionUpgrades.size()) {
        if (upgradeIndex < productionUpgrades.size() &&
            (spawnIndex == productionSpawns.size() ||
             productionUpgrades[upgradeIndex].producer <
                 productionSpawns[spawnIndex].producer)) {
            ObjectProductionUpgradeCompletionIntent& intent =
                productionUpgrades[upgradeIndex++];
            intent.submissionOrdinal =
                m_world.m_objectSimulation.reserveGameplaySubmissionOrdinal();
            intent.confirmedTick = m_presentation.m_confirmedTick;
            pendingUpgrades.push_back(std::move(intent));
        } else {
            ObjectProductionSpawnIntent& intent =
                productionSpawns[spawnIndex++];
            intent.submissionOrdinal =
                m_world.m_objectSimulation.reserveGameplaySubmissionOrdinal();
            intent.confirmedTick = m_presentation.m_confirmedTick;
            pendingSpawns.push_back(std::move(intent));
        }
    }
    return staged;
}

} // namespace engine

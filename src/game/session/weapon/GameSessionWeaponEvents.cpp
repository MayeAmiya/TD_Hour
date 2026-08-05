#include "game/session/core/GameSession.h"
#include "game/session/state/GameSessionDomainState.h"

#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"
#include "game/session/weapon/GameSessionGameplayTransactionDrain.h"

namespace engine {

void detail::GameSessionGameplayTransactionDrain::run(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionAIState& ai,
    GameSessionScriptPresentationState& presentation,
    GameSessionObjectEventState& objectEvents,
    GameSessionFrameCommitState& frame,
    GameSessionLifecycleTransactionPort lifecycle) {
    if (content.m_drainingGameplayWork) return;
    GameSessionGameplayPublicationPort publication{
        content, world, presentation, frame};
    GameSessionObjectLifecycleTransactions objectLifecycle{
        content, world, presentation, lifecycle, &objectEvents};
    detail::GameSessionWeaponEventDrain drain{
        {
            .content = content,
            .world = world,
            .ai = ai,
            .presentation = presentation,
            .objectEvents = objectEvents,
        },
        {
            .lifecycle = objectLifecycle,
            .ownership = GameSessionObjectOwnershipTransactions{
                content, world, ai, presentation, lifecycle, &publication},
            .publication = publication,
            .frame = GameSessionFramePort{
                content, world, presentation, objectEvents, frame},
            .navigation = GameSessionNavigationTransactions{
                content, presentation},
            .navigationFootprints =
                GameSessionNavigationFootprintTransactions{
                    content, world, presentation, frame},
            .targetRemap = GameSessionObjectTargetRemapTransactions{
                world},
            .weaponEvents = GameSessionWeaponEventPublisher{
                content, world, presentation, publication},
            .projectiles = GameSessionProjectileSpawnTransactions{
                content, world, objectLifecycle},
            .bridges = GameSessionBridgeLifecycleTransactions{
                content, world, presentation, lifecycle,
                GameSessionObjectDamageTransactions{
                    content, world, presentation, lifecycle}},
            .deletePostamble = GameSessionDeletePostambleTransactions{
                content, world, presentation, objectEvents,
                GameSessionNavigationTransactions{
                    content, presentation},
                publication},
            .healthEvents = GameSessionHealthEventPublisher{
                content, world, ai, presentation, objectEvents,
                publication},
        }};
    struct DrainGuard final {
        bool& active;
        detail::GameSessionWeaponEventDrain*& drain;
        ~DrainGuard() {
            drain = nullptr;
            active = false;
        }
    } guard{content.m_drainingGameplayWork, content.m_gameplayDrain};
    content.m_drainingGameplayWork = true;
    content.m_gameplayDrain = &drain;
    drain.run();
}

} // namespace engine

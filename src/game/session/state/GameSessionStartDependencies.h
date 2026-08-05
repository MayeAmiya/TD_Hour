#pragma once

#include "core/container/container_types.h"

#include "game/object/ai/runtime/ObjectAIRuntime.h"
#include "game/data/base/ObjectSimulationRules.h"
#include "game/player/MatchSetup.h"

#include <cstdint>
#include <optional>

namespace engine {

class MultiplayerRuleset;
class ScienceCatalog;
class UpgradeCatalog;

// Immutable content required to create a match. GameStartInfo remains the
// launcher/lobby descriptor; this value is the sealed content handoff owned by
// the application composition root.
struct GameSessionStartDependencies final {
    container::SharedPtr<const MultiplayerRuleset> ruleset;
    container::SharedPtr<const ScienceCatalog> scienceCatalog;
    container::SharedPtr<const UpgradeCatalog> upgradeCatalog;
    uint64_t simulationContentFingerprint = 0;
    std::optional<ResolvedMatchSetup> resolvedMatchSetup;
    bool allowReplayModeOverlay = false;
    ObjectSimulationRules objectSimulationRules;
    ai::ObjectAIRuntimeConfig objectAIRuntimeConfig{
        .maximumActors = 65536,
        .slotsPerBatch = 256,
        .membershipEventCapacity = 262144,
        .transientValueCapacity = 262144,
    };
};

} // namespace engine

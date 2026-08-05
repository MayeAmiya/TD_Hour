#pragma once

#include "core/container/container_types.h"

#include "game/data/base/EnergySimulationRules.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/contracts/ObjectDisabledTypes.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <optional>
namespace game {

struct ThingTemplate;

// One authored QuantityModifier entry. It changes only the number of units
// emitted by one paid production job; price and build time still belong to the
// requested unit template and the session's later modifier calculator.
struct ObjectProductionQuantityModifier final {
    container::String templateName;
    uint32_t quantity = 1;
};

// Immutable projection of the single legacy ProductionUpdate host. Door
// timings are retained now for future model-condition presentation, while P1
// factory logic consumes queue capacity and QuantityModifier immediately.
struct ObjectProductionPlan final {
    // The original nine-entry value matched the nine physical ControlBar
    // queue widgets.  Presentation now groups consecutive equal jobs (A3,
    // B2, A3), so an omitted MaxQueueEntries must no longer turn that visual
    // limit into a gameplay limit. Explicit authored values such as 1 or 2
    // still override this default in the compiler.
    static constexpr uint32_t DefaultMaxQueueEntries = 255;

    uint32_t authoredOrder = 0;
    uint32_t maxQueueEntries = DefaultMaxQueueEntries;
    uint32_t numberOfDoorAnimations = 0;
    uint32_t doorOpeningMilliseconds = 0;
    uint32_t doorWaitOpenMilliseconds = 0;
    uint32_t doorClosingMilliseconds = 0;
    // ProductionUpdate's temporary CONSTRUCTION_COMPLETE model condition
    // after a product exits.  Construction-scaffold teardown is a Draw
    // TransitionState and must use its W3D duration plus the authored
    // AnimationSpeedFactorRange instead of this timer.
    uint32_t constructionCompleteMilliseconds = 0;
    // RefCode defaults ProductionUpdate to process HELD. Authored
    // DisabledTypesToProcess replaces this mask at recipe compile time.
    engine::ObjectDisabledMask disabledTypesToProcess =
        engine::objectDisabledBit(engine::ObjectDisabledReason::Held);
    container::Vector<ObjectProductionQuantityModifier> quantityModifiers;
    container::Vector<container::String> diagnostics;
};

enum class ObjectProductionExitKind : uint8_t {
    Default,
    Queue,
    SpawnPoint,
    SupplyCenter,
    AirfieldParking,
    FlightDeck,
};

// Immutable projection of the first authored legacy ExitInterface host.
// Common coordinates stay in local space; the kind-specific suffix preserves
// exactly the data consumed by Queue/SpawnPoint/SupplyCenter without keeping
// a legacy module pointer or reparsing INI during a confirmed frame.
struct ObjectProductionExitPlan final {
    uint32_t authoredOrder = 0;
    ObjectProductionExitKind kind = ObjectProductionExitKind::Default;
    math::q32_32 unitCreatePointX{};
    math::q32_32 unitCreatePointY{};
    math::q32_32 unitCreatePointZ{};
    math::q32_32 naturalRallyPointX{};
    math::q32_32 naturalRallyPointY{};
    math::q32_32 naturalRallyPointZ{};
    uint32_t exitDelayMilliseconds = 0;
    uint32_t initialBurst = 0;
    uint32_t grantTemporaryStealthMilliseconds = 0;
    container::String spawnPointBoneName;
    bool allowAirborneCreation = false;
    bool useSpawnRallyPoint = false;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectProductionPlan>
compileObjectProductionPlan(const ThingTemplate& templateData);

[[nodiscard]] container::SharedPtr<const ObjectProductionExitPlan>
compileObjectProductionExitPlan(const ThingTemplate& templateData);

} // namespace game

namespace game::terrain {
class TerrainLogic;
}

namespace game {
class CommandBarOverrideState;
}

#pragma once

#include "core/container/container_types.h"

#include "core/ecs/ObjectId.h"

#include <cstdint>
#include <optional>
namespace game {

struct ThingTemplate;

// LifetimeUpdate kills through Body/Die; DeletionUpdate asks GameLogic to
// remove the object directly. The identical timing syntax is compiled once
// into this common value plan rather than becoming two legacy module classes.
enum class ObjectLifetimeAction : uint8_t {
    Kill,
    Destroy,
};

struct ObjectLifetimeRule final {
    uint32_t authoredOrder = 0;
    uint32_t minimumLifetimeMilliseconds = 0;
    uint32_t maximumLifetimeMilliseconds = 0;
    // Stable content identity participates in the counter PRF. It prevents
    // a same-order third-party replacement from silently inheriting another
    // module's random duration.
    uint64_t stableRuleKey = 0;
    ObjectLifetimeAction action = ObjectLifetimeAction::Kill;
};

struct ObjectLifetimePlan final {
    container::Vector<ObjectLifetimeRule> rules;
    // Typed sub-compilers report load-stopping recipe errors here. The owning
    // ThingFactory promotes them to ObjectRecipeDiagnostic so malformed timer
    // text cannot silently become a zero-frame self-destruct at runtime.
    container::Vector<container::String> diagnostics;
};

// Compiles the resolved final recipe. A null result means that neither
// LifetimeUpdate nor DeletionUpdate exists on the object.
[[nodiscard]] container::SharedPtr<const ObjectLifetimePlan>
compileObjectLifetimePlan(const ThingTemplate& templateData);

} // namespace game


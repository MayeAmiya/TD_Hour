#pragma once

#include "core/container/container_types.h"

#include <cstdint>
#include <optional>
namespace game {

// The reference engine registered module classes in three separate factory
// domains.  The modern content compiler keeps that fact as immutable data;
// it does not recreate the old ModuleFactory or its virtual Module objects.
enum class ObjectModuleCatalogDomain : uint8_t {
    Behavior,
    ClientUpdate,
};

// Typed owner of a module's Object::onDie callback. `None` is required for
// modules without the Die interface; `Unknown` is reserved for opaque Mod
// modules outside the stock catalog.
enum class ObjectOnDieHandlerKind : uint8_t {
    None,
    DeathReaction,
    Bridge,
    Airfield,
    Production,
    Spawn,
    TechBuilding,
    PropagandaTower,
    Containment,
    Minefield,
    NeutronMissile,
    Unknown,
    Count,
};

struct ObjectModuleCatalogEntry final {
    uint32_t interfaceMask = 0;
    ObjectModuleCatalogDomain domain = ObjectModuleCatalogDomain::Behavior;
    ObjectOnDieHandlerKind onDieHandler = ObjectOnDieHandlerKind::None;
    bool isAiModule = false;
};

// Looks up a stock Generals/Zero Hour module class by its authored spelling.
// Unknown third-party classes deliberately return nullopt: callers preserve
// their raw recipe as opaque data instead of inventing an interface mask.
[[nodiscard]] std::optional<ObjectModuleCatalogEntry>
findObjectModuleCatalogEntry(container::StringView moduleClass) noexcept;

// Stock declarations deliberately excluded from the modern object-module
// inventory. They are accepted only so unmodified legacy INI remains loadable,
// then discarded before recipe/interface compilation.
[[nodiscard]] bool isIgnoredLegacyObjectModule(
    container::StringView moduleClass) noexcept;

} // namespace game

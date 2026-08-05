#pragma once

#include "core/container/hash_containers.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/definition/ObjectKindOf.h"
#include "math/fixed/q32_32.h"

#include <optional>

namespace game {

struct CrateObjectChoice final {
    container::String objectTemplate;
    math::q32_32 chance{};
};

// Immutable projection of one legacy CrateData block. Conditions remain
// simulation values; the eventual object spawn is still owned by GameSession.
struct CrateTemplateDefinition final {
    container::String name;
    math::q32_32 creationChance{};
    std::optional<ObjectVeterancyLevel> veterancyLevel;
    container::Vector<container::String> killedByKinds;
    ObjectKindOfMask killedByKindMask{};
    container::String killerScience;
    container::Vector<CrateObjectChoice> possibleCrates;
    bool ownedByMaker = false;
};

class CrateTemplateCatalog final {
public:
    [[nodiscard]] static container::Vector<container::String>
    enumerateVfsLoadFiles(
        container::Span<const container::StringView> loadRoots);

    [[nodiscard]] bool loadFromVfsFiles(
        const container::Vector<container::String>& logicalFiles,
        container::String* error = nullptr);
    // Applies one INI_LOAD_CREATE_OVERRIDES source to an already loaded
    // private catalog. Existing definitions are copied before sparse fields
    // are parsed, matching RefCode's Overridable chain semantics.
    [[nodiscard]] bool applyOverridesFromVfs(
        container::StringView path, container::String* error = nullptr);
    [[nodiscard]] bool loadFromVfsLoadDirectories(
        container::Span<const container::StringView> loadRoots,
        container::String* error = nullptr);
    void clear() noexcept;

    [[nodiscard]] bool isLoaded() const noexcept { return m_loaded; }
    [[nodiscard]] size_t size() const noexcept { return m_definitions.size(); }
    [[nodiscard]] const CrateTemplateDefinition* find(
        container::StringView name) const noexcept;
    [[nodiscard]] const container::Vector<CrateTemplateDefinition>&
    definitions() const noexcept { return m_definitions; }

private:
    container::Vector<CrateTemplateDefinition> m_definitions;
    container::HashMap<container::String, size_t> m_indices;
    bool m_loaded = false;
};

} // namespace game

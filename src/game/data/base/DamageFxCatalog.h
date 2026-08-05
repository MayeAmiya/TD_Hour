#pragma once

#include "core/container/hash_containers.h"
#include "game/base/DamageTypes.h"
#include "game/base/ObjectVeterancy.h"
#include "math/fixed/q32_32.h"

#include <cstddef>
#include <cstdint>

namespace engine::fx {
class FxListCatalog;
}

namespace game {

struct DamageFxContentId final {
    uint32_t value = 0;

    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    friend bool operator==(DamageFxContentId, DamageFxContentId) = default;
};

// DamageFX.ini does not author a separate sound field.  Both particles and
// sounds are selected through an FXList; DamageFxCatalog therefore retains
// the symbolic FXList reference and freezes a transitive Sound-nugget
// projection when resolveFxReferences() is called.
struct DamageFxEffectReference final {
    container::String fxListName;

    [[nodiscard]] bool empty() const noexcept { return fxListName.empty(); }
};

struct DamageFxRule final {
    // Authoring may arrive as a decimal INI token, but the catalog is the
    // session-facing content boundary.  Quantize once while loading so hit
    // resolution never projects authoritative damage back through float.
    math::q32_32 amountForMajorFx{};
    DamageFxEffectReference minor;
    DamageFxEffectReference major;
    // ZH authors this as an integer millisecond duration. The confirmed
    // session converts it upward to its own logic-tick rate when O-002
    // creates per-object throttle state.
    uint32_t throttleTimeMilliseconds = 0;
};

struct DamageFxDefinition final {
    static constexpr size_t kDamageTypeCount =
        static_cast<size_t>(DamageType::COUNT);
    static constexpr size_t kVeterancyLevelCount = 4;

    DamageFxContentId id;
    container::String name;
    container::Array<
        container::Array<DamageFxRule, kVeterancyLevelCount>,
        kDamageTypeCount> rules;

    [[nodiscard]] const DamageFxRule* findRule(
        DamageType damageType, ObjectVeterancyLevel veterancy) const noexcept;
};

// Immutable-after-load projection of RefCode DamageFXStore.  Definitions are
// expanded to DamageType x Veterancy once at content-load time, so the future
// hit runtime performs two bounded array indexes rather than interpreting INI
// inheritance or hashing field names.
class DamageFxCatalog final {
public:
    [[nodiscard]] static container::Vector<container::String>
    enumerateVfsLoadFiles(
        container::Span<const container::StringView> loadRoots);

    [[nodiscard]] bool loadFromVfsFiles(
        const container::Vector<container::String>& logicalFiles,
        container::String* error = nullptr);
    // Applies one later CreateOverrides source to an already loaded private
    // catalog. RefCode replaces a repeated DamageFX table wholesale while
    // retaining every definition not named by the modifier.
    [[nodiscard]] bool applyOverridesFromVfs(
        container::StringView path, container::String* error = nullptr);
    [[nodiscard]] bool loadFromVfsLoadDirectories(
        container::Span<const container::StringView> loadRoots,
        container::String* error = nullptr);
    [[nodiscard]] bool loadFromText(
        container::StringView content,
        container::StringView sourceName = "<memory>",
        container::String* error = nullptr);

    // Resolves the Sound nuggets carried by referenced FXLists, including
    // nested FXListAtBonePos chains. The FXList names remain symbolic because
    // GameSession emits typed presentation events rather than owning renderer
    // handles.
    void resolveFxReferences(const engine::fx::FxListCatalog& fxLists);
    void clear() noexcept;

    [[nodiscard]] bool isLoaded() const noexcept { return m_loaded; }
    [[nodiscard]] size_t size() const noexcept { return m_definitions.size(); }
    [[nodiscard]] const DamageFxDefinition* find(
        container::StringView name) const noexcept;
    [[nodiscard]] const DamageFxDefinition* find(
        DamageFxContentId id) const noexcept;
    [[nodiscard]] DamageFxContentId findId(
        container::StringView name) const noexcept;
    [[nodiscard]] const DamageFxRule* findRule(
        container::StringView name, DamageType damageType,
        ObjectVeterancyLevel veterancy) const noexcept;
    [[nodiscard]] const DamageFxRule* findRule(
        DamageFxContentId id, DamageType damageType,
        ObjectVeterancyLevel veterancy) const noexcept;
    [[nodiscard]] const DamageFxEffectReference* selectEffect(
        container::StringView name, DamageType damageType,
        ObjectVeterancyLevel veterancy,
        math::q32_32 damageAmount) const noexcept;
    [[nodiscard]] const DamageFxEffectReference* selectEffect(
        DamageFxContentId id, DamageType damageType,
        ObjectVeterancyLevel veterancy,
        math::q32_32 damageAmount) const noexcept;
    [[nodiscard]] container::Span<const container::String> soundEvents(
        const DamageFxEffectReference& effect) const noexcept;

    [[nodiscard]] const container::Vector<DamageFxDefinition>&
    definitions() const noexcept { return m_definitions; }
    [[nodiscard]] const container::Vector<container::String>&
    diagnostics() const noexcept { return m_diagnostics; }

private:
    [[nodiscard]] bool appendParsedLayer(
        container::StringView content, container::StringView sourceName,
        container::String* error);
    void sealDefinitions();

    container::Vector<DamageFxDefinition> m_definitions;
    container::HashMap<container::String, size_t> m_indices;
    container::HashMap<container::String, container::Vector<container::String>>
        m_soundEventsByFxList;
    container::Vector<container::String> m_diagnostics;
    bool m_loaded = false;
};

} // namespace game

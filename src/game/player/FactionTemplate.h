#pragma once

#include "core/container/container_types.h"

#include "PlayerTypes.h"
#include "game/base/GameBalanceConstants.h"
#include <cstdint>
#include <optional>
namespace engine {

// Stored as bytes rather than a renderer color type: rules data must remain
// usable by headless simulation and canonical serialization.
struct PlayerRgbColor final {
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;

    constexpr auto operator<=>(const PlayerRgbColor&) const noexcept = default;
};

inline constexpr int32_t kBasisPointsPerWhole = 10'000;

// 10000 means 100%.  A fixed integer representation keeps future cost/time
// calculation deterministic instead of inheriting floating-point percent
// parsing from the original runtime Player object.
struct ProductionPercentModifier final {
    container::String thingTemplateName;
    int32_t multiplierBasisPoints = kBasisPointsPerWhole;
};

struct ProductionVeterancyModifier final {
    container::String thingTemplateName;
    // Kept as canonical authored data until the veterancy system owns a typed
    // enum and validates all legacy/mod names.
    container::String veterancyName;
};

struct PlayerTemplateSimulationData final {
    int32_t startingMoney = 0;
    PlayerRgbColor preferredColor{};
    container::String startingBuilding;
    container::Array<container::String, 10> startingUnits{};
    container::Vector<ProductionPercentModifier> productionCostModifiers;
    container::Vector<ProductionPercentModifier> productionTimeModifiers;
    container::Vector<ProductionVeterancyModifier> productionVeterancyModifiers;
    container::Vector<container::String> intrinsicSciences;
    int32_t intrinsicSciencePurchasePoints = 0;
};

// Presentation names stay in rules data so UI/audio can resolve the original
// PlayerTemplate completely, but they are deliberately segregated from the
// values that affect lockstep simulation.
struct PlayerTemplatePresentationData final {
    container::String displayName;
    container::Array<container::String, 3> purchaseScienceCommandSets{};
    container::String specialPowerShortcutCommandSet;
    container::String specialPowerShortcutWindow;
    int32_t specialPowerShortcutButtonCount = 0;
    container::String scoreScreenImage;
    container::String loadScreenImage;
    container::String loadScreenMusic;
    container::String scoreScreenMusic;
    container::String headWaterMark;
    container::String flagWaterMark;
    container::String enabledImage;
    container::String sideIconImage;
    container::String generalImage;
    container::String beaconTemplate;
    container::String armyTooltip;
    container::String features;
    container::String medallionRegular;
    container::String medallionHilite;
    container::String medallionSelect;
};

struct TemplateSourceInfo final {
    container::String path;
    uint32_t layer = 0;
};

// Unknown/extension fields are intentionally retained rather than discarded.
// This gives a mod author a diagnostic surface and prevents the loader from
// pretending that an unsupported legacy field never existed.
struct TemplateExtensionField final {
    container::String key;
    container::String value;
    TemplateSourceInfo source;
};

struct FactionTemplate final {
    FactionTemplateId id = INVALID_FACTION_TEMPLATE_ID;
    container::String name;
    container::String side;
    container::String baseSide;
    bool playable = false;
    bool observer = false;
    bool oldFaction = false;
    uint32_t authoredOrder = 0;
    container::Vector<TemplateSourceInfo> sources;
    PlayerTemplateSimulationData simulation;
    PlayerTemplatePresentationData presentation;
    container::Vector<TemplateExtensionField> extensionFields;
};

struct MultiplayerColorDefinition final {
    MultiplayerColorId id = INVALID_MULTIPLAYER_COLOR_ID;
    container::String name;
    container::String tooltipName;
    PlayerRgbColor day{};
    PlayerRgbColor night{};
    uint32_t authoredOrder = 0;
    container::Vector<TemplateSourceInfo> sources;
};

struct NamedRgbSetting final {
    container::String name;
    PlayerRgbColor color{};
    TemplateSourceInfo source;
};

struct MultiplayerRules final {
    int32_t startCountdownSeconds = 5;
    int32_t maxBeaconsPerPlayer = 3;
    bool useShroud = false;
    bool showRandomPlayerTemplate = true;
    bool showRandomStartPosition = true;
    bool showRandomColor = true;
    container::Vector<int32_t> startingMoneyChoices;
    int32_t defaultStartingMoney = DEFAULT_TEMPLATE_STARTING_CASH;
    container::Vector<MultiplayerColorDefinition> colors;
    container::Vector<NamedRgbSetting> onlineChatColors;
    container::Vector<TemplateExtensionField> extensionFields;
};

// Frozen, immutable content snapshot used by both front-end adapters and a
// match resolver.  It is a value object, not a process-global gameplay
// singleton; a GameSession can retain a shared immutable snapshot later.
class MultiplayerRuleset final {
public:
    bool loadFromVfs(container::StringView multiplayerPath,
                     container::StringView playerTemplatePath,
                     container::String* error = nullptr);
    // Applies the PlayerTemplate, MultiplayerColor and MultiplayerSettings
    // blocks from a map/session INI as ZH CreateOverrides.  The current
    // ruleset remains unchanged if the source cannot be parsed or applied.
    // Files containing none of those blocks are a successful no-op.
    [[nodiscard]] bool applyOverridesFromVfs(container::StringView path,
                                             container::String* error = nullptr);
    // Degraded-content fallback: publishes a valid immutable empty value
    // object without inventing factions, colors or economy definitions.
    // Session setup may still reject an unavailable requested faction, but
    // content loading itself remains observable and non-fatal.
    void sealEmpty();
    void clear();

    [[nodiscard]] bool isLoaded() const noexcept { return m_loaded; }
    // Used for lockstep, replay and resolved-match compatibility. It contains
    // only values that current simulation/resolution can observe.
    [[nodiscard]] uint64_t simulationFingerprint() const noexcept { return m_simulationFingerprint; }
    // Diagnostic/content identity includes presentation and retained mod
    // extension data. It is useful for bug reports, but must not reject a
    // peer merely because of localization, UI or audio differences.
    [[nodiscard]] uint64_t contentFingerprint() const noexcept { return m_contentFingerprint; }
    // Source-compatible name for existing callers. Match code must call the
    // explicit simulationFingerprint() method instead.
    [[nodiscard]] uint64_t fingerprint() const noexcept { return simulationFingerprint(); }
    [[nodiscard]] const MultiplayerRules& multiplayer() const noexcept { return m_multiplayer; }
    [[nodiscard]] const container::Vector<FactionTemplate>& factionTemplates() const noexcept {
        return m_templates;
    }
    [[nodiscard]] const container::Vector<FactionTemplateId>& playableTemplateIds() const noexcept {
        return m_playableTemplateIds;
    }
    [[nodiscard]] const container::Vector<FactionTemplateId>& templateIdsByAuthoredOrder() const noexcept {
        return m_templateIdsByAuthoredOrder;
    }
    [[nodiscard]] const container::Vector<MultiplayerColorId>& colorIdsByAuthoredOrder() const noexcept {
        return m_colorIdsByAuthoredOrder;
    }

    [[nodiscard]] const FactionTemplate* findFaction(FactionTemplateId id) const noexcept;
    [[nodiscard]] const FactionTemplate* findFaction(container::StringView name) const;
    [[nodiscard]] const MultiplayerColorDefinition* findColor(MultiplayerColorId id) const noexcept;
    [[nodiscard]] const MultiplayerColorDefinition* findColor(container::StringView name) const;
    [[nodiscard]] std::optional<FactionTemplateId> playableTemplateIdAt(size_t authoredIndex) const noexcept;
    [[nodiscard]] std::optional<MultiplayerColorId> colorIdAt(size_t authoredIndex) const noexcept;

private:
    void rebuildDerivedState();

    container::Vector<FactionTemplate> m_templates;
    container::Vector<FactionTemplateId> m_playableTemplateIds;
    container::Vector<FactionTemplateId> m_templateIdsByAuthoredOrder;
    container::Vector<MultiplayerColorId> m_colorIdsByAuthoredOrder;
    MultiplayerRules m_multiplayer;
    uint64_t m_simulationFingerprint = 0;
    uint64_t m_contentFingerprint = 0;
    bool m_loaded = false;
};

} // namespace engine

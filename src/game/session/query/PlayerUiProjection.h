#pragma once

#include "core/container/container_types.h"
#include "game/session/query/InGameCommandProjection.h"
#include "game/session/query/InGameCommandQuerySource.h"
#include "game/session/query/GameSessionCommandQueryPort.h"
#include "game/session/query/GameSessionEconomyQueryPort.h"
#include "game/session/query/GameSessionRulesetQueryPort.h"
#include "presentation/ui/MappedImageContentLayer.h"
#include "presentation/ui/MapStringContentLayer.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace engine {
class GameSessionContentStartState;
}

namespace engine::session_query {

struct SessionUiContentProjection final {
    container::SharedPtr<const container::Vector<ui::MappedImageContentLayer>>
        mappedImageLayers;
    container::SharedPtr<const ui::MapStringContentLayer> mapStrings;
};

struct PlayerPowerProjection final {
    uint64_t revision = 0;
    int32_t production = 0;
    int32_t consumption = 0;
    bool sufficient = true;
};

// Read-only HUD value.  The authoritative balance remains in PlayerRegistry;
// the presentation thread receives only this copied, revisioned value.
struct PlayerMoneyProjection final {
    uint64_t revision = 0;
    int64_t cash = 0;
};

struct SciencePurchaseButtonProjection final {
    uint64_t buttonStableId = 0;
    container::String commandButtonName;
    container::String science;
    container::String buttonImage;
    container::String textLabel;
    container::String descriptionLabel;
    bool visible = false;
    bool enabled = false;
    bool acquired = false;
    int32_t cost = 0;

    friend bool operator==(const SciencePurchaseButtonProjection&,
                           const SciencePurchaseButtonProjection&) = default;
};

struct SciencePurchaseProjection final {
    static constexpr size_t kRank1Count = 4;
    static constexpr size_t kRank3Count = 15;
    static constexpr size_t kRank8Count = 4;

    uint64_t revision = 0;
    bool available = false;
    int32_t purchasePoints = 0;
    int32_t rankLevel = 1;
    float rankProgress = 0.0f;
    container::Array<SciencePurchaseButtonProjection, kRank1Count> rank1{};
    container::Array<SciencePurchaseButtonProjection, kRank3Count> rank3{};
    container::Array<SciencePurchaseButtonProjection, kRank8Count> rank8{};
};

struct SpecialPowerShortcutButtonProjection final {
    uint64_t buttonStableId = 0;
    container::String commandButtonName;
    container::String buttonImage;
    container::String textLabel;
    container::String descriptionLabel;
    ObjectId sourceObject = INVALID_OBJECT_ID;
    InGameCommandSlotAvailability availability;
    uint16_t availableSourceCount = 0;

    friend bool operator==(const SpecialPowerShortcutButtonProjection&,
                           const SpecialPowerShortcutButtonProjection&) = default;
};

struct SpecialPowerShortcutProjection final {
    uint64_t revision = 0;
    bool available = false;
    container::String windowName;
    container::Vector<SpecialPowerShortcutButtonProjection> buttons;
};

class PlayerUiQueryPort final {
public:
    PlayerUiQueryPort(
        const GameSessionContentStartState& content,
        InGameCommandQuerySource source,
        GameSessionCommandQueryPort commands,
        GameSessionEconomyQueryPort economy,
        GameSessionRulesetQueryPort ruleset,
        uint64_t confirmedTick,
        uint32_t logicFramesPerSecond) noexcept
        : m_content(&content), m_source(std::move(source)),
          m_commands(std::move(commands)), m_economy(std::move(economy)),
          m_ruleset(std::move(ruleset)), m_confirmedTick(confirmedTick),
          m_logicFramesPerSecond(logicFramesPerSecond) {}

    [[nodiscard]] SessionUiContentProjection content() const noexcept;
    [[nodiscard]] PlayerPowerProjection power() const noexcept;
    [[nodiscard]] PlayerMoneyProjection money() const noexcept;
    [[nodiscard]] SciencePurchaseProjection sciencePurchase(
        uint64_t uiRevision) const;
    [[nodiscard]] SpecialPowerShortcutProjection specialPowerShortcuts(
        uint64_t uiRevision) const;

private:
    const GameSessionContentStartState* m_content = nullptr;
    InGameCommandQuerySource m_source;
    GameSessionCommandQueryPort m_commands;
    GameSessionEconomyQueryPort m_economy;
    GameSessionRulesetQueryPort m_ruleset;
    uint64_t m_confirmedTick = 0;
    uint32_t m_logicFramesPerSecond = 30;
};

} // namespace engine::session_query

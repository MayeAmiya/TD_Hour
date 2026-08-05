#include "ScoreScreenViewModel.h"
#include "StringTable.h"

#include <cctype>
#include <initializer_list>
#include <string>

namespace gui::ingame {
namespace {

void appendNumber(container::String& text, uint64_t value) {
    text += std::to_string(value);
}

[[nodiscard]] container::String localized(
    std::initializer_list<container::StringView> keys,
    container::StringView fallback) {
    const engine::StringTable& strings = engine::StringTable::instance();
    for (const container::StringView key : keys) {
        container::String value = strings.fetch(container::String{key});
        if (!value.empty()) return value;
    }
    return container::String{fallback};
}

// Several stock ScoreScreen labels are authored for narrow, multi-line
// column headers.  Collapse their layout whitespace before reusing them in
// the one-line fallback roster.
[[nodiscard]] container::String compactLabel(container::String value) {
    container::String compact;
    compact.reserve(value.size());
    bool pendingSpace = false;
    for (const unsigned char character : value) {
        if (std::isspace(character)) {
            pendingSpace = !compact.empty();
            continue;
        }
        if (pendingSpace) compact.push_back(' ');
        compact.push_back(static_cast<char>(character));
        pendingSpace = false;
    }
    return compact;
}

} // namespace

ScoreScreenViewModel ScoreScreenViewModel::fromSnapshot(
    const engine::MatchResultSnapshot& snapshot) {
    ScoreScreenViewModel model;
    model.victory = snapshot.localVictory();
    model.resultTitle = model.victory
        ? localized({"GUI:Victory", "GUI:YouAreVictorious"}, "Victory")
        : localized({"GUI:Defeat", "GUI:YouHaveBeenDefeated"}, "Defeat");
    model.backgroundImage = snapshot.localScoreScreenImage;
    model.music = snapshot.localScoreScreenMusic;

    model.missionSummary = snapshot.startInfo.mapName;
    model.missionSummary += "  |  ";
    model.missionSummary += compactLabel(localized({"GUI:Time"}, "tick"));
    model.missionSummary += " ";
    appendNumber(model.missionSummary, snapshot.confirmedTick);

    const container::String localPlayerLabel =
        compactLabel(localized({"GUI:You"}, "You"));
    const container::String earnedLabel = compactLabel(localized(
        {"GUI:ResourcesCollected", "GUI:MoneyEarned"}, "Earned"));
    const container::String spentLabel =
        compactLabel(localized({"GUI:MoneySpent"}, "Spent"));
    const container::String unitsBuiltLabel =
        compactLabel(localized({"GUI:UnitsBuilt"}, "Built U/B"));
    const container::String destroyedLabel =
        compactLabel(localized({"GUI:UnitsDestroyed"}, "Destroyed U/B"));
    const container::String lostLabel =
        compactLabel(localized({"GUI:UnitsLost"}, "Lost U/B"));

    model.playerRows.reserve(snapshot.players.size());
    model.players.reserve(snapshot.players.size());
    for (const engine::MatchResultPlayerRow& player : snapshot.players) {
        container::String line = player.displayName.empty()
            ? player.side
            : player.displayName;
        if (player.localPlayer) {
            line += " (";
            line += localPlayerLabel;
            line += ")";
        }
        line += "  |  ";
        line += earnedLabel;
        line += " ";
        appendNumber(line, player.moneyEarned);
        line += "  ";
        line += spentLabel;
        line += " ";
        appendNumber(line, player.moneySpent);
        line += "  |  ";
        line += unitsBuiltLabel;
        line += " ";
        appendNumber(line, player.unitsBuilt);
        line += "/";
        appendNumber(line, player.buildingsBuilt);
        line += "  |  ";
        line += destroyedLabel;
        line += " ";
        appendNumber(line, player.unitsDestroyed);
        line += "/";
        appendNumber(line, player.buildingsDestroyed);
        line += "  |  ";
        line += lostLabel;
        line += " ";
        appendNumber(line, player.unitsLost);
        line += "/";
        appendNumber(line, player.buildingsLost);
        model.playerRows.push_back(std::move(line));

        ScoreScreenPlayerViewModel row;
        row.name = player.displayName.empty() ? player.side : player.displayName;
        if (player.localPlayer) {
            row.name += " (";
            row.name += localPlayerLabel;
            row.name += ")";
        }
        row.unitsBuilt = std::to_string(player.unitsBuilt);
        row.unitsLost = std::to_string(player.unitsLost);
        row.unitsDestroyed = std::to_string(player.unitsDestroyed);
        row.buildingsBuilt = std::to_string(player.buildingsBuilt);
        row.buildingsLost = std::to_string(player.buildingsLost);
        row.buildingsDestroyed = std::to_string(player.buildingsDestroyed);
        row.resourcesCollected = std::to_string(player.moneyEarned);
        model.players.push_back(std::move(row));
    }
    return model;
}

} // namespace gui::ingame

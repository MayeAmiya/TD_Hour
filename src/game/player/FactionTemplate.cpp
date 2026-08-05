#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "FactionTemplate.h"

#include "core/data/ini/GeneralsIniParser.h"
#include "VFS.h"
#include "debug/debug.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <limits>
#include <type_traits>
#include <utility>

namespace engine {
namespace {

constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

constexpr auto trim = container::trimAsciiCopy;

[[nodiscard]] container::String stripComment(container::StringView value) {
    const size_t comment = value.find(';');
    return trim(value.substr(0, comment));
}

[[nodiscard]] container::String canonical(container::StringView value) {
    container::String output = trim(value);
    std::transform(output.begin(), output.end(), output.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return output;
}

[[nodiscard]] bool equalsInsensitive(container::StringView lhs, container::StringView rhs) {
    return canonical(lhs) == canonical(rhs);
}

[[nodiscard]] bool parseBoolean(container::StringView value, bool& output) {
    const container::String normalized = canonical(stripComment(value));
    if (normalized == "yes" || normalized == "true" || normalized == "1") {
        output = true;
        return true;
    }
    if (normalized == "no" || normalized == "false" || normalized == "0") {
        output = false;
        return true;
    }
    return false;
}

[[nodiscard]] bool parseInt32(container::StringView value, int32_t& output) {
    const container::String cleaned = stripComment(value);
    if (cleaned.empty()) return false;
    const char* first = cleaned.data();
    const char* last = first + cleaned.size();
    int32_t parsed = 0;
    const auto [end, error] = std::from_chars(first, last, parsed);
    if (error != std::errc{} || end != last) return false;
    output = parsed;
    return true;
}

[[nodiscard]] bool parseChannel(container::StringView value, char channel, uint8_t& output) {
    const container::String cleaned = stripComment(value);
    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(channel)));
    for (size_t index = 0; index + 2 <= cleaned.size(); ++index) {
        if (std::toupper(static_cast<unsigned char>(cleaned[index])) != upper ||
            cleaned[index + 1] != ':') {
            continue;
        }
        size_t cursor = index + 2;
        while (cursor < cleaned.size() && std::isspace(static_cast<unsigned char>(cleaned[cursor]))) {
            ++cursor;
        }
        const char* first = cleaned.data() + cursor;
        const char* last = cleaned.data() + cleaned.size();
        int parsed = 0;
        const auto [end, error] = std::from_chars(first, last, parsed);
        if (error != std::errc{} || end == first) return false;
        output = static_cast<uint8_t>(std::clamp(parsed, 0, 255));
        return true;
    }
    return false;
}

[[nodiscard]] bool parseRgb(container::StringView value, PlayerRgbColor& output) {
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    if (!parseChannel(value, 'R', red) || !parseChannel(value, 'G', green) ||
        !parseChannel(value, 'B', blue)) {
        return false;
    }
    output = {red, green, blue};
    return true;
}

[[nodiscard]] container::Vector<container::String> words(container::StringView value) {
    const container::String cleaned = stripComment(value);
    container::Vector<container::String> output;
    size_t cursor = 0;
    while (cursor < cleaned.size()) {
        while (cursor < cleaned.size() && std::isspace(static_cast<unsigned char>(cleaned[cursor]))) {
            ++cursor;
        }
        const size_t begin = cursor;
        while (cursor < cleaned.size() && !std::isspace(static_cast<unsigned char>(cleaned[cursor]))) {
            ++cursor;
        }
        if (begin != cursor) output.emplace_back(cleaned.substr(begin, cursor - begin));
    }
    return output;
}

[[nodiscard]] bool parsePercentBasisPoints(container::StringView value, int32_t& output) {
    container::String cleaned = stripComment(value);
    if (cleaned.empty()) return false;
    if (cleaned.back() == '%') cleaned.pop_back();
    cleaned = trim(cleaned);
    if (cleaned.empty()) return false;

    bool negative = false;
    size_t cursor = 0;
    if (cleaned[cursor] == '+' || cleaned[cursor] == '-') {
        negative = cleaned[cursor] == '-';
        ++cursor;
    }
    if (cursor == cleaned.size()) return false;

    const int64_t maximumMagnitude = negative
        ? -static_cast<int64_t>(std::numeric_limits<int32_t>::min())
        : static_cast<int64_t>(std::numeric_limits<int32_t>::max());
    const int64_t maximumWhole = maximumMagnitude / 100;
    int64_t whole = 0;
    bool sawDigit = false;
    while (cursor < cleaned.size() && std::isdigit(static_cast<unsigned char>(cleaned[cursor]))) {
        sawDigit = true;
        const int64_t digit = cleaned[cursor] - '0';
        // Check before multiplying: malformed mod INI must not overflow the
        // parser's signed accumulator before it can be rejected.
        if (whole > (maximumWhole - digit) / 10) return false;
        whole = whole * 10 + digit;
        ++cursor;
    }
    if (!sawDigit) return false;

    int64_t fractionalHundredths = 0;
    if (cursor < cleaned.size() && cleaned[cursor] == '.') {
        ++cursor;
        int digits = 0;
        while (cursor < cleaned.size() && std::isdigit(static_cast<unsigned char>(cleaned[cursor]))) {
            if (digits < 2) {
                fractionalHundredths = fractionalHundredths * 10 + (cleaned[cursor] - '0');
            }
            ++digits;
            ++cursor;
        }
        if (digits == 0) return false;
        if (digits == 1) fractionalHundredths *= 10;
        // Further fractional digits are intentionally truncated, matching a
        // fixed 1/100-percent representation rather than platform float math.
    }
    if (cursor != cleaned.size()) return false;

    int64_t result = whole * 100 + fractionalHundredths;
    if (result > maximumMagnitude) {
        return false;
    }
    if (negative) result = -result;
    output = static_cast<int32_t>(result);
    return true;
}

template <typename Modifier>
void updateModifier(container::Vector<Modifier>& modifiers, Modifier modifier) {
    const container::String target = canonical(modifier.thingTemplateName);
    const auto found = std::find_if(modifiers.begin(), modifiers.end(), [&](const Modifier& current) {
        return canonical(current.thingTemplateName) == target;
    });
    if (found == modifiers.end()) {
        modifiers.push_back(std::move(modifier));
    } else {
        *found = std::move(modifier);
    }
}

[[nodiscard]] bool parsePercentModifier(container::StringView value, ProductionPercentModifier& output) {
    const container::Vector<container::String> parts = words(value);
    if (parts.size() != 2 || !parsePercentBasisPoints(parts[1], output.multiplierBasisPoints)) {
        return false;
    }
    output.thingTemplateName = parts[0];
    return !output.thingTemplateName.empty();
}

[[nodiscard]] bool parseVeterancyModifier(container::StringView value,
                                          ProductionVeterancyModifier& output) {
    const container::Vector<container::String> parts = words(value);
    if (parts.size() != 2) return false;
    output.thingTemplateName = parts[0];
    output.veterancyName = parts[1];
    return true;
}

[[nodiscard]] bool parseStartingUnitIndex(container::StringView key, size_t& index) {
    const container::String normalized = canonical(key);
    constexpr container::StringView prefix = "startingunit";
    if (!normalized.starts_with(prefix)) return false;
    const container::StringView suffix(normalized.data() + prefix.size(),
                                  normalized.size() - prefix.size());
    if (suffix.empty()) return false;
    uint32_t parsed = 0;
    const auto [end, error] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), parsed);
    if (error != std::errc{} || end != suffix.data() + suffix.size() || parsed >= 10) return false;
    index = parsed;
    return true;
}

void addExtension(container::Vector<TemplateExtensionField>& fields, container::String key,
                  container::String value, const TemplateSourceInfo& source) {
    fields.push_back({std::move(key), std::move(value), source});
}

void applyFactionField(FactionTemplate& faction, const container::String& key,
                       const container::String& rawValue, const TemplateSourceInfo& source) {
    const container::String normalized = canonical(key);
    const container::String value = stripComment(rawValue);
    if (normalized == "side") faction.side = value;
    else if (normalized == "baseside") faction.baseSide = value;
    else if (normalized == "playableside") {
        if (!parseBoolean(value, faction.playable)) addExtension(faction.extensionFields, key, rawValue, source);
    } else if (normalized == "isobserver") {
        if (!parseBoolean(value, faction.observer)) addExtension(faction.extensionFields, key, rawValue, source);
    } else if (normalized == "oldfaction") {
        if (!parseBoolean(value, faction.oldFaction)) addExtension(faction.extensionFields, key, rawValue, source);
    } else if (normalized == "displayname") faction.presentation.displayName = value;
    else if (normalized == "startmoney") {
        int32_t parsed = 0;
        if (parseInt32(value, parsed) && parsed >= 0) faction.simulation.startingMoney = parsed;
        else addExtension(faction.extensionFields, key, rawValue, source);
    } else if (normalized == "preferredcolor") {
        if (!parseRgb(value, faction.simulation.preferredColor)) addExtension(faction.extensionFields, key, rawValue, source);
    } else if (normalized == "startingbuilding") faction.simulation.startingBuilding = value;
    else if (normalized == "productioncostchange") {
        ProductionPercentModifier modifier;
        if (parsePercentModifier(value, modifier)) updateModifier(faction.simulation.productionCostModifiers, std::move(modifier));
        else addExtension(faction.extensionFields, key, rawValue, source);
    } else if (normalized == "productiontimechange") {
        ProductionPercentModifier modifier;
        if (parsePercentModifier(value, modifier)) updateModifier(faction.simulation.productionTimeModifiers, std::move(modifier));
        else addExtension(faction.extensionFields, key, rawValue, source);
    } else if (normalized == "productionveterancylevel") {
        ProductionVeterancyModifier modifier;
        if (parseVeterancyModifier(value, modifier)) updateModifier(faction.simulation.productionVeterancyModifiers, std::move(modifier));
        else addExtension(faction.extensionFields, key, rawValue, source);
    } else if (normalized == "intrinsicsciences") {
        faction.simulation.intrinsicSciences = words(value);
        if (faction.simulation.intrinsicSciences.size() == 1 &&
            equalsInsensitive(faction.simulation.intrinsicSciences.front(), "none")) {
            faction.simulation.intrinsicSciences.clear();
        }
    } else if (normalized == "intrinsicsciencepurchasepoints") {
        if (!parseInt32(value, faction.simulation.intrinsicSciencePurchasePoints)) {
            addExtension(faction.extensionFields, key, rawValue, source);
        }
    } else if (normalized == "purchasesciencecommandsetrank1") faction.presentation.purchaseScienceCommandSets[0] = value;
    else if (normalized == "purchasesciencecommandsetrank3") faction.presentation.purchaseScienceCommandSets[1] = value;
    else if (normalized == "purchasesciencecommandsetrank8") faction.presentation.purchaseScienceCommandSets[2] = value;
    else if (normalized == "specialpowershortcutcommandset") faction.presentation.specialPowerShortcutCommandSet = value;
    else if (normalized == "specialpowershortcutwinname") faction.presentation.specialPowerShortcutWindow = value;
    else if (normalized == "specialpowershortcutbuttoncount") {
        if (!parseInt32(value, faction.presentation.specialPowerShortcutButtonCount)) {
            addExtension(faction.extensionFields, key, rawValue, source);
        }
    } else if (normalized == "scorescreenimage") faction.presentation.scoreScreenImage = value;
    else if (normalized == "loadscreenimage") faction.presentation.loadScreenImage = value;
    else if (normalized == "loadscreenmusic") faction.presentation.loadScreenMusic = value;
    else if (normalized == "scorescreenmusic") faction.presentation.scoreScreenMusic = value;
    else if (normalized == "headwatermark") faction.presentation.headWaterMark = value;
    else if (normalized == "flagwatermark") faction.presentation.flagWaterMark = value;
    else if (normalized == "enabledimage") faction.presentation.enabledImage = value;
    else if (normalized == "sideiconimage") faction.presentation.sideIconImage = value;
    else if (normalized == "generalimage") faction.presentation.generalImage = value;
    else if (normalized == "beaconname") faction.presentation.beaconTemplate = value;
    else if (normalized == "armytooltip") faction.presentation.armyTooltip = value;
    else if (normalized == "features") faction.presentation.features = value;
    else if (normalized == "medallionregular") faction.presentation.medallionRegular = value;
    else if (normalized == "medallionhilite") faction.presentation.medallionHilite = value;
    else if (normalized == "medallionselect") faction.presentation.medallionSelect = value;
    else {
        size_t unitIndex = 0;
        if (parseStartingUnitIndex(key, unitIndex)) {
            faction.simulation.startingUnits[unitIndex] = value;
        } else {
            addExtension(faction.extensionFields, key, rawValue, source);
        }
    }
}

void applyFactionBlock(container::Vector<FactionTemplate>& templates, const game::IniBlock& block,
                       const TemplateSourceInfo& source, uint32_t& nextAuthoredOrder) {
    const container::String name = stripComment(block.name);
    if (name.empty()) return;
    const container::String canonicalName = canonical(name);
    auto found = std::find_if(templates.begin(), templates.end(), [&](const FactionTemplate& templateValue) {
        return canonical(templateValue.name) == canonicalName;
    });
    if (found == templates.end()) {
        FactionTemplate faction;
        faction.name = name;
        faction.authoredOrder = nextAuthoredOrder++;
        templates.push_back(std::move(faction));
        found = std::prev(templates.end());
    }
    found->sources.push_back(source);
    for (const auto& [key, value] : block.values) {
        applyFactionField(*found, key, value, source);
    }
}

void applyMultiplayerSettings(MultiplayerRules& rules, const game::IniBlock& block,
                              const TemplateSourceInfo& source) {
    for (const auto& [key, rawValue] : block.values) {
        const container::String normalized = canonical(key);
        const container::String value = stripComment(rawValue);
        if (normalized == "startcountdowntimer") {
            if (!parseInt32(value, rules.startCountdownSeconds)) addExtension(rules.extensionFields, key, rawValue, source);
        } else if (normalized == "maxbeaconsperplayer") {
            if (!parseInt32(value, rules.maxBeaconsPerPlayer)) addExtension(rules.extensionFields, key, rawValue, source);
        } else if (normalized == "useshroud") {
            if (!parseBoolean(value, rules.useShroud)) addExtension(rules.extensionFields, key, rawValue, source);
        } else if (normalized == "showrandomplayertemplate") {
            if (!parseBoolean(value, rules.showRandomPlayerTemplate)) addExtension(rules.extensionFields, key, rawValue, source);
        } else if (normalized == "showrandomstartpos") {
            if (!parseBoolean(value, rules.showRandomStartPosition)) addExtension(rules.extensionFields, key, rawValue, source);
        } else if (normalized == "showrandomcolor") {
            if (!parseBoolean(value, rules.showRandomColor)) addExtension(rules.extensionFields, key, rawValue, source);
        } else {
            addExtension(rules.extensionFields, key, rawValue, source);
        }
    }
}

void applyMultiplayerColor(MultiplayerRules& rules, const game::IniBlock& block,
                           const TemplateSourceInfo& source, uint32_t& nextAuthoredOrder) {
    const container::String name = stripComment(block.name);
    if (name.empty()) return;
    const container::String canonicalName = canonical(name);
    auto found = std::find_if(rules.colors.begin(), rules.colors.end(),
        [&](const MultiplayerColorDefinition& color) { return canonical(color.name) == canonicalName; });
    if (found == rules.colors.end()) {
        MultiplayerColorDefinition color;
        color.name = name;
        color.authoredOrder = nextAuthoredOrder++;
        rules.colors.push_back(std::move(color));
        found = std::prev(rules.colors.end());
    }
    found->sources.push_back(source);
    for (const auto& [key, rawValue] : block.values) {
        const container::String normalized = canonical(key);
        const container::String value = stripComment(rawValue);
        if (normalized == "rgbcolor" || normalized == "rgbfcolor") {
            if (!parseRgb(value, found->day)) addExtension(rules.extensionFields, key, rawValue, source);
        } else if (normalized == "rgbnightcolor") {
            if (!parseRgb(value, found->night)) addExtension(rules.extensionFields, key, rawValue, source);
        } else if (normalized == "tooltipname") {
            found->tooltipName = value;
        } else {
            addExtension(rules.extensionFields, key, rawValue, source);
        }
    }
}

void applyChatColors(MultiplayerRules& rules, const game::IniBlock& block,
                     const TemplateSourceInfo& source) {
    for (const auto& [key, rawValue] : block.values) {
        PlayerRgbColor color;
        if (!parseRgb(rawValue, color)) {
            addExtension(rules.extensionFields, key, rawValue, source);
            continue;
        }
        const container::String normalized = canonical(key);
        auto found = std::find_if(rules.onlineChatColors.begin(), rules.onlineChatColors.end(),
            [&](const NamedRgbSetting& setting) { return canonical(setting.name) == normalized; });
        NamedRgbSetting setting{.name = key, .color = color, .source = source};
        if (found == rules.onlineChatColors.end()) rules.onlineChatColors.push_back(std::move(setting));
        else *found = std::move(setting);
    }
}

void applyStartingMoneyChoice(MultiplayerRules& rules, const game::IniBlock& block,
                              const TemplateSourceInfo& source) {
    int32_t value = 0;
    bool hasValue = false;
    bool isDefault = false;
    for (const auto& [key, rawValue] : block.values) {
        const container::String normalized = canonical(key);
        if (normalized == "value") {
            hasValue = parseInt32(rawValue, value) && value >= 0;
            if (!hasValue) addExtension(rules.extensionFields, key, rawValue, source);
        } else if (normalized == "default") {
            if (!parseBoolean(rawValue, isDefault)) addExtension(rules.extensionFields, key, rawValue, source);
        } else {
            addExtension(rules.extensionFields, key, rawValue, source);
        }
    }
    if (!hasValue) return;
    rules.startingMoneyChoices.push_back(value);
    if (isDefault) rules.defaultStartingMoney = value;
}

template <typename Callback>
[[nodiscard]] bool forEachLayeredBlock(container::StringView path, Callback&& callback) {
    auto& vfs = io::VFS::instance();
    if (!vfs.exists(path)) return false;
    game::GeneralsIniParser parser;
    if (!parser.parse(vfs.readAll(path), path)) return false;
    const TemplateSourceInfo source{.path = container::String(path), .layer = 0};
    for (const game::IniBlock& block : parser.blocks()) {
        callback(block, source);
    }
    return true;
}

template <typename T, typename Name>
void sortByCanonicalName(container::Vector<T>& values, Name&& name) {
    std::sort(values.begin(), values.end(), [&](const T& lhs, const T& rhs) {
        const container::String left = canonical(name(lhs));
        const container::String right = canonical(name(rhs));
        if (left != right) return left < right;
        return name(lhs) < name(rhs);
    });
}

void sortModifiers(container::Vector<ProductionPercentModifier>& modifiers) {
    sortByCanonicalName(modifiers, [](const ProductionPercentModifier& modifier) -> const container::String& {
        return modifier.thingTemplateName;
    });
}

void sortModifiers(container::Vector<ProductionVeterancyModifier>& modifiers) {
    sortByCanonicalName(modifiers, [](const ProductionVeterancyModifier& modifier) -> const container::String& {
        return modifier.thingTemplateName;
    });
}

class CanonicalHasher final {
public:
    void byte(uint8_t value) noexcept {
        m_value ^= value;
        m_value *= kFnvPrime;
    }

    void boolean(bool value) noexcept { byte(value ? 1u : 0u); }

    void u32(uint32_t value) noexcept {
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            byte(static_cast<uint8_t>((value >> shift) & 0xffu));
        }
    }

    void i32(int32_t value) noexcept { u32(static_cast<uint32_t>(value)); }

    void string(container::StringView value) noexcept {
        u32(static_cast<uint32_t>(value.size()));
        for (const unsigned char character : value) byte(character);
    }

    void color(PlayerRgbColor value) noexcept {
        byte(value.red);
        byte(value.green);
        byte(value.blue);
    }

    [[nodiscard]] uint64_t finish() const noexcept { return m_value; }

private:
    uint64_t m_value = kFnvOffsetBasis;
};

void hashExtensions(CanonicalHasher& hash, container::Span<const TemplateExtensionField> fields) {
    hash.u32(static_cast<uint32_t>(fields.size()));
    for (const TemplateExtensionField& field : fields) {
        hash.string(field.key);
        hash.string(field.value);
    }
}

void hashSimulationModifiers(CanonicalHasher& hash,
                             container::Span<const ProductionPercentModifier> modifiers) {
    hash.u32(static_cast<uint32_t>(modifiers.size()));
    for (const auto& modifier : modifiers) {
        hash.string(modifier.thingTemplateName);
        hash.i32(modifier.multiplierBasisPoints);
    }
}

[[nodiscard]] uint64_t calculateSimulationFingerprint(const container::Vector<FactionTemplate>& templates,
                                                       const MultiplayerRules& multiplayer) {
    CanonicalHasher hash;
    hash.u32(static_cast<uint32_t>(templates.size()));
    for (const FactionTemplate& faction : templates) {
        // IDs and authored order both participate in the legacy adapter:
        // canonical IDs are persisted, authored order resolves old indices.
        hash.u32(faction.id.value);
        hash.string(faction.name);
        hash.string(faction.side);
        hash.string(faction.baseSide);
        hash.boolean(faction.playable);
        hash.boolean(faction.observer);
        hash.boolean(faction.oldFaction);
        hash.u32(faction.authoredOrder);
        hash.i32(faction.simulation.startingMoney);
        hash.string(faction.simulation.startingBuilding);
        for (const container::String& unit : faction.simulation.startingUnits) hash.string(unit);
        hashSimulationModifiers(hash, faction.simulation.productionCostModifiers);
        hashSimulationModifiers(hash, faction.simulation.productionTimeModifiers);
        hash.u32(static_cast<uint32_t>(faction.simulation.productionVeterancyModifiers.size()));
        for (const auto& modifier : faction.simulation.productionVeterancyModifiers) {
            hash.string(modifier.thingTemplateName);
            hash.string(modifier.veterancyName);
        }
        hash.u32(static_cast<uint32_t>(faction.simulation.intrinsicSciences.size()));
        for (const container::String& science : faction.simulation.intrinsicSciences) hash.string(science);
        hash.i32(faction.simulation.intrinsicSciencePurchasePoints);
    }

    hash.i32(multiplayer.startCountdownSeconds);
    hash.i32(multiplayer.maxBeaconsPerPlayer);
    hash.boolean(multiplayer.useShroud);
    hash.i32(multiplayer.defaultStartingMoney);
    hash.u32(static_cast<uint32_t>(multiplayer.startingMoneyChoices.size()));
    for (const int32_t value : multiplayer.startingMoneyChoices) hash.i32(value);
    // Color names/order influence the legacy color-index adapter and seeded
    // resolution. RGB/tooltip values are pure presentation data.
    hash.u32(static_cast<uint32_t>(multiplayer.colors.size()));
    for (const MultiplayerColorDefinition& color : multiplayer.colors) {
        hash.u32(color.id.value);
        hash.string(color.name);
        hash.u32(color.authoredOrder);
    }
    return hash.finish();
}

[[nodiscard]] uint64_t calculateContentFingerprint(const container::Vector<FactionTemplate>& templates,
                                                    const MultiplayerRules& multiplayer) {
    CanonicalHasher hash;
    hash.u32(static_cast<uint32_t>(templates.size()));
    for (const FactionTemplate& faction : templates) {
        hash.u32(faction.id.value);
        hash.string(faction.name);
        hash.string(faction.side);
        hash.string(faction.baseSide);
        hash.boolean(faction.playable);
        hash.boolean(faction.observer);
        hash.boolean(faction.oldFaction);
        hash.u32(faction.authoredOrder);
        hash.i32(faction.simulation.startingMoney);
        hash.color(faction.simulation.preferredColor);
        hash.string(faction.simulation.startingBuilding);
        for (const container::String& unit : faction.simulation.startingUnits) hash.string(unit);
        hashSimulationModifiers(hash, faction.simulation.productionCostModifiers);
        hashSimulationModifiers(hash, faction.simulation.productionTimeModifiers);
        hash.u32(static_cast<uint32_t>(faction.simulation.productionVeterancyModifiers.size()));
        for (const auto& modifier : faction.simulation.productionVeterancyModifiers) {
            hash.string(modifier.thingTemplateName);
            hash.string(modifier.veterancyName);
        }
        hash.u32(static_cast<uint32_t>(faction.simulation.intrinsicSciences.size()));
        for (const container::String& science : faction.simulation.intrinsicSciences) hash.string(science);
        hash.i32(faction.simulation.intrinsicSciencePurchasePoints);
        hash.string(faction.presentation.displayName);
        for (const container::String& commandSet : faction.presentation.purchaseScienceCommandSets) hash.string(commandSet);
        hash.string(faction.presentation.specialPowerShortcutCommandSet);
        hash.string(faction.presentation.specialPowerShortcutWindow);
        hash.i32(faction.presentation.specialPowerShortcutButtonCount);
        hash.string(faction.presentation.scoreScreenImage);
        hash.string(faction.presentation.loadScreenImage);
        hash.string(faction.presentation.loadScreenMusic);
        hash.string(faction.presentation.scoreScreenMusic);
        hash.string(faction.presentation.headWaterMark);
        hash.string(faction.presentation.flagWaterMark);
        hash.string(faction.presentation.enabledImage);
        hash.string(faction.presentation.sideIconImage);
        hash.string(faction.presentation.generalImage);
        hash.string(faction.presentation.beaconTemplate);
        hash.string(faction.presentation.armyTooltip);
        hash.string(faction.presentation.features);
        hash.string(faction.presentation.medallionRegular);
        hash.string(faction.presentation.medallionHilite);
        hash.string(faction.presentation.medallionSelect);
        hashExtensions(hash, faction.extensionFields);
    }

    hash.i32(multiplayer.startCountdownSeconds);
    hash.i32(multiplayer.maxBeaconsPerPlayer);
    hash.boolean(multiplayer.useShroud);
    hash.boolean(multiplayer.showRandomPlayerTemplate);
    hash.boolean(multiplayer.showRandomStartPosition);
    hash.boolean(multiplayer.showRandomColor);
    hash.i32(multiplayer.defaultStartingMoney);
    hash.u32(static_cast<uint32_t>(multiplayer.startingMoneyChoices.size()));
    for (const int32_t value : multiplayer.startingMoneyChoices) hash.i32(value);
    hash.u32(static_cast<uint32_t>(multiplayer.colors.size()));
    for (const MultiplayerColorDefinition& color : multiplayer.colors) {
        hash.u32(color.id.value);
        hash.string(color.name);
        hash.string(color.tooltipName);
        hash.color(color.day);
        hash.color(color.night);
        hash.u32(color.authoredOrder);
    }
    hash.u32(static_cast<uint32_t>(multiplayer.onlineChatColors.size()));
    for (const NamedRgbSetting& setting : multiplayer.onlineChatColors) {
        hash.string(setting.name);
        hash.color(setting.color);
    }
    hashExtensions(hash, multiplayer.extensionFields);
    return hash.finish();
}

} // namespace

bool MultiplayerRuleset::loadFromVfs(container::StringView multiplayerPath,
                                     container::StringView playerTemplatePath,
                                     container::String* error) {
    if (error) error->clear();
    clear();

    uint32_t nextTemplateOrder = 0;
    if (!forEachLayeredBlock(playerTemplatePath, [&](const game::IniBlock& block,
                                                     const TemplateSourceInfo& source) {
            if (equalsInsensitive(block.type, "PlayerTemplate")) {
                applyFactionBlock(m_templates, block, source, nextTemplateOrder);
            }
        })) {
        if (error) *error = "PlayerTemplate INI was not found in VFS: " + container::String(playerTemplatePath);
        return false;
    }
    if (m_templates.empty()) {
        if (error) *error = "PlayerTemplate INI contained no PlayerTemplate blocks: " +
            container::String(playerTemplatePath);
        return false;
    }

    uint32_t nextColorOrder = 0;
    if (!forEachLayeredBlock(multiplayerPath, [&](const game::IniBlock& block,
                                                  const TemplateSourceInfo& source) {
            if (equalsInsensitive(block.type, "MultiplayerSettings")) {
                applyMultiplayerSettings(m_multiplayer, block, source);
            } else if (equalsInsensitive(block.type, "MultiplayerColor")) {
                applyMultiplayerColor(m_multiplayer, block, source, nextColorOrder);
            } else if (equalsInsensitive(block.type, "MultiplayerStartingMoneyChoice")) {
                applyStartingMoneyChoice(m_multiplayer, block, source);
            } else if (equalsInsensitive(block.type, "OnlineChatColors")) {
                applyChatColors(m_multiplayer, block, source);
            }
        })) {
        if (error) *error = "Multiplayer INI was not found in VFS: " + container::String(multiplayerPath);
        clear();
        return false;
    }

    rebuildDerivedState();
    m_loaded = true;
    TD_LOG_INFO("[MultiplayerRuleset] Loaded {} faction templates, {} colors, simulation={:016X}, content={:016X}",
                m_templates.size(), m_multiplayer.colors.size(), m_simulationFingerprint,
                m_contentFingerprint);
    return true;
}

bool MultiplayerRuleset::applyOverridesFromVfs(container::StringView path,
                                                container::String* error) {
    if (error) error->clear();
    if (!m_loaded) {
        if (error) *error = "MultiplayerRuleset overrides require an already loaded ruleset";
        return false;
    }

    auto& vfs = io::VFS::instance();
    if (!vfs.exists(path)) {
        if (error) *error = "Ruleset override INI was not found in VFS: " + container::String(path);
        return false;
    }

    game::GeneralsIniParser parser;
    if (!parser.parse(vfs.readAll(path), path)) {
        if (error) *error = "Ruleset override INI could not be parsed: " + container::String(path);
        return false;
    }

    bool hasRelevantBlock = false;
    for (const game::IniBlock& block : parser.blocks()) {
        if (equalsInsensitive(block.type, "PlayerTemplate") ||
            equalsInsensitive(block.type, "MultiplayerColor") ||
            equalsInsensitive(block.type, "MultiplayerSettings")) {
            hasRelevantBlock = true;
            break;
        }
    }
    if (!hasRelevantBlock) return true;

    // All potentially failing allocation and parsing work happens on a local
    // value.  The final move assignment is statically required to be
    // non-throwing, so a failed override can never expose a partially patched
    // session ruleset.
    MultiplayerRuleset candidate = *this;
    uint32_t nextTemplateOrder = static_cast<uint32_t>(candidate.m_templates.size());
    uint32_t nextColorOrder = static_cast<uint32_t>(candidate.m_multiplayer.colors.size());
    const TemplateSourceInfo source{.path = container::String(path), .layer = 0};

    for (const game::IniBlock& block : parser.blocks()) {
        if (equalsInsensitive(block.type, "PlayerTemplate")) {
            applyFactionBlock(candidate.m_templates, block, source, nextTemplateOrder);
        } else if (equalsInsensitive(block.type, "MultiplayerColor")) {
            applyMultiplayerColor(candidate.m_multiplayer, block, source, nextColorOrder);
        } else if (equalsInsensitive(block.type, "MultiplayerSettings")) {
            applyMultiplayerSettings(candidate.m_multiplayer, block, source);
        }
    }

    candidate.rebuildDerivedState();
    static_assert(std::is_nothrow_move_assignable_v<MultiplayerRuleset>);
    *this = std::move(candidate);
    return true;
}

void MultiplayerRuleset::rebuildDerivedState() {
    m_playableTemplateIds.clear();
    m_templateIdsByAuthoredOrder.clear();
    m_colorIdsByAuthoredOrder.clear();

    sortByCanonicalName(m_templates, [](const FactionTemplate& faction) -> const container::String& {
        return faction.name;
    });
    for (size_t index = 0; index < m_templates.size(); ++index) {
        m_templates[index].id = FactionTemplateId{static_cast<uint32_t>(index + 1)};
        sortModifiers(m_templates[index].simulation.productionCostModifiers);
        sortModifiers(m_templates[index].simulation.productionTimeModifiers);
        sortModifiers(m_templates[index].simulation.productionVeterancyModifiers);
    }

    container::Vector<const FactionTemplate*> authoredTemplates;
    authoredTemplates.reserve(m_templates.size());
    for (const FactionTemplate& faction : m_templates) {
        // An authored observer template may retain PlayableSide=Yes for menu
        // metadata, but it is never an eligible participant faction.
        if (faction.playable && !faction.observer) authoredTemplates.push_back(&faction);
    }
    std::sort(authoredTemplates.begin(), authoredTemplates.end(), [](const FactionTemplate* lhs,
                                                                      const FactionTemplate* rhs) {
        if (lhs->authoredOrder != rhs->authoredOrder) return lhs->authoredOrder < rhs->authoredOrder;
        return canonical(lhs->name) < canonical(rhs->name);
    });
    for (const FactionTemplate* faction : authoredTemplates) m_playableTemplateIds.push_back(faction->id);

    container::Vector<const FactionTemplate*> allAuthoredTemplates;
    allAuthoredTemplates.reserve(m_templates.size());
    for (const FactionTemplate& faction : m_templates) allAuthoredTemplates.push_back(&faction);
    std::sort(allAuthoredTemplates.begin(), allAuthoredTemplates.end(), [](const FactionTemplate* lhs,
                                                                           const FactionTemplate* rhs) {
        if (lhs->authoredOrder != rhs->authoredOrder) return lhs->authoredOrder < rhs->authoredOrder;
        return canonical(lhs->name) < canonical(rhs->name);
    });
    for (const FactionTemplate* faction : allAuthoredTemplates) {
        m_templateIdsByAuthoredOrder.push_back(faction->id);
    }

    sortByCanonicalName(m_multiplayer.colors, [](const MultiplayerColorDefinition& color) -> const container::String& {
        return color.name;
    });
    for (size_t index = 0; index < m_multiplayer.colors.size(); ++index) {
        m_multiplayer.colors[index].id = MultiplayerColorId{static_cast<uint32_t>(index + 1)};
    }
    container::Vector<const MultiplayerColorDefinition*> authoredColors;
    authoredColors.reserve(m_multiplayer.colors.size());
    for (const MultiplayerColorDefinition& color : m_multiplayer.colors) authoredColors.push_back(&color);
    std::sort(authoredColors.begin(), authoredColors.end(), [](const MultiplayerColorDefinition* lhs,
                                                                const MultiplayerColorDefinition* rhs) {
        if (lhs->authoredOrder != rhs->authoredOrder) return lhs->authoredOrder < rhs->authoredOrder;
        return canonical(lhs->name) < canonical(rhs->name);
    });
    for (const MultiplayerColorDefinition* color : authoredColors) m_colorIdsByAuthoredOrder.push_back(color->id);

    sortByCanonicalName(m_multiplayer.onlineChatColors, [](const NamedRgbSetting& setting) -> const container::String& {
        return setting.name;
    });
    m_simulationFingerprint = calculateSimulationFingerprint(m_templates, m_multiplayer);
    m_contentFingerprint = calculateContentFingerprint(m_templates, m_multiplayer);
}

void MultiplayerRuleset::clear() {
    m_templates.clear();
    m_playableTemplateIds.clear();
    m_templateIdsByAuthoredOrder.clear();
    m_colorIdsByAuthoredOrder.clear();
    m_multiplayer = {};
    m_simulationFingerprint = 0;
    m_contentFingerprint = 0;
    m_loaded = false;
}

void MultiplayerRuleset::sealEmpty() {
    clear();
    rebuildDerivedState();
    m_loaded = true;
}

const FactionTemplate* MultiplayerRuleset::findFaction(FactionTemplateId id) const noexcept {
    if (!id || id.value > m_templates.size()) return nullptr;
    return &m_templates[id.value - 1];
}

const FactionTemplate* MultiplayerRuleset::findFaction(container::StringView name) const {
    const container::String wanted = canonical(name);
    const auto found = std::lower_bound(m_templates.begin(), m_templates.end(), wanted,
        [](const FactionTemplate& faction, const container::String& value) {
            return canonical(faction.name) < value;
        });
    if (found == m_templates.end() || canonical(found->name) != wanted) return nullptr;
    return &*found;
}

const MultiplayerColorDefinition* MultiplayerRuleset::findColor(MultiplayerColorId id) const noexcept {
    if (!id || id.value > m_multiplayer.colors.size()) return nullptr;
    return &m_multiplayer.colors[id.value - 1];
}

const MultiplayerColorDefinition* MultiplayerRuleset::findColor(container::StringView name) const {
    const container::String wanted = canonical(name);
    const auto found = std::lower_bound(m_multiplayer.colors.begin(), m_multiplayer.colors.end(), wanted,
        [](const MultiplayerColorDefinition& color, const container::String& value) {
            return canonical(color.name) < value;
        });
    if (found == m_multiplayer.colors.end() || canonical(found->name) != wanted) return nullptr;
    return &*found;
}

std::optional<FactionTemplateId> MultiplayerRuleset::playableTemplateIdAt(size_t authoredIndex) const noexcept {
    if (authoredIndex >= m_playableTemplateIds.size()) return std::nullopt;
    return m_playableTemplateIds[authoredIndex];
}

std::optional<MultiplayerColorId> MultiplayerRuleset::colorIdAt(size_t authoredIndex) const noexcept {
    if (authoredIndex >= m_colorIdsByAuthoredOrder.size()) return std::nullopt;
    return m_colorIdsByAuthoredOrder[authoredIndex];
}

} // namespace engine

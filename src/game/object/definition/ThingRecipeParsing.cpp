#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "CombatProfile.h"
#include "ObjectModuleCatalog.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "VFS.h"
#include "debug/debug.h"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <optional>
#include "ThingRecipeDetail.h"

namespace game::detail {

ModuleData makeModuleData(const IniBlock& block) {
    ModuleData module;
    module.type = block.type;
    module.tag = block.name;
    module.sourcePath = block.source.path;
    module.sourceLine = block.source.line;
    module.valueSourceLines = block.valueSourceLines;
    module.properties.reserve(block.values.size());
    module.values = block.values;
    for (const auto& [key, value] : block.values) {
        module.properties.insert_or_assign(key, value);
    }
    module.children.reserve(block.children.size());
    for (const IniBlock& child : block.children) {
        module.children.push_back(makeModuleData(child));
    }
    return module;
}

const container::String* firstValue(const ModuleData& module, container::StringView key) {
    for (const auto& [currentKey, value] : module.values) {
        if (currentKey == key && !value.empty()) return &value;
    }
    return nullptr;
}

container::String lowerAscii(container::String value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

container::StringView firstToken(container::StringView value) noexcept {
    const size_t begin = value.find_first_not_of(" \t");
    if (begin == container::StringView::npos) return {};
    const size_t end = value.find_first_of(" \t", begin);
    return value.substr(begin, end - begin);
}

container::StringView tokensAfterFirst(container::StringView value) noexcept {
    const size_t begin = value.find_first_not_of(" \t");
    if (begin == container::StringView::npos) return {};
    const size_t end = value.find_first_of(" \t", begin);
    if (end == container::StringView::npos) return {};
    const size_t tail = value.find_first_not_of(" \t", end);
    return tail == container::StringView::npos ? container::StringView{} : value.substr(tail);
}

[[nodiscard]] bool hasAsciiToken(container::StringView value,
                                 container::StringView expected) {
    const container::String foldedExpected =
        lowerAscii(container::String{expected});
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t\r\n", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t\r\n", cursor);
        const container::StringView token = value.substr(
            cursor, end == container::StringView::npos
                        ? value.size() - cursor : end - cursor);
        if (lowerAscii(container::String{token}) == foldedExpected) return true;
        if (end == container::StringView::npos) break;
        cursor = end + 1;
    }
    return false;
}

[[nodiscard]] bool applyLegacyBitMaskText(
    container::String& destination, container::StringView authored) {
    container::Vector<container::String> bits;
    const auto appendExisting = [&bits](container::StringView text) {
        size_t cursor = 0;
        while (cursor < text.size()) {
            cursor = text.find_first_not_of(" \t\r\n,", cursor);
            if (cursor == container::StringView::npos) break;
            const size_t end = text.find_first_of(" \t\r\n,", cursor);
            container::StringView token = text.substr(
                cursor, end == container::StringView::npos
                            ? text.size() - cursor : end - cursor);
            if (!token.empty() && (token.front() == '+' ||
                                   token.front() == '-')) {
                token.remove_prefix(1);
            }
            if (!token.empty() &&
                lowerAscii(container::String{token}) != "none") {
                bits.emplace_back(token);
            }
            if (end == container::StringView::npos) break;
            cursor = end + 1;
        }
    };
    appendExisting(destination);
    const auto findBit = [&bits](container::StringView sought) {
        const container::String folded = lowerAscii(container::String{sought});
        return std::find_if(bits.begin(), bits.end(),
                            [&folded](const container::String& bit) {
                                return lowerAscii(bit) == folded;
                            });
    };

    bool foundNormal = false;
    bool foundEdit = false;
    size_t cursor = 0;
    while (cursor < authored.size()) {
        cursor = authored.find_first_not_of(" \t\r\n,", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = authored.find_first_of(" \t\r\n,", cursor);
        container::StringView token = authored.substr(
            cursor, end == container::StringView::npos
                        ? authored.size() - cursor : end - cursor);
        const container::String folded = lowerAscii(container::String{token});
        if (folded == "none") {
            if (foundNormal || foundEdit) return false;
            bits.clear();
            cursor = authored.size();
            break;
        }
        const bool add = !token.empty() && token.front() == '+';
        const bool subtract = !token.empty() && token.front() == '-';
        if (add || subtract) {
            if (foundNormal) return false;
            foundEdit = true;
            token.remove_prefix(1);
            if (token.empty()) return false;
            const auto found = findBit(token);
            if (subtract) {
                if (found != bits.end()) bits.erase(found);
            } else if (found == bits.end()) {
                bits.emplace_back(token);
            }
        } else {
            if (foundEdit) return false;
            if (!foundNormal) bits.clear();
            foundNormal = true;
            if (findBit(token) == bits.end()) bits.emplace_back(token);
        }
        if (end == container::StringView::npos) break;
        cursor = end + 1;
    }

    destination.clear();
    for (const container::String& bit : bits) {
        if (!destination.empty()) destination.push_back(' ');
        destination += bit;
    }
    return true;
}

bool parseBool(container::StringView value) {
    const container::String lower = lowerAscii(container::String(value));
    return lower == "yes" || lower == "true" || lower == "1";
}

float parseFloat(container::StringView value) {
    return parseContentFloatOr(value, {
        .source = __FILE__, .block = "Object", .field = "Real"});
}

uint32_t parseUnsigned(container::StringView value) {
    const container::String owned(value);
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(owned.c_str(), &end, 10);
    if (end == owned.c_str()) return 0;
    return parsed > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(parsed);
}

int32_t parseSigned(container::StringView value) {
    const container::String owned(value);
    char* end = nullptr;
    const long long parsed = std::strtoll(owned.c_str(), &end, 10);
    if (end == owned.c_str()) return 0;
    return parsed > std::numeric_limits<int32_t>::max()
        ? std::numeric_limits<int32_t>::max()
        : parsed < std::numeric_limits<int32_t>::min()
            ? std::numeric_limits<int32_t>::min()
            : static_cast<int32_t>(parsed);
}

[[nodiscard]] std::optional<container::Array<int32_t, 4>>
parseVeterancyIntList(container::StringView value) {
    container::Array<int32_t, 4> result{};
    size_t cursor = 0;
    for (size_t index = 0; index < result.size(); ++index) {
        cursor = value.find_first_not_of(" \t,", cursor);
        if (cursor == container::StringView::npos) return std::nullopt;
        const size_t end = value.find_first_of(" \t,", cursor);
        const container::StringView token = value.substr(cursor, end - cursor);
        const container::String owned{token};
        char* parsedEnd = nullptr;
        const long long parsed = std::strtoll(owned.c_str(), &parsedEnd, 10);
        if (parsedEnd == owned.c_str() || *parsedEnd != '\0' ||
            parsed < std::numeric_limits<int32_t>::min() ||
            parsed > std::numeric_limits<int32_t>::max()) {
            return std::nullopt;
        }
        result[index] = static_cast<int32_t>(parsed);
        cursor = end == container::StringView::npos ? value.size() : end;
    }
    // RefCode's fixed-size list parser consumes LEVEL_COUNT entries and
    // ignores any authored tail. Some shipped ZH objects contain five values
    // even though the runtime veterancy array has four slots.
    return result;
}

[[nodiscard]] container::Vector<container::String> parseNameList(
    container::StringView value) {
    container::Vector<container::String> result;
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t\r\n,", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t\r\n,", cursor);
        result.emplace_back(value.substr(
            cursor, end == container::StringView::npos
                ? value.size() - cursor : end - cursor));
        if (end == container::StringView::npos) break;
        cursor = end + 1u;
    }
    return result;
}

[[nodiscard]] uint64_t allDamageTypesMask() noexcept {
    constexpr size_t count = static_cast<size_t>(DamageType::COUNT);
    static_assert(count < 64);
    return (uint64_t{1} << count) - 1u;
}

std::optional<uint64_t> parseDamageTypeMask(container::StringView value) {
    uint64_t mask = allDamageTypesMask();
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t,|", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t,|", cursor);
        container::StringView token = value.substr(cursor, end - cursor);
        cursor = end;
        if (token.empty()) continue;
        const container::String lower = lowerAscii(container::String(token));
        if (lower == "all") {
            mask = allDamageTypesMask();
            continue;
        }
        if (lower == "none") {
            mask = 0;
            continue;
        }
        const char operation = token.front();
        if (operation != '+' && operation != '-') return std::nullopt;
        token.remove_prefix(1);
        const std::optional<DamageType> damageType = tryParseDamageType(token);
        if (!damageType) return std::nullopt;
        const uint64_t bit = uint64_t{1} << static_cast<uint32_t>(*damageType);
        if (operation == '+') mask |= bit;
        else mask &= ~bit;
    }
    return mask;
}


} // namespace game::detail

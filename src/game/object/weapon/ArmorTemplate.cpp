#include "core/container/container_types.h"
#include "ArmorTemplate.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "debug/debug.h"
#include <algorithm>
#include <optional>

namespace game {
namespace {

[[nodiscard]] bool applyArmorCoefficient(
    ArmorTemplate& target, container::StringView source) noexcept {
    source = container::trimAsciiView(source);
    const size_t separator = source.find_first_of(" \t\r\n");
    if (separator == container::StringView::npos) return false;

    const container::StringView damageName = source.substr(0, separator);
    container::StringView percent =
        container::trimAsciiView(source.substr(separator + 1));
    const size_t percentEnd = percent.find_first_of(" \t\r\n");
    if (percentEnd != container::StringView::npos) {
        percent = percent.substr(0, percentEnd);
    }
    if (!percent.empty() && percent.back() == '%') percent.remove_suffix(1);
    const std::optional<float> authored =
        game::parseContentFloat(percent, {
            .source = __FILE__, .block = "Armor", .field = "Percent"});
    if (!authored) return false;

    // INI::scanPercentToReal always divides by 100; the '%' character is a
    // token separator, not the switch between percent and scalar syntax.
    const math::q32_32 coefficient{
        std::max(0.0f, *authored / 100.0f)};
    if (container::asciiEqualIgnoreCase(damageName, "Default")) {
        target.armor.fill(coefficient);
        return true;
    }
    const std::optional<DamageType> damage = tryParseDamageType(damageName);
    if (!damage) return false;
    target.armor[static_cast<size_t>(*damage)] = coefficient;
    return true;
}

} // namespace

ArmorStore& ArmorStore::instance() {
    static ArmorStore s_instance;
    return s_instance;
}

void ArmorStore::clear() {
    m_armors.clear();
}

bool ArmorStore::loadFromIni(
    const container::String& filePath, ini::LegacyIniLoadType) {
    GeneralsIniParser parser;
    if (!parser.parseFile(filePath)) {
        return false;
    }

    for (auto& block : parser.blocks()) {
        if (block.type != "Armor") continue;

        if (block.name.empty()) continue;

        // ArmorStore::parseArmorDefinition clears the complete coefficient
        // table for every declaration, including CreateOverrides sources.
        // A later Armor is therefore a full replacement, not a sparse patch.
        ArmorTemplate tmpl;
        tmpl.name = block.name;

        for (auto& [key, val] : block.values) {
            if (key == "Armor" && !applyArmorCoefficient(tmpl, val)) {
                TD_LOG_WARN(
                    "[ArmorStore] Ignored invalid Armor coefficient '{}' in '{}'",
                    val, block.name);
            }
        }

        tmpl.loaded = true;
        m_armors.insert_or_assign(tmpl.name, std::move(tmpl));
    }

    TD_LOG_INFO("[ArmorStore] Loaded {} armor templates from {}", m_armors.size(), filePath);
    return true;
}

const ArmorTemplate* ArmorStore::find(const container::String& name) const {
    auto it = m_armors.find(name);
    return it != m_armors.end() ? &it->second : nullptr;
}

} // namespace game

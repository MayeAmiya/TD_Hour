#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/plan/combat/ObjectTransitionDamageFxPlanTypes.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace game {
namespace {

using container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView trim(container::StringView value) noexcept {
    const size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == container::StringView::npos) return {};
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

[[nodiscard]] container::StringView moduleClass(
    const ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

[[nodiscard]] const container::String* moduleValueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto iterator = module.values.rbegin();
         iterator != module.values.rend(); ++iterator) {
        if (asciiEqualIgnoreCase(iterator->first, key)) return &iterator->second;
    }
    return nullptr;
}

[[nodiscard]] container::Vector<container::StringView> splitTokens(
    container::StringView value) {
    container::Vector<container::StringView> tokens;
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t\r\n:", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t\r\n:", cursor);
        tokens.push_back(value.substr(
            cursor, end == container::StringView::npos
                        ? value.size() - cursor : end - cursor));
        if (end == container::StringView::npos) break;
        cursor = end + 1;
    }
    return tokens;
}

[[nodiscard]] std::optional<float> parseFloat(
    container::StringView value) noexcept {
    return parseContentFloat(value, {
        .source = __FILE__, .block = "Object",
        .module = "TransitionDamageFX", .field = "Real"});
}

[[nodiscard]] std::optional<bool> parseBool(
    container::StringView value) noexcept {
    if (asciiEqualIgnoreCase(value, "YES") ||
        asciiEqualIgnoreCase(value, "TRUE") || value == "1") return true;
    if (asciiEqualIgnoreCase(value, "NO") ||
        asciiEqualIgnoreCase(value, "FALSE") || value == "0") return false;
    return std::nullopt;
}

[[nodiscard]] constexpr uint64_t allDamageTypes() noexcept {
    constexpr uint8_t count = static_cast<uint8_t>(DamageType::COUNT);
    if constexpr (count >= 64) return std::numeric_limits<uint64_t>::max();
    return (uint64_t{1} << count) - 1u;
}

[[nodiscard]] std::optional<uint64_t> parseDamageTypeMask(
    container::StringView value) {
    // INI::parseDamageTypeFlags starts from the field's constructor default
    // (ALL) and accepts only ALL/NONE or explicit +/- mutations.
    uint64_t mask = allDamageTypes();
    bool sawToken = false;
    for (container::StringView token : splitTokens(value)) {
        if (asciiEqualIgnoreCase(token, "ALL")) {
            mask = allDamageTypes();
            sawToken = true;
            continue;
        }
        if (asciiEqualIgnoreCase(token, "NONE")) {
            mask = 0;
            sawToken = true;
            continue;
        }
        if (token.empty() ||
            (token.front() != '+' && token.front() != '-')) {
            return std::nullopt;
        }
        const bool remove = token.front() == '-';
        token.remove_prefix(1);
        const std::optional<DamageType> parsed = tryParseDamageType(token);
        if (!parsed) return std::nullopt;
        const uint8_t bitIndex = static_cast<uint8_t>(*parsed);
        if (bitIndex >= 64) return std::nullopt;
        const uint64_t bit = uint64_t{1} << bitIndex;
        if (remove) mask &= ~bit;
        else mask |= bit;
        sawToken = true;
    }
    return sawToken ? std::optional<uint64_t>{mask} : std::nullopt;
}

[[nodiscard]] container::String diagnosticPrefix(const ModuleData& module) {
    if (!module.moduleTag.empty()) return module.moduleTag;
    if (!module.tag.empty()) return module.tag;
    return container::String{moduleClass(module)};
}

void appendDiagnostic(ObjectTransitionDamageFxPlan& plan,
                      const ModuleData& module, container::String message) {
    plan.diagnostics.push_back(
        diagnosticPrefix(module) + ": " + std::move(message));
}

[[nodiscard]] std::optional<ObjectTransitionDamageFxEntry> parseEntry(
    container::StringView value, ObjectTransitionDamageFxPayloadKind kind,
    uint8_t slot, bool& explicitNone) {
    explicitNone = false;
    const container::Vector<container::StringView> tokens = splitTokens(value);
    if (tokens.empty()) return std::nullopt;

    ObjectTransitionDamageFxEntry entry;
    entry.kind = kind;
    entry.slot = slot;
    size_t resourceLabel = 0;
    if (asciiEqualIgnoreCase(tokens[0], "BONE")) {
        if (tokens.size() != 6 ||
            !asciiEqualIgnoreCase(tokens[2], "RANDOMBONE")) {
            return std::nullopt;
        }
        const std::optional<bool> randomBone = parseBool(tokens[3]);
        if (!randomBone || tokens[1].empty()) return std::nullopt;
        entry.location.kind = ObjectTransitionDamageFxLocationKind::Bone;
        entry.location.boneName = container::String{tokens[1]};
        entry.location.randomBone = *randomBone;
        resourceLabel = 4;
    } else if (asciiEqualIgnoreCase(tokens[0], "LOC")) {
        if (tokens.size() != 9 || !asciiEqualIgnoreCase(tokens[1], "X") ||
            !asciiEqualIgnoreCase(tokens[3], "Y") ||
            !asciiEqualIgnoreCase(tokens[5], "Z")) {
            return std::nullopt;
        }
        const std::optional<float> x = parseFloat(tokens[2]);
        const std::optional<float> y = parseFloat(tokens[4]);
        const std::optional<float> z = parseFloat(tokens[6]);
        if (!x || !y || !z) return std::nullopt;
        entry.location.kind =
            ObjectTransitionDamageFxLocationKind::LocalCoordinate;
        entry.location.localPosition =
            engine::LogicFixedVec3::fromFloats(*x, *y, *z);
        resourceLabel = 7;
    } else {
        return std::nullopt;
    }

    const container::StringView expected =
        kind == ObjectTransitionDamageFxPayloadKind::FxList ? "FXLIST"
        : kind == ObjectTransitionDamageFxPayloadKind::ObjectCreationList
            ? "OCL" : "PSYS";
    if (!asciiEqualIgnoreCase(tokens[resourceLabel], expected) ||
        tokens[resourceLabel + 1].empty()) return std::nullopt;
    entry.resource = container::String{tokens[resourceLabel + 1]};
    if (asciiEqualIgnoreCase(entry.resource, "NONE")) {
        explicitNone = true;
        return std::nullopt;
    }
    return entry;
}

} // namespace

container::SharedPtr<const ObjectTransitionDamageFxPlan>
compileObjectTransitionDamageFxPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectTransitionDamageFxPlan>();
    constexpr container::Array<container::StringView, 3> statePrefixes{
        "Damaged", "ReallyDamaged", "Rubble"};

    for (const ModuleData& module : templateData.modules) {
        if (!asciiEqualIgnoreCase(moduleClass(module),
                                  "TransitionDamageFX")) continue;
        ObjectTransitionDamageFxRule rule;
        rule.authoredOrder = module.authoredOrder;
        rule.fxDamageTypes = allDamageTypes();
        rule.oclDamageTypes = allDamageTypes();
        rule.particleDamageTypes = allDamageTypes();

        const auto parseMask = [&](container::StringView field,
                                   uint64_t& output) {
            const container::String* value = moduleValueLast(module, field);
            if (!value) return;
            if (const std::optional<uint64_t> parsed =
                    parseDamageTypeMask(*value)) {
                output = *parsed;
            } else {
                appendDiagnostic(*plan, module,
                    container::String{field} +
                    " contains an unknown damage type");
            }
        };
        parseMask("DamageFXTypes", rule.fxDamageTypes);
        parseMask("DamageOCLTypes", rule.oclDamageTypes);
        parseMask("DamageParticleTypes", rule.particleDamageTypes);

        for (size_t stateOffset = 0; stateOffset < statePrefixes.size();
             ++stateOffset) {
            const size_t stateIndex = stateOffset + 1;
            for (size_t slotIndex = 0;
                 slotIndex < kTransitionDamageMaximumSlots; ++slotIndex) {
                const uint8_t slot = static_cast<uint8_t>(slotIndex);
                const container::String suffix =
                    std::to_string(slotIndex + 1);
                const auto append = [&](container::String field,
                                        ObjectTransitionDamageFxPayloadKind kind) {
                    const container::String* value =
                        moduleValueLast(module, field);
                    if (!value || trim(*value).empty()) return;
                    bool explicitNone = false;
                    const std::optional<ObjectTransitionDamageFxEntry> entry =
                        parseEntry(*value, kind, slot, explicitNone);
                    if (entry) {
                        rule.entries[stateIndex].push_back(*entry);
                    } else if (!explicitNone &&
                               !asciiEqualIgnoreCase(trim(*value), "NONE")) {
                        appendDiagnostic(*plan, module,
                            field + " has invalid TransitionDamageFX syntax");
                    }
                };
                const container::String prefix{statePrefixes[stateOffset]};
                append(prefix + "FXList" + suffix,
                       ObjectTransitionDamageFxPayloadKind::FxList);
                append(prefix + "OCL" + suffix,
                       ObjectTransitionDamageFxPayloadKind::ObjectCreationList);
                append(prefix + "ParticleSystem" + suffix,
                       ObjectTransitionDamageFxPayloadKind::ParticleSystem);
            }
        }
        plan->rules.push_back(std::move(rule));
    }
    if (plan->rules.empty() && plan->diagnostics.empty()) return {};
    return plan;
}

} // namespace game

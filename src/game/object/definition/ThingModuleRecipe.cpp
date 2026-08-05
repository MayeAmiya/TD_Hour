#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
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

[[nodiscard]] bool isObjectModuleBlock(container::StringView type) noexcept {
    return type == "Draw" || type == "Behavior" || type == "Update" ||
           type == "AI" || type == "Upgrade" || type == "ActiveBody" ||
           type == "StructureBody" || type == "ImmortalBody" ||
           type == "HighlanderBody" || type == "InactiveBody" ||
           type == "UndeadBody" || type == "HiveStructureBody" ||
           type == "Body" || type == "ClientUpdate" || type == "Module";
}

[[nodiscard]] bool isCombatSetBlock(container::StringView type) noexcept {
    return type == "WeaponSet" || type == "ArmorSet";
}

[[nodiscard]] bool isRecipeModuleWrapper(container::StringView type) noexcept {
    // These original sections change how a child module is merged into a
    // copied template. Stage-0 keeps their authored children and explicit
    // order; the full interface-mask conflict compiler can later use that
    // recipe metadata without reparsing the legacy INI source.
    return type == "InheritableModule" || type == "OverrideableByLikeKind";
}

[[nodiscard]] container::StringView moduleTagToken(const ModuleData& module) noexcept {
    if (!module.moduleTag.empty()) return module.moduleTag;
    if (module.type == "Body" || module.type == "Behavior" || module.type == "Update" ||
        module.type == "AI" || module.type == "Upgrade" || module.type == "Draw" ||
        module.type == "ClientUpdate" || module.type == "Module") {
        return tokensAfterFirst(module.tag);
    }
    return module.tag;
}

[[nodiscard]] container::StringView moduleClassToken(const ModuleData& module) noexcept {
    if (!module.moduleClass.empty()) return module.moduleClass;
    if (module.type == "Body" || module.type == "Behavior" || module.type == "Update" ||
        module.type == "AI" || module.type == "Upgrade" || module.type == "Draw" ||
        module.type == "ClientUpdate" || module.type == "Module") {
        return firstToken(module.tag);
    }
    return module.type;
}

[[nodiscard]] ModuleRecipeCategory moduleCategoryFor(const ModuleData& module) noexcept {
    if (module.type == "Draw") return ModuleRecipeCategory::Draw;
    if (module.type == "ClientUpdate") return ModuleRecipeCategory::ClientUpdate;
    if (isObjectModuleBlock(module.type)) return ModuleRecipeCategory::Behavior;
    return ModuleRecipeCategory::Unknown;
}

void populateModuleMetadata(ModuleData& module,
                            const std::optional<ObjectModuleCatalogEntry>& catalogEntry) {
    module.moduleClass = container::String(moduleClassToken(module));
    module.moduleTag = container::String(moduleTagToken(module));
    module.category = moduleCategoryFor(module);
    module.interfaceMask = ModuleRecipeInterfaceNone;
    module.interfaceMaskKnown = false;
    module.isAiModule = false;

    if (bodyKindFor(module)) {
        module.category = ModuleRecipeCategory::Behavior;
        module.interfaceMask = ModuleRecipeInterfaceBody;
        module.interfaceMaskKnown = true;
        return;
    }
    if (module.category == ModuleRecipeCategory::Draw) {
        module.interfaceMask = ModuleRecipeInterfaceDraw;
        module.interfaceMaskKnown = true;
        return;
    }
    if (module.category == ModuleRecipeCategory::ClientUpdate) {
        module.interfaceMask = ModuleRecipeInterfaceClientUpdate;
        module.interfaceMaskKnown = true;
        return;
    }

    // The old ModuleFactory had an exact registration table.  Use a frozen
    // data projection of that table rather than guessing from a class suffix
    // (for example, AutoHealBehavior is Update|Upgrade|Damage, not merely an
    // "Update" because of its spelling). Unknown mod classes remain opaque
    // and are diagnosed by the recipe compiler below.
    if (catalogEntry && catalogEntry->domain == ObjectModuleCatalogDomain::Behavior &&
        module.category == ModuleRecipeCategory::Behavior) {
        module.interfaceMask = catalogEntry->interfaceMask;
        module.interfaceMaskKnown = true;
        module.isAiModule = catalogEntry->isAiModule;
    }
}

[[nodiscard]] bool recipeOperationIsInheritable(ModuleRecipeOperation operation) noexcept {
    return operation == ModuleRecipeOperation::InheritableDefault;
}

[[nodiscard]] bool recipeOperationIsOverrideable(ModuleRecipeOperation operation) noexcept {
    return operation == ModuleRecipeOperation::OverrideableDefault;
}

[[nodiscard]] bool kindOfContains(container::StringView value, container::StringView sought) {
    for (container::StringView token : splitWhitespace(value)) {
        while (!token.empty() && (token.front() == '+' || token.front() == '-')) token.remove_prefix(1);
        if (lowerAscii(container::String(token)) == lowerAscii(container::String(sought))) return true;
    }
    return false;
}

[[nodiscard]] bool isOverrideableLikeKindCandidate(const ThingTemplate& templateData) {
    static constexpr container::Array<container::StringView, 10> disallowed = {
        "AIRCRAFT", "SHRUBBERY", "OPTIMIZED_TREE", "STRUCTURE", "DRAWABLE_ONLY",
        "MOB_NEXUS", "IGNORED_IN_GUI", "CLEARED_BY_BUILD", "DEFENSIVE_WALL", "BALLISTIC_MISSILE",
    };
    static constexpr container::Array<container::StringView, 3> additionalDisallowed = {
        "SUPPLY_SOURCE", "BOAT", "INERT",
    };
    static constexpr container::Array<container::StringView, 4> candidates = {
        "SCORE", "VEHICLE", "INFANTRY", "PORTABLE_STRUCTURE",
    };
    const bool explicitlyDisallowed = std::any_of(disallowed.begin(), disallowed.end(),
        [&templateData](container::StringView kind) { return kindOfContains(templateData.kindOf, kind); }) ||
        std::any_of(additionalDisallowed.begin(), additionalDisallowed.end(),
        [&templateData](container::StringView kind) { return kindOfContains(templateData.kindOf, kind); });
    if (explicitlyDisallowed) return false;
    return std::any_of(candidates.begin(), candidates.end(),
        [&templateData](container::StringView kind) { return kindOfContains(templateData.kindOf, kind); });
}

void deferInterfaceResolution(TemplateRecipeParseState& state, const ThingTemplate& templateData,
                              const ModuleData& incoming) {
    state.requiresInterfaceResolution = true;
    if (state.reportedUnresolvedInterface) return;
    state.reportedUnresolvedInterface = true;
    state.diagnostics.push_back({
        .severity = ObjectRecipeDiagnosticSeverity::Warning,
        .message = "retained copied module with unresolved interface while adding '" +
            container::String(moduleClassToken(incoming)) + "' to '" + templateData.name + "'",
    });
}

void markOpaqueModule(TemplateRecipeParseState& state, const ThingTemplate& templateData,
                      const ModuleData& module) {
    state.requiresInterfaceResolution = true;
    // This diagnostic is stronger than the later copied-parent conflict
    // detail: once the class itself is opaque, emitting both only obscures
    // the actionable module name in large modded object files.
    state.reportedUnresolvedInterface = true;
    state.diagnostics.push_back({
        .severity = ObjectRecipeDiagnosticSeverity::Warning,
        .message = "retained opaque module class '" + module.moduleClass + "' on '" +
            templateData.name + "'; no stock interface metadata is available",
    });
}

void resolveCopiedParentModuleConflicts(ThingAuthoringTemplate& templateData, const ModuleData& incoming,
                                        TemplateRecipeParseState& state) {
    // In a map/VFS overlay RefCode permits modules only through Add/Remove/
    // Replace and deliberately does not perform this default-interface
    // clearing pass. The base/reskin compiler does it for every added module.
    if (!state.copiedParentRecipe || state.overlayLoad) return;

    for (auto it = templateData.modules.begin(); it != templateData.modules.end();) {
        ModuleData& existing = *it;
        if (!existing.copiedFromParent) {
            ++it;
            continue;
        }
        // ModuleFactory stores Behavior, Draw and ClientUpdate in separate
        // collections. Their interface domains do not cross, so an unknown
        // Behavior mask cannot make a Draw replacement ambiguous.
        if (existing.category != incoming.category) {
            ++it;
            continue;
        }
        // Body is a single, syntactically constrained behavior interface:
        // a non-Body declaration is rejected when it tries to name a Body
        // class, so it cannot collide with an incoming valid Body merely
        // because its broader behavior mask is still opaque.
        if ((incoming.interfaceMask & ModuleRecipeInterfaceBody) != 0 &&
            !bodyKindFor(existing)) {
            ++it;
            continue;
        }
        if ((existing.interfaceMask & ModuleRecipeInterfaceBody) != 0 &&
            !bodyKindFor(incoming)) {
            ++it;
            continue;
        }

        const bool bothMasksKnown = existing.interfaceMaskKnown && incoming.interfaceMaskKnown;
        const bool masksOverlap = bothMasksKnown &&
            (existing.interfaceMask & incoming.interfaceMask) != ModuleRecipeInterfaceNone;
        const bool sameClass = existing.category == incoming.category &&
            moduleClassToken(existing) == moduleClassToken(incoming);

        if (recipeOperationIsInheritable(existing.recipeOperation)) {
            // The original has one content-specific exception: this default
            // auto-heal is useless on non-trainable templates.
            if (moduleTagToken(existing) == "ModuleTag_DefaultAutoHealBehavior" &&
                !templateData.isTrainable) {
                it = templateData.modules.erase(it);
            } else {
                ++it;
            }
            continue;
        }

        if (recipeOperationIsOverrideable(existing.recipeOperation)) {
            if (bothMasksKnown && !masksOverlap) {
                ++it;
            } else if (sameClass || !isOverrideableLikeKindCandidate(templateData)) {
                it = templateData.modules.erase(it);
            } else {
                if (!bothMasksKnown) deferInterfaceResolution(state, templateData, incoming);
                ++it;
            }
            continue;
        }

        if (masksOverlap) {
            it = templateData.modules.erase(it);
        } else {
            if (!bothMasksKnown) deferInterfaceResolution(state, templateData, incoming);
            ++it;
        }
    }
}

[[nodiscard]] bool moduleTagIsUnique(const ThingTemplate& templateData, container::StringView tag) {
    return std::none_of(templateData.modules.begin(), templateData.modules.end(),
        [tag](const ModuleData& module) { return moduleTagToken(module) == tag; });
}

[[nodiscard]] std::optional<ModuleData> removeRecipeModule(ThingAuthoringTemplate& templateData,
                                                            container::StringView requestedTag) {
    const auto found = std::find_if(templateData.modules.begin(), templateData.modules.end(),
        [requestedTag](const ModuleData& module) {
            return moduleTagToken(module) == requestedTag;
        });
    if (found == templateData.modules.end()) return std::nullopt;
    ModuleData result = std::move(*found);
    templateData.modules.erase(found);
    return result;
}

struct ReplacementRequirement final {
    container::String moduleClass;
    container::String replacedTag;
};

void applyObjectModule(ThingAuthoringTemplate& templateData, const IniBlock& child,
                        TemplateRecipeParseState& state,
                        ModuleRecipeOperation operation = ModuleRecipeOperation::Direct,
                        const std::optional<ReplacementRequirement>& replacement = std::nullopt) {
    if (child.type == "RemoveModule") {
        const container::StringView target = firstToken(child.name);
        if (target.empty()) {
            state.diagnostics.push_back({
                .severity = ObjectRecipeDiagnosticSeverity::Error,
                .message = "RemoveModule has no target tag",
            });
        } else if (!removeRecipeModule(templateData, target)) {
            state.diagnostics.push_back({
                // RefCode applies Map.ini RemoveModule against the effective
                // object after all VFS layers are merged.  A target can be
                // absent when an optional/modded base branch is not present;
                // that operation is a deterministic no-op, not a reason to
                // reject the whole map session.
                .severity = ObjectRecipeDiagnosticSeverity::Warning,
                .message = "RemoveModule target not found; ignored: " +
                    container::String(target),
            });
        }
        return;
    }
    if (child.type == "ReplaceModule") {
        const container::StringView target = firstToken(child.name);
        const std::optional<ModuleData> removed = target.empty()
            ? std::nullopt
            : removeRecipeModule(templateData, target);
        if (target.empty() || !removed) {
            state.diagnostics.push_back({
                .severity = target.empty()
                    ? ObjectRecipeDiagnosticSeverity::Error
                    : ObjectRecipeDiagnosticSeverity::Warning,
                .message = target.empty()
                    ? "ReplaceModule has no target tag"
                    : "ReplaceModule target not found; ignored: " +
                        container::String(target),
            });
            return;
        }
        const uint32_t firstReplacementOrder = state.nextAuthoredOrder;
        const ReplacementRequirement requirement{
            .moduleClass = container::String(moduleClassToken(*removed)),
            .replacedTag = container::String(moduleTagToken(*removed)),
        };
        for (const IniBlock& nested : child.children) {
            applyObjectModule(templateData, nested, state, ModuleRecipeOperation::Replaced, requirement);
        }
        const bool hasReplacement = std::any_of(templateData.modules.begin(), templateData.modules.end(),
            [firstReplacementOrder](const ModuleData& module) {
                return module.recipeOperation == ModuleRecipeOperation::Replaced &&
                    module.authoredOrder >= firstReplacementOrder;
            });
        if (!hasReplacement) {
            state.diagnostics.push_back({
                .severity = ObjectRecipeDiagnosticSeverity::Error,
                .message = "ReplaceModule did not provide a valid replacement for tag " + container::String(target),
            });
        }
        return;
    }
    if (child.type == "AddModule") {
        for (const IniBlock& nested : child.children) {
            applyObjectModule(templateData, nested, state, ModuleRecipeOperation::Added, replacement);
        }
        return;
    }
    if (isRecipeModuleWrapper(child.type)) {
        const ModuleRecipeOperation nestedOperation = child.type == "InheritableModule"
            ? ModuleRecipeOperation::InheritableDefault
            : ModuleRecipeOperation::OverrideableDefault;
        for (const IniBlock& nested : child.children) {
            applyObjectModule(templateData, nested, state, nestedOperation, replacement);
        }
        return;
    }
    if (!isObjectModuleBlock(child.type)) return;

    ModuleData module = makeModuleData(child);
    module.recipeOperation = operation;
    module.authoredOrder = state.nextAuthoredOrder++;
    if (isIgnoredLegacyObjectModule(moduleClassToken(module))) {
        // This declaration has no gameplay or presentation meaning in
        // GeneralsTD. Drop it before opaque-module/interface handling so
        // unmodified Zero Hour INI remains compatible without creating a
        // runtime module, archetype rule, entitlement flag or warning spam.
        return;
    }
    const std::optional<ObjectModuleCatalogEntry> catalogEntry =
        findObjectModuleCatalogEntry(moduleClassToken(module));
    populateModuleMetadata(module, catalogEntry);
    if (catalogEntry &&
        ((catalogEntry->domain == ObjectModuleCatalogDomain::Behavior &&
          module.category != ModuleRecipeCategory::Behavior) ||
         (catalogEntry->domain == ObjectModuleCatalogDomain::ClientUpdate &&
          module.category != ModuleRecipeCategory::ClientUpdate))) {
        state.diagnostics.push_back({
            .severity = ObjectRecipeDiagnosticSeverity::Error,
            .message = "module class '" + module.moduleClass + "' was declared in the wrong module domain",
        });
        return;
    }
    const std::optional<ObjectBodyKind> bodyKind = bodyKindFor(module);
    if (child.type == "Body" && !bodyKind) {
        state.diagnostics.push_back({
            .severity = ObjectRecipeDiagnosticSeverity::Error,
            .message = "Body declaration does not name a Body module: '" + module.moduleClass + "'",
        });
        return;
    }
    if (child.type == "Behavior" && bodyKind) {
        state.diagnostics.push_back({
            .severity = ObjectRecipeDiagnosticSeverity::Error,
            .message = "Behavior declaration cannot name a Body module: '" + module.moduleClass + "'",
        });
        return;
    }
    if (module.moduleClass.empty() || module.moduleTag.empty()) {
        state.diagnostics.push_back({
            .severity = ObjectRecipeDiagnosticSeverity::Error,
            .message = "module declaration requires both class and unique tag",
        });
        return;
    }
    if (module.category == ModuleRecipeCategory::Behavior && !catalogEntry && !bodyKind) {
        markOpaqueModule(state, templateData, module);
    }
    if (replacement) {
        if (module.moduleClass != replacement->moduleClass) {
            state.diagnostics.push_back({
                .severity = ObjectRecipeDiagnosticSeverity::Error,
                .message = "ReplaceModule changed class for tag " + replacement->replacedTag,
            });
            return;
        }
        if (module.moduleTag == replacement->replacedTag) {
            state.diagnostics.push_back({
                .severity = ObjectRecipeDiagnosticSeverity::Error,
                .message = "ReplaceModule must assign a new tag for " + replacement->replacedTag,
            });
            return;
        }
    }

    resolveCopiedParentModuleConflicts(templateData, module, state);
    if (module.isAiModule) {
        // RefCode keeps at most one AI module: adding a later AIUpdate
        // removes any earlier AI module even when it was locally authored.
        std::erase_if(templateData.modules, [](const ModuleData& existing) {
            return existing.isAiModule;
        });
    }
    if (!moduleTagIsUnique(templateData, module.moduleTag)) {
        state.diagnostics.push_back({
            .severity = ObjectRecipeDiagnosticSeverity::Error,
            .message = "module tag is not unique within object: '" + module.moduleTag + "'",
        });
        return;
    }

    if (child.type == "Update") {
        for (const auto& [key, value] : child.values) {
            if (key == "Weapon") templateData.weapons.push_back(value);
        }
    }
    templateData.modules.push_back(std::move(module));
}

[[nodiscard]] bool isOverlayModuleOperation(container::StringView type) noexcept {
    return type == "AddModule" || type == "RemoveModule" || type == "ReplaceModule";
}

void applyObjectRecipeEntries(ThingAuthoringTemplate& templateData, const IniBlock& block,
                              TemplateRecipeParseState& state, bool reskin) {
    const auto applyChild = [&](const IniBlock& child) {
        // ArmorSet/WeaponSet is compiled as a separate typed immutable
        // projection after this general recipe pass. It is not a legacy
        // ModuleFactory declaration, so neither the module-only VFS-overlay
        // rule nor applyObjectModule may discard it here.
        if (isCombatSetBlock(child.type)) {
            if (reskin) {
                state.diagnostics.push_back({
                    .severity = ObjectRecipeDiagnosticSeverity::Error,
                    .message = "ObjectReskin cannot declare '" + child.type + "'",
                });
            }
            return;
        }
        if (child.type == "UnitSpecificSounds") {
            if (reskin) {
                state.diagnostics.push_back({
                    .severity = ObjectRecipeDiagnosticSeverity::Error,
                    .message =
                        "ObjectReskin cannot declare 'UnitSpecificSounds'",
                });
                return;
            }
            for (const auto& [semanticName, eventName] : child.values) {
                if (!semanticName.empty()) {
                    templateData.unitSpecificSounds.insert_or_assign(
                        semanticName, eventName);
                }
            }
            return;
        }
        if (child.type == "UnitSpecificFX") {
            if (reskin) {
                state.diagnostics.push_back({
                    .severity = ObjectRecipeDiagnosticSeverity::Error,
                    .message =
                        "ObjectReskin cannot declare 'UnitSpecificFX'",
                });
                return;
            }
            for (const auto& [semanticName, fxListName] : child.values) {
                if (!semanticName.empty()) {
                    templateData.unitSpecificFx.insert_or_assign(
                        semanticName, fxListName);
                }
            }
            return;
        }
        if (child.type == "Prerequisites") {
            if (reskin) {
                state.diagnostics.push_back({
                    .severity = ObjectRecipeDiagnosticSeverity::Error,
                    .message = "ObjectReskin cannot declare 'Prerequisites'",
                });
                return;
            }
            if (!state.authoredPrerequisites) {
                templateData.prerequisiteObjectAlternatives.clear();
                templateData.prerequisiteSciences.clear();
                state.authoredPrerequisites = true;
            }
            for (const auto& [key, value] : child.values) {
                if (key == "Object") {
                    container::Vector<container::String> alternatives =
                        parseNameList(value);
                    if (!alternatives.empty()) {
                        templateData.prerequisiteObjectAlternatives.push_back(
                            std::move(alternatives));
                    }
                } else if (key == "Science") {
                    container::Vector<container::String> sciences =
                        parseNameList(value);
                    templateData.prerequisiteSciences.insert(
                        templateData.prerequisiteSciences.end(),
                        std::make_move_iterator(sciences.begin()),
                        std::make_move_iterator(sciences.end()));
                }
            }
            return;
        }
        if (reskin && child.type != "Draw") {
            state.diagnostics.push_back({
                .severity = ObjectRecipeDiagnosticSeverity::Error,
                .message = "ObjectReskin cannot declare '" + child.type + "'",
            });
            return;
        }
        if (state.overlayLoad && !isOverlayModuleOperation(child.type)) {
            if (state.strictCreateOverrides) {
                state.diagnostics.push_back({
                    .severity = ObjectRecipeDiagnosticSeverity::Error,
                    .message = "CreateOverrides requires module '" +
                        child.type +
                        "' to be inside AddModule/RemoveModule/ReplaceModule",
                });
                return;
            }
            // A VFS layer is not necessarily RefCode's Map.ini
            // INI_LOAD_CREATE_OVERRIDES mode. The shipped base/ZH/localised
            // archives contain complete same-name Object declarations in
            // higher-priority layers, written with ordinary Draw/Body/
            // Behavior blocks. Treat the first such block as a full recipe
            // patch: preserve copied scalar data, then apply the usual
            // copied-parent interface resolution instead of rejecting all
            // modules and making campaign/skirmish GameData loading fail.
            // Genuine Add/Remove/Replace-only overlays retain their stricter
            // path because this branch is never entered for them.
            state.overlayLoad = false;
        }
        applyObjectModule(templateData, child, state);
    };

    // Hand-authored tool fixtures may still construct IniBlock directly.
    // Parsed content always has entries, but retain deterministic fallback
    // rather than silently ignoring those fixtures or third-party callers.
    if (block.entries.empty()) {
        for (const auto& [key, value] : block.values) {
            applyObjectField(templateData, key, value, state, reskin);
        }
        for (const IniBlock& child : block.children) applyChild(child);
        return;
    }

    for (const IniEntry& entry : block.entries) {
        if (entry.kind == IniEntryKind::Value) {
            if (entry.index >= block.values.size()) {
                state.diagnostics.push_back({
                    .severity = ObjectRecipeDiagnosticSeverity::Error,
                    .message = "INI value-order index is invalid",
                });
                continue;
            }
            const auto& [key, value] = block.values[entry.index];
            applyObjectField(templateData, key, value, state, reskin);
            continue;
        }
        if (entry.index >= block.children.size()) {
            state.diagnostics.push_back({
                .severity = ObjectRecipeDiagnosticSeverity::Error,
                .message = "INI child-order index is invalid",
            });
            continue;
        }
        applyChild(block.children[entry.index]);
    }
}

} // namespace game::detail

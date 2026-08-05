#pragma once

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/ThingFactory.h"
#include "CombatProfile.h"
#include "core/data/ini/GeneralsIniParser.h"

namespace game::detail {

struct TemplateRecipeParseState final {
    bool authoredLocomotor = false;
    bool authoredPrerequisites = false;
    bool copiedParentRecipe = false;
    bool overlayLoad = false;
    bool strictCreateOverrides = false;
    bool requiresInterfaceResolution = false;
    bool reportedUnresolvedInterface = false;
    uint32_t nextAuthoredOrder = 0;
    container::Vector<ObjectRecipeDiagnostic> diagnostics;
};

enum class RecipeLoadMode : uint8_t {
    Base,
    Overlay,
    StrictCreateOverrides,
};

ModuleData makeModuleData(const IniBlock& block);
const container::String* firstValue(const ModuleData& module,
                                    container::StringView key);
container::String lowerAscii(container::String value);
container::StringView firstToken(container::StringView value) noexcept;
container::StringView tokensAfterFirst(container::StringView value) noexcept;
bool hasAsciiToken(container::StringView value,
                   container::StringView expected);
bool applyLegacyBitMaskText(container::String& destination,
                            container::StringView authored);
bool parseBool(container::StringView value);
float parseFloat(container::StringView value);
uint32_t parseUnsigned(container::StringView value);
int32_t parseSigned(container::StringView value);
std::optional<container::Array<int32_t, 4>> parseVeterancyIntList(
    container::StringView value);
container::Vector<container::String> parseNameList(container::StringView value);
std::optional<uint64_t> parseDamageTypeMask(container::StringView value);

uint64_t objectRecipeFingerprint(const ThingTemplate& templateData,
                                 const CombatProfile* combatProfile) noexcept;

container::Vector<container::StringView> splitWhitespace(
    container::StringView value);
void appendLocomotorBinding(ThingAuthoringTemplate& templateData,
                            container::StringView value);
std::optional<ObjectBodyKind> bodyKindFor(const ModuleData& module);
void applyBodyModule(ObjectBodyAuthoringTemplate& body, ObjectBodyKind kind,
                     const ModuleData& module);

std::optional<ModelAnimationCandidate> parseModelAnimationCandidate(
    container::StringView authored, bool idle);
container::Vector<container::StringView> whitespaceTokens(
    container::StringView value);
container::String normalizedOptionalName(container::StringView authored);
void appendVisualRules(
    const ModuleData& draw,
    container::Vector<ModelConditionVisualRule>& conditionVisuals,
    container::Vector<ModelConditionTransitionRule>& transitions);
const container::String* findDefaultModel(const ModuleData& module);

container::StringView moduleTagToken(const ModuleData& module) noexcept;
container::StringView moduleClassToken(const ModuleData& module) noexcept;
bool isCombatSetBlock(container::StringView type) noexcept;
bool kindOfContains(container::StringView value, container::StringView sought);
void applyObjectField(ThingAuthoringTemplate& templateData, container::StringView key,
                      container::StringView value,
                      TemplateRecipeParseState& state, bool reskin);
void applyObjectRecipeEntries(ThingAuthoringTemplate& templateData,
                              const IniBlock& block,
                              TemplateRecipeParseState& state, bool reskin);

std::optional<std::pair<container::String, container::String>>
parseObjectReskinHeader(container::StringView value);
void rebuildBodyAndDrawProjection(ThingAuthoringTemplate& templateData,
                                  TemplateRecipeParseState& state);
void markModulesCopiedFromParent(ThingTemplate& templateData);

} // namespace game::detail

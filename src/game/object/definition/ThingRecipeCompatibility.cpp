#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
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

namespace game {
bool legacyThingTemplatesEquivalent(
    const ThingTemplate& left, const ThingTemplate& right) noexcept {
    constexpr auto equalName = container::asciiEqualIgnoreCase;
    if (equalName(left.name, right.name)) return true;

    const bool leftIsReskinOfRight =
        !left.legacyReskinRootName.empty() &&
        equalName(left.legacyReskinRootName, right.name);
    const bool rightIsReskinOfLeft =
        !right.legacyReskinRootName.empty() &&
        equalName(right.legacyReskinRootName, left.name);
    const bool reskinSiblings =
        !left.legacyReskinRootName.empty() &&
        !right.legacyReskinRootName.empty() &&
        equalName(left.legacyReskinRootName,
                  right.legacyReskinRootName);
    if (leftIsReskinOfRight || rightIsReskinOfLeft || reskinSiblings)
        return true;

    const auto lists = [&equalName](const ThingTemplate& source,
                                    const ThingTemplate& candidate) {
        return std::any_of(
            source.buildVariations.begin(), source.buildVariations.end(),
            [&equalName, &candidate](container::StringView variation) {
                return equalName(variation, candidate.name);
            });
    };
    return lists(left, right) || lists(right, left);
}


} // namespace game

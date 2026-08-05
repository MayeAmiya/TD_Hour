#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingFactory.h"
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
using namespace detail;

ThingFactory& ThingFactory::instance() {
    static ThingFactory s_instance;
    return s_instance;
}

void ThingFactory::clear() {
    m_things.clear();
    m_archetypes.clear();
    m_derivedMetadataFinalized = false;
}

const ThingAuthoringTemplate* ThingFactory::find(const container::String& name) const {
    auto it = m_things.find(name);
    return it != m_things.end() ? &it->second : nullptr;
}

container::SharedPtr<const ObjectArchetype> ThingFactory::findArchetype(const container::String& name) const {
    const auto found = m_archetypes.find(name);
    return found == m_archetypes.end() ? container::SharedPtr<const ObjectArchetype>{} : found->second;
}


} // namespace game

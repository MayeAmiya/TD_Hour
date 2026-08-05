#include "ThingRecipeDetail.h"
#include "game/object/definition/ObjectArchetype.h"

namespace game {

using namespace detail;

void ThingFactory::finalizeDerivedMetadata() {
    if (m_derivedMetadataFinalized) return;
    for (auto& [name, thing] : m_things) {
        static_cast<void>(name);
        thing.isBuildFacility = kindOfContains(thing.kindOf, "COMMANDCENTER");
    }
    for (const auto& [name, consumer] : m_things) {
        static_cast<void>(name);
        for (const auto& alternatives : consumer.prerequisiteObjectAlternatives) {
            for (const container::String& prerequisite : alternatives) {
                // ProductionPrerequisite::resolveNames uses the ordinary
                // case-sensitive ThingFactory lookup.  Do not leak the
                // case-insensitive BuildVariations rule into this domain.
                const auto found = m_things.find(prerequisite);
                if (found != m_things.end())
                    found->second.isBuildFacility = true;
            }
        }
    }

    // Archetypes were compiled incrementally while INI layers loaded. Rebind
    // only their immutable template value/fingerprint now that reverse
    // prerequisite classification can see the complete content universe;
    // every typed module plan remains shared and unchanged.
    for (const auto& [name, thing] : m_things) {
        const auto found = m_archetypes.find(name);
        if (found == m_archetypes.end() || !found->second) continue;
        auto replacement = std::make_shared<ObjectArchetype>(*found->second);
        replacement->templateData = thing;
        replacement->recipeFingerprint = objectRecipeFingerprint(
            replacement->templateData, replacement->combatProfile.get());
        found->second = std::move(replacement);
    }
    m_derivedMetadataFinalized = true;
}

} // namespace game

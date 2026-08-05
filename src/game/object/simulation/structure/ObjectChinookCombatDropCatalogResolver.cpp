#include "game/object/simulation/structure/ObjectChinookCombatDropResolver.h"

#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/terrain/TerrainLogic.h"

namespace engine {
namespace {

[[nodiscard]] container::String numberedBoneName(
    container::StringView prefix, size_t ordinal) {
    container::String result(prefix);
    result.push_back(static_cast<char>('0' + ordinal / 10u));
    result.push_back(static_cast<char>('0' + ordinal % 10u));
    return result;
}

class TerrainCombatDropSurfaceResolver final
    : public ObjectChinookCombatDropSurfaceResolver {
public:
    explicit TerrainCombatDropSurfaceResolver(
        const game::terrain::TerrainLogic& terrain) noexcept
        : m_terrain(terrain) {}

    [[nodiscard]] math::q32_32 surfaceHeight(
        const LogicFixedVec3& ropeStart) const noexcept override {
        // Destroyed bridge sections are removed from TerrainLogic's elevated
        // surface set, matching onlyHealthyBridges=true in ChinookAIUpdate.
        const game::terrain::TerrainPathfindLayerId layer =
            m_terrain.highestPathfindLayerAtRaw(
                ropeStart.x.raw(), ropeStart.y.raw(), ropeStart.z.raw());
        return math::q32_32::from_raw(
            m_terrain.pathfindLayerHeightRawAt(
                layer, ropeStart.x.raw(), ropeStart.y.raw())
                .value_or(m_terrain.groundHeightRaw(
                    ropeStart.x.raw(), ropeStart.y.raw())));
    }

private:
    const game::terrain::TerrainLogic& m_terrain;
};

} // namespace

std::optional<ObjectChinookCombatDropBeginRequest>
resolveObjectChinookCombatDropBeginFromPristineBones(
    const ObjectChinookCombatDropBeginResolveInput& input,
    const ObjectChinookCombatDropPristineBones& bones,
    const game::terrain::TerrainLogic& terrain) {
    return resolveObjectChinookCombatDropBeginFromPristineBones(
        input, bones, TerrainCombatDropSurfaceResolver{terrain});
}

std::optional<ObjectChinookCombatDropBeginRequest>
resolveObjectChinookCombatDropBegin(
    const ObjectChinookCombatDropBeginResolveInput& input,
    const game::W3dPristineBoneCatalog& catalog,
    const game::terrain::TerrainLogic& terrain) {
    if (!catalog.isLoaded() || input.archetypeName.empty()) {
        return std::nullopt;
    }
    ObjectChinookCombatDropPristineBones bones;
    for (size_t index = 0; index < kMaximumChinookCombatDropBones; ++index) {
        const size_t ordinal = index + 1u;
        bones.ropeStarts[index] = catalog.find(
            input.archetypeName, input.visualRuleIndex,
            numberedBoneName("RopeStart", ordinal));
        bones.dropStarts[index] = catalog.find(
            input.archetypeName, input.visualRuleIndex,
            numberedBoneName("RopeEnd", ordinal));
    }
    return resolveObjectChinookCombatDropBeginFromPristineBones(
        input, bones, terrain);
}

} // namespace engine

#include "W3dPristineBoneCatalog.h"

#include <algorithm>
#include <utility>

namespace game {
namespace {

[[nodiscard]] container::String lowerAscii(container::StringView value) {
    container::String result{value};
    for (char& character : result) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character + ('a' - 'A'));
        }
    }
    return result;
}

} // namespace

void W3dPristineBoneCatalog::clear() {
    m_poses.clear();
    m_bindings.clear();
    m_diagnostics.clear();
    m_sourceFingerprint = 0;
    m_loaded = false;
}

std::optional<data::w3d::FixedRigidTransform>
W3dPristineBoneCatalog::find(
    container::StringView archetypeName, size_t visualRuleIndex,
    container::StringView boneName) const {
    if (!m_loaded || archetypeName.empty() || boneName.empty()) {
        return std::nullopt;
    }
    const auto binding = m_bindings.find(lowerAscii(archetypeName));
    if (binding == m_bindings.end() ||
        visualRuleIndex >= binding->second.poseByVisualRule.size()) {
        return std::nullopt;
    }
    const W3dPristinePoseId id =
        binding->second.poseByVisualRule[visualRuleIndex];
    if (!id || id.value > m_poses.size()) return std::nullopt;
    const W3dPristinePose& pose = m_poses[id.value - 1u];
    const container::String folded = lowerAscii(boneName);
    const auto found = std::lower_bound(
        pose.bones.begin(), pose.bones.end(), folded,
        [](const W3dPristineBoneEntry& entry,
           const container::String& key) {
            return entry.foldedName < key;
        });
    if (found == pose.bones.end() || found->foldedName != folded) {
        return std::nullopt;
    }
    data::w3d::FixedRigidTransform result = found->local;
    result.translation.x *= binding->second.assetScale;
    result.translation.y *= binding->second.assetScale;
    result.translation.z *= binding->second.assetScale;
    return result;
}

W3dWeaponBarrelTable W3dPristineBoneCatalog::resolveWeaponBarrels(
    container::StringView archetypeName, size_t visualRuleIndex,
    container::StringView fireFxPrefix,
    container::StringView recoilPrefix,
    container::StringView muzzleFlashPrefix,
    container::StringView launchPrefix) const {
    W3dWeaponBarrelTable result;
    const auto numberedName = [](container::StringView prefix,
                                 uint32_t ordinal) {
        if (prefix.empty()) return container::String{};
        container::String value(prefix);
        value.push_back(static_cast<char>('0' + ordinal / 10u));
        value.push_back(static_cast<char>('0' + ordinal % 10u));
        return value;
    };
    const auto pose = [this, archetypeName, visualRuleIndex](
                          container::StringView name) {
        return name.empty()
            ? std::optional<data::w3d::FixedRigidTransform>{}
            : find(archetypeName, visualRuleIndex, name);
    };

    container::String previousFireFxBone;
    for (uint32_t ordinal = 1; ordinal <= 99; ++ordinal) {
        const container::String fire = numberedName(fireFxPrefix, ordinal);
        const container::String recoil = numberedName(recoilPrefix, ordinal);
        const container::String muzzle = numberedName(muzzleFlashPrefix, ordinal);
        const container::String launch = numberedName(launchPrefix, ordinal);
        const auto firePose = pose(fire);
        const auto recoilPose = pose(recoil);
        const auto muzzlePose = pose(muzzle);
        const auto launchPose = pose(launch);
        if (!firePose && !recoilPose && !muzzlePose && !launchPose) break;

        W3dWeaponBarrelEntry entry;
        if (firePose) {
            entry.fireFxBone = fire;
            previousFireFxBone = fire;
        } else if (muzzlePose && !previousFireFxBone.empty()) {
            // W3DModelDraw::validateWeaponBarrelInfo carries the preceding FX
            // pivot across a muzzle-only numbered barrel.
            entry.fireFxBone = previousFireFxBone;
        }
        if (recoilPose) entry.recoilBone = recoil;
        if (muzzlePose) entry.muzzleFlash = muzzle;
        if (launchPose) {
            entry.launchBone = launch;
            entry.launchLocal = *launchPose;
            entry.hasLaunchBone = true;
        }
        result.barrels.push_back(std::move(entry));
    }
    if (!result.barrels.empty()) {
        result.numbered = true;
        return result;
    }

    const auto firePose = pose(fireFxPrefix);
    const auto recoilPose = pose(recoilPrefix);
    const auto muzzlePose = pose(muzzleFlashPrefix);
    const auto launchPose = pose(launchPrefix);
    if (!firePose && !recoilPose && !muzzlePose && !launchPose) return result;
    W3dWeaponBarrelEntry entry;
    if (firePose) entry.fireFxBone = container::String(fireFxPrefix);
    if (recoilPose) entry.recoilBone = container::String(recoilPrefix);
    if (muzzlePose) entry.muzzleFlash = container::String(muzzleFlashPrefix);
    if (launchPose) {
        entry.launchBone = container::String(launchPrefix);
        entry.launchLocal = *launchPose;
        entry.hasLaunchBone = true;
    }
    result.barrels.push_back(std::move(entry));
    return result;
}

} // namespace game

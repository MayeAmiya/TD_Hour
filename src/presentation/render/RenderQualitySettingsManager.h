#pragma once

#include "presentation/render/RenderGameDataSettings.h"

#include <atomic>

namespace config {
class GraphPreferences;
}

namespace engine {

// StaticGameLOD historically owns FPSLimit even though frame pacing belongs
// to display policy.  Keep that compatibility bridge separate from scene
// feature presets so new callers do not accidentally couple the authorities.
struct RenderDisplayPresetSet final {
    container::Array<bool, static_cast<size_t>(RenderStaticLod::Count)>
        fpsLimitEnabled{};
};

// Pure Options.ini projection.  No canonical RenderGameDataSettings value is
// mutated while parsing preferences, and legacy AntiAliasing remains separate
// from the final-image antiAliasingMode field used by FXAA.
struct RenderQualityPreferenceOverrides final {
    RenderFeatureQualityOverrides feature;
    RenderDisplayOverrides display;
};

[[nodiscard]] RenderDisplayPresetSet renderDisplayPresetSetFromGameData(
    const RenderGameDataSettings& settings) noexcept;
[[nodiscard]] RenderQualityPreferenceOverrides
renderQualityPreferenceOverrides(
    const config::GraphPreferences& preferences,
    RenderStaticLod inheritedStaticLod,
    container::Vector<container::String>* diagnostics = nullptr);

// One atomically replaceable value handle for both quality authorities.
// Mutations happen on the owning host/settings thread; consumers retain a
// SharedPtr<const ...> and therefore never observe a half-applied revision.
struct RenderQualitySettingsSnapshot final {
    ResolvedRenderFeatureSnapshot feature;
    ResolvedRenderDisplaySnapshot display;
    uint64_t revision = 0;
};

// Sparse external layers have stable ownership and priority. Host contains
// command-line/embedding policy established before a session; Runtime is the
// launcher/host API or automation layer and overrides only fields it
// explicitly sets. In-game UI is deliberately not a quality-settings owner.
enum class RenderQualityOverrideLayer : uint8_t {
    Host,
    Runtime,
};

class RenderQualitySettingsManager final {
public:
    RenderQualitySettingsManager();

    void configure(
        const RenderFeaturePresetSet& featurePresets,
        const RenderDisplayPresetSet& displayPresets,
        const RenderFeatureQualitySettings& featureBase,
        const RenderDisplaySettings& displayBase,
        const RenderFeatureQualityOverrides& featureOverrides = {},
        const RenderDisplayOverrides& displayOverrides = {});
    void setSources(
        const RenderFeaturePresetSet& featurePresets,
        const RenderDisplayPresetSet& displayPresets,
        const RenderFeatureQualitySettings& featureBase,
        const RenderDisplaySettings& displayBase);
    void setOverrides(
        const RenderFeatureQualityOverrides& featureOverrides,
        const RenderDisplayOverrides& displayOverrides);
    void setOverrides(
        RenderQualityOverrideLayer layer,
        const RenderFeatureQualityOverrides& featureOverrides,
        const RenderDisplayOverrides& displayOverrides);
    // Merge only explicitly present Display fields into an existing external
    // layer. Window/output events use this so they cannot erase unrelated
    // launcher/host overrides that share the Runtime layer.
    void patchDisplayOverrides(
        RenderQualityOverrideLayer layer,
        const RenderDisplayOverrides& displayOverrides);
    void clearOverrides(RenderQualityOverrideLayer layer);
    void setCapabilities(
        const RenderDisplayCapabilities& capabilities);

    [[nodiscard]] container::SharedPtr<const RenderQualitySettingsSnapshot>
    snapshot() const noexcept {
        return m_snapshot.load(std::memory_order_acquire);
    }
    [[nodiscard]] uint64_t revision() const noexcept {
        const auto current = snapshot();
        return current ? current->revision : 0;
    }

private:
    void publish();

    RenderFeaturePresetSet m_featurePresets;
    RenderDisplayPresetSet m_displayPresets;
    RenderFeatureQualitySettings m_featureBase;
    RenderDisplaySettings m_displayBase;
    RenderFeatureQualityOverrides m_preferenceFeatureOverrides;
    RenderDisplayOverrides m_preferenceDisplayOverrides;
    RenderFeatureQualityOverrides m_hostFeatureOverrides;
    RenderDisplayOverrides m_hostDisplayOverrides;
    RenderFeatureQualityOverrides m_runtimeFeatureOverrides;
    RenderDisplayOverrides m_runtimeDisplayOverrides;
    RenderDisplayCapabilities m_capabilities;
    std::atomic<container::SharedPtr<const RenderQualitySettingsSnapshot>>
        m_snapshot;
};

// Compatibility projection for legacy consumers. GameDataLoader supplies its
// persistent manager so this function publishes the same immutable snapshot
// that is projected into the old aggregate.
void applyRenderOptions(
    const config::GraphPreferences& preferences,
    RenderGameDataSettings& settings,
    RenderQualitySettingsManager& manager,
    container::Vector<container::String>* diagnostics = nullptr);

} // namespace engine

#pragma once

#include "container/hash_containers.h"
namespace config {

// Presentation and input preferences backed by the original flat Options.ini.
class GraphPreferences {
public:
    GraphPreferences() = default;
    ~GraphPreferences() = default;

    // Load from file
    bool load(const container::String& filePath);
    bool loadFromString(const container::String& content);

    // Save to file
    bool save(const container::String& filePath) const;
    container::String saveToString() const;

    // Clear all preferences
    void clear();

    // Type-safe getters
    bool getBool(const container::String& key, bool defaultValue = false) const;
    int getInt(const container::String& key, int defaultValue = 0) const;
    float getFloat(const container::String& key, float defaultValue = 0.0f) const;
    container::String getString(const container::String& key, const container::String& defaultValue = "") const;

    // Type-safe setters
    void setBool(const container::String& key, bool value);
    void setInt(const container::String& key, int value);
    void setFloat(const container::String& key, float value);
    void setString(const container::String& key, const container::String& value);

    // Check if key exists
    bool hasKey(const container::String& key) const;

    // Remove key
    void remove(const container::String& key);

    // Size
    size_t size() const { return m_preferences.size(); }
    bool empty() const { return m_preferences.empty(); }

    // ── Anti-aliasing ──────────────────────────────────────────────────
    int getAntiAliasing() const { return getInt("AntiAliasing", 0); }
    void setAntiAliasing(int value) { setInt("AntiAliasing", value); }

    // ── Texture filtering ──────────────────────────────────────────────
    int getTextureFilter() const { return getInt("TextureFilter", 2); }
    void setTextureFilter(int value) { setInt("TextureFilter", value); }

    int getAnisotropyLevel() const { return getInt("AnisotropyLevel", 2); }
    void setAnisotropyLevel(int value) { setInt("AnisotropyLevel", value); }

    // ── Gamma ──────────────────────────────────────────────────────────
    int getGamma() const { return getInt("Gamma", 50); }
    void setGamma(int value) { setInt("Gamma", value); }

    // ── Texture quality ────────────────────────────────────────────────
    int getTextureReduction() const { return getInt("TextureReduction", -1); }
    void setTextureReduction(int value) { setInt("TextureReduction", value); }

    // ── LOD ────────────────────────────────────────────────────────────
    container::String getStaticGameLOD() const { return getString("StaticGameLOD", "HIGH"); }
    void setStaticGameLOD(const container::String& value) { setString("StaticGameLOD", value); }

    bool getDynamicLOD() const { return getBool("DynamicLOD", true); }
    void setDynamicLOD(bool value) { setBool("DynamicLOD", value); }

    bool getFPSLimit() const { return getBool("FPSLimit", true); }
    void setFPSLimit(bool value) { setBool("FPSLimit", value); }

    // ── Shadows ────────────────────────────────────────────────────────
    bool getUseShadowVolumes() const { return getBool("UseShadowVolumes", false); }
    void setUseShadowVolumes(bool value) { setBool("UseShadowVolumes", value); }

    bool getUseShadowDecals() const { return getBool("UseShadowDecals", false); }
    void setUseShadowDecals(bool value) { setBool("UseShadowDecals", value); }

    // ── Terrain ────────────────────────────────────────────────────────
    bool getUseCloudMap() const { return getBool("UseCloudMap", true); }
    void setUseCloudMap(bool value) { setBool("UseCloudMap", value); }

    bool getUseLightMap() const { return getBool("UseLightMap", true); }
    void setUseLightMap(bool value) { setBool("UseLightMap", value); }

    bool getShowTrees() const { return getBool("ShowTrees", true); }
    void setShowTrees(bool value) { setBool("ShowTrees", value); }

    // ── Water ──────────────────────────────────────────────────────────
    bool getShowSoftWaterEdge() const { return getBool("ShowSoftWaterEdge", true); }
    void setShowSoftWaterEdge(bool value) { setBool("ShowSoftWaterEdge", value); }

    // ── Effects ────────────────────────────────────────────────────────
    bool getExtraAnimations() const { return getBool("ExtraAnimations", true); }
    void setExtraAnimations(bool value) { setBool("ExtraAnimations", value); }

    bool getHeatEffects() const { return getBool("HeatEffects", true); }
    void setHeatEffects(bool value) { setBool("HeatEffects", value); }

    // ── Occlusion ──────────────────────────────────────────────────────
    bool getBuildingOcclusion() const { return getBool("BuildingOcclusion", true); }
    void setBuildingOcclusion(bool value) { setBool("BuildingOcclusion", value); }

    // ── Particles ──────────────────────────────────────────────────────
    int getMaxParticleCount() const { return getInt("MaxParticleCount", 500); }
    void setMaxParticleCount(int value) { setInt("MaxParticleCount", value); }

private:
    container::HashMap<container::String, container::String> m_preferences;
    // Preserve the spelling of loaded/set keys for a readable flat
    // Options.ini while all lookups use a case-insensitive canonical key.
    container::HashMap<container::String, container::String> m_keySpelling;

    static container::String boolToString(bool value);
    static container::String intToString(int value);
    static container::String floatToString(float value);
    static bool parseBool(const container::String& value, bool defaultValue);
    static int parseInt(const container::String& value, int defaultValue);
    static float parseFloat(const container::String& value, float defaultValue);
};

} // namespace config

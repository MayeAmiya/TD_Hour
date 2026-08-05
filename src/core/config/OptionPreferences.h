#pragma once

#include "container/hash_containers.h"
namespace config {

// User preferences backed by INI file
class OptionPreferences {
public:
    OptionPreferences() = default;
    ~OptionPreferences() = default;

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
    double getDouble(const container::String& key, double defaultValue = 0.0) const;
    container::String getString(const container::String& key, const container::String& defaultValue = "") const;

    // Type-safe setters
    void setBool(const container::String& key, bool value);
    void setInt(const container::String& key, int value);
    void setFloat(const container::String& key, float value);
    void setDouble(const container::String& key, double value);
    void setString(const container::String& key, const container::String& value);

    // Check if key exists
    bool hasKey(const container::String& key) const;

    // Remove key
    void remove(const container::String& key);

    // Get all keys
    container::Vector<container::String> keys() const;

    // Size
    size_t size() const { return m_preferences.size(); }
    bool empty() const { return m_preferences.empty(); }

    // Access underlying map
    const container::HashMap<container::String, container::String>& data() const { return m_preferences; }

    // ── Audio ──────────────────────────────────────────────────────────
    float getSFXVolume() const { return getFloat("SFXVolume", 80.0f); }
    float getSFX3DVolume() const { return getFloat("SFX3DVolume", 80.0f); }
    float getVoiceVolume() const { return getFloat("VoiceVolume", 90.0f); }
    float getMusicVolume() const { return getFloat("MusicVolume", 70.0f); }
    float getMoneyTransactionVolume() const { return getFloat("MoneyTransactionVolume", 80.0f); }
    container::String get3DAudioProvider() const { return getString("3DAudioProvider", ""); }
    container::String getSpeakerType() const { return getString("SpeakerType", ""); }

    void setSFXVolume(float value) { setFloat("SFXVolume", value); }
    void setSFX3DVolume(float value) { setFloat("SFX3DVolume", value); }
    void setVoiceVolume(float value) { setFloat("VoiceVolume", value); }
    void setMusicVolume(float value) { setFloat("MusicVolume", value); }
    void setMoneyTransactionVolume(float value) { setFloat("MoneyTransactionVolume", value); }
    void set3DAudioProvider(const container::String& value) { setString("3DAudioProvider", value); }
    void setSpeakerType(const container::String& value) { setString("SpeakerType", value); }

    // ── Controls ───────────────────────────────────────────────────────
    bool getUseAlternateMouse() const { return getBool("UseAlternateMouse", false); }
    bool getUseRightMouseScrollWithAlternateMouse() const { return getBool("UseRightMouseScrollWithAlternateMouse", false); }
    bool getRetaliation() const { return getBool("Retaliation", true); }
    bool getUseDoubleClickAttackMove() const { return getBool("UseDoubleClickAttackMove", false); }
    int getScrollFactor() const { return getInt("ScrollFactor", 50); }
    bool getDrawScrollAnchor() const { return getBool("DrawScrollAnchor", false); }
    bool getMoveScrollAnchor() const { return getBool("MoveScrollAnchor", true); }

    void setUseAlternateMouse(bool value) { setBool("UseAlternateMouse", value); }
    void setUseRightMouseScrollWithAlternateMouse(bool value) { setBool("UseRightMouseScrollWithAlternateMouse", value); }
    void setRetaliation(bool value) { setBool("Retaliation", value); }
    void setUseDoubleClickAttackMove(bool value) { setBool("UseDoubleClickAttackMove", value); }
    void setScrollFactor(int value) { setInt("ScrollFactor", value); }
    void setDrawScrollAnchor(bool value) { setBool("DrawScrollAnchor", value); }
    void setMoveScrollAnchor(bool value) { setBool("MoveScrollAnchor", value); }

    // ── Game ───────────────────────────────────────────────────────────
    int getCampaignDifficulty() const { return getInt("CampaignDifficulty", 1); }
    bool getArchiveReplays() const { return getBool("ArchiveReplays", false); }
    bool getSaveCameraInReplays() const { return getBool("SaveCameraInReplays", true); }
    bool getUseCameraInReplays() const { return getBool("UseCameraInReplays", true); }
    bool getPlayerObserverEnabled() const { return getBool("PlayerObserverEnabled", true); }
    bool getUseSystemMapDir() const { return getBool("UseSystemMapDir", true); }
    bool getShowMoneyPerMinute() const { return getBool("ShowMoneyPerMinute", false); }

    void setCampaignDifficulty(int value) { setInt("CampaignDifficulty", value); }
    void setArchiveReplays(bool value) { setBool("ArchiveReplays", value); }
    void setSaveCameraInReplays(bool value) { setBool("SaveCameraInReplays", value); }
    void setUseCameraInReplays(bool value) { setBool("UseCameraInReplays", value); }
    void setPlayerObserverEnabled(bool value) { setBool("PlayerObserverEnabled", value); }
    void setUseSystemMapDir(bool value) { setBool("UseSystemMapDir", value); }
    void setShowMoneyPerMinute(bool value) { setBool("ShowMoneyPerMinute", value); }

    // ── HUD ────────────────────────────────────────────────────────────
    int getNetworkLatencyFontSize() const { return getInt("NetworkLatencyFontSize", 8); }
    int getRenderFpsFontSize() const { return getInt("RenderFpsFontSize", 8); }
    int getSystemTimeFontSize() const { return getInt("SystemTimeFontSize", 8); }
    int getGameTimeFontSize() const { return getInt("GameTimeFontSize", 8); }
    int getPlayerInfoListFontSize() const { return getInt("PlayerInfoListFontSize", 8); }
    float getResolutionFontAdjustment() const { return getFloat("ResolutionFontAdjustment", -1.0f); }

    void setNetworkLatencyFontSize(int value) { setInt("NetworkLatencyFontSize", value); }
    void setRenderFpsFontSize(int value) { setInt("RenderFpsFontSize", value); }
    void setSystemTimeFontSize(int value) { setInt("SystemTimeFontSize", value); }
    void setGameTimeFontSize(int value) { setInt("GameTimeFontSize", value); }
    void setPlayerInfoListFontSize(int value) { setInt("PlayerInfoListFontSize", value); }
    void setResolutionFontAdjustment(float value) { setFloat("ResolutionFontAdjustment", value); }

private:
    container::HashMap<container::String, container::String> m_preferences;

    // Helper to convert value to string
    static container::String boolToString(bool value);
    static container::String intToString(int value);
    static container::String floatToString(float value);
    static container::String doubleToString(double value);

    // Helper to parse string to value
    static bool parseBool(const container::String& value, bool defaultValue);
    static int parseInt(const container::String& value, int defaultValue);
    static float parseFloat(const container::String& value, float defaultValue);
    static double parseDouble(const container::String& value, double defaultValue);
};

} // namespace config

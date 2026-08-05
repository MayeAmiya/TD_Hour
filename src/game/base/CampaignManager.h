#pragma once

#include "core/container/container_types.h"

#include "game/base/GameSettings.h"
namespace game {

struct Mission {
    container::String name;
    container::String mapName;
    container::String nextMission;
    container::String movieLabel;
    container::String objectives[5];
    container::String generalName;
    container::String locationName;
    container::String unitNames[3];
    container::String briefingVoice;
    int voiceLength = 0;
};

struct Campaign {
    container::String name;
    container::String firstMission;
    container::String campaignNameLabel;
    container::String finalMovieName;
    bool isChallengeCampaign = false;
    container::String playerFactionName;
    container::Vector<Mission> missions;

    const Mission* getNextMission(const Mission* current) const;
};

class CampaignManager {
public:
    static CampaignManager& instance();

    bool loadFromIni(const container::String& filename);

    void setCampaign(const container::String& name);
    Campaign* getCurrentCampaign() const { return m_currentCampaign; }

    void gotoNextMission();
    const Mission* getCurrentMission() const { return m_currentMission; }
    container::String getCurrentMap() const;

    void setGameDifficulty(int difficulty) { m_difficulty = difficulty; }
    int getGameDifficulty() const { return m_difficulty; }

    void setVictorious(bool victorious) { m_victorious = victorious; }
    bool isVictorious() const { return m_victorious; }

    int getRankPoints() const { return 0; }

    Campaign* findCampaign(const container::String& name);
    [[nodiscard]] const Mission* findMissionByMap(
        container::StringView mapName,
        const Campaign** campaign = nullptr) const;
    [[nodiscard]] const Campaign* findCampaignByMap(
        container::StringView mapName) const;
    const container::Vector<Campaign>& getCampaigns() const { return m_campaigns; }

private:
    CampaignManager() = default;

    container::Vector<Campaign> m_campaigns;
    Campaign* m_currentCampaign = nullptr;
    const Mission* m_currentMission = nullptr;
    int m_difficulty = engine::DIFFICULTY_NORMAL;
    bool m_victorious = false;
};

extern CampaignManager* TheCampaignManager;

} // namespace game

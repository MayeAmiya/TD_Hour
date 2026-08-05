#pragma once

#include "core/container/container_types.h"

#include "game/base/GameSettings.h"
namespace game {

constexpr int NUM_GENERALS = 12;

struct GeneralPersona {
    container::String bioName;
    container::String bioDOB;
    container::String bioBirthplace;
    container::String bioStrategy;
    container::String bioRank;
    container::String bioBranch;
    container::String bioClassNumber;

    container::String imageBioPortraitSmall;
    container::String imageBioPortraitLarge;
    container::String imageDefeated;
    container::String imageVictorious;

    container::String campaign;
    container::String playerTemplateName;

    container::String portraitMovieLeftName;
    container::String portraitMovieRightName;

    container::String selectionSound;
    container::String tauntSound1;
    container::String tauntSound2;
    container::String tauntSound3;
    container::String winSound;
    container::String lossSound;
    container::String previewSound;
    container::String nameSound;

    container::String defeatedText;
    container::String victoriousText;

    bool startsEnabled = false;
};

class ChallengeGenerals {
public:
    static ChallengeGenerals& instance();

    bool loadFromIni(const container::String& filename);

    GeneralPersona& getGeneral(int index);
    const GeneralPersona& getGeneral(int index) const;

    int getPlayerGeneralByCampaignName(const container::String& name) const;
    int getGeneralByGeneralName(const container::String& name) const;
    int getGeneralByTemplateName(const container::String& name) const;

    void setCurrentPlayerTemplateNum(int num) { m_playerTemplateNum = num; }
    int getCurrentPlayerTemplateNum() const { return m_playerTemplateNum; }

    void setCurrentDifficulty(int difficulty) { m_currentDifficulty = difficulty; }
    int getCurrentDifficulty() const { return m_currentDifficulty; }

    int getEnabledCount() const;

private:
    ChallengeGenerals() = default;

    container::Array<GeneralPersona, NUM_GENERALS> m_generals;
    int m_playerTemplateNum = -1;
    int m_currentDifficulty = engine::DIFFICULTY_NORMAL;
};

extern ChallengeGenerals* TheChallengeGenerals;

} // namespace game

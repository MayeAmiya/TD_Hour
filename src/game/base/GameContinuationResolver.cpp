#include "game/base/GameContinuationResolver.h"

#include "game/base/CampaignManager.h"
#include "core/container/string_utils.h"

#include <limits>

namespace engine {
namespace {

GameContinuationResult failure(GameContinuationStatus status, container::String error)
{
    GameContinuationResult result;
    result.status = status;
    result.error = std::move(error);
    return result;
}

const game::Mission* findMission(const game::Campaign& campaign,
                                 container::StringView missionName)
{
    for (const game::Mission& mission : campaign.missions) {
        if (container::asciiEqualIgnoreCase(mission.name, missionName)) {
            return &mission;
        }
    }
    return nullptr;
}

} // namespace

GameContinuationResult GameContinuationResolver::resolve(
    const GameStartInfo& current,
    GameContinuationAction action)
{
    if (action == GameContinuationAction::Retry) {
        if (current.mapName.empty()) {
            return failure(GameContinuationStatus::MissingMission,
                           "retry requires the current map identity");
        }
        GameContinuationResult result;
        result.status = GameContinuationStatus::Success;
        result.startInfo = current;
        result.startInfo.saveFileName.clear();
        result.startInfo.replayFileName.clear();
        result.startInfo.network.reset();
        return result;
    }

    const GameSequenceIdentity& sequence = current.sequence;
    switch (sequence.type) {
        case GameSequenceType::Campaign:
            if (sequence.campaignName.empty() || sequence.missionName.empty()) {
                return failure(GameContinuationStatus::InvalidSequence,
                               "campaign sequence requires campaignName and missionName");
            }
            break;
        case GameSequenceType::Challenge:
            if (sequence.campaignName.empty() || sequence.missionName.empty() ||
                sequence.challengeGeneral.empty()) {
                return failure(GameContinuationStatus::InvalidSequence,
                               "challenge sequence requires campaignName, missionName, and challengeGeneral");
            }
            break;
        case GameSequenceType::None:
        default:
            return failure(GameContinuationStatus::InvalidSequence,
                           "game has no continuable sequence identity");
    }

    game::Campaign* campaign =
        game::CampaignManager::instance().findCampaign(sequence.campaignName);
    if (!campaign) {
        return failure(GameContinuationStatus::MissingCampaign,
                       "campaign not found: " + sequence.campaignName);
    }

    const game::Mission* currentMission = findMission(*campaign, sequence.missionName);
    if (!currentMission || currentMission->mapName.empty()) {
        return failure(GameContinuationStatus::MissingMission,
                       "mission not found or has no map: " + sequence.missionName);
    }

    const game::Mission* targetMission = currentMission;
    switch (action) {
        case GameContinuationAction::Next:
            if (currentMission->nextMission.empty()) {
                return failure(GameContinuationStatus::NoNextMission,
                               "mission has no next mission: " + currentMission->name);
            }
            targetMission = findMission(*campaign, currentMission->nextMission);
            if (!targetMission || targetMission->mapName.empty()) {
                return failure(GameContinuationStatus::MissingMission,
                               "next mission not found or has no map: " +
                                   currentMission->nextMission);
            }
            break;
        default:
            return failure(GameContinuationStatus::InvalidSequence,
                           "invalid continuation action");
    }

    GameContinuationResult result;
    result.status = GameContinuationStatus::Success;
    result.startInfo = current;
    result.startInfo.mapName = targetMission->mapName;
    result.startInfo.sequence.campaignName = campaign->name;
    result.startInfo.sequence.missionName = targetMission->name;
    result.startInfo.mapCRC = 0;
    result.startInfo.mapSize = 0;
    result.startInfo.saveFileName.clear();
    result.startInfo.replayFileName.clear();
    result.startInfo.network.reset();
    result.startInfo.seed = current.seed == std::numeric_limits<int>::max()
        ? std::numeric_limits<int>::min()
        : current.seed + 1;
    return result;
}

} // namespace engine

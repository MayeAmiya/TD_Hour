#include "core/container/container_types.h"
#include "ModelConditionState.h"
#include <bit>
#include <cctype>
namespace game {
namespace {

static_assert(kModelConditionFlagCount == 118,
    "ModelConditionFlag order must match RefCode condition name table");

constexpr container::Array<container::StringView, kModelConditionFlagCount>
    kRefCodeConditionNames = {{
    "TOPPLED", "FRONTCRUSHED", "BACKCRUSHED", "DAMAGED", "REALLY_DAMAGED", "RUBBLE",
    "SPECIAL_DAMAGED", "NIGHT", "SNOW", "PARACHUTING", "GARRISONED", "ENEMYNEAR",
    "WEAPONSET_VETERAN", "WEAPONSET_ELITE", "WEAPONSET_HERO", "WEAPONSET_CRATEUPGRADE_ONE",
    "WEAPONSET_CRATEUPGRADE_TWO", "WEAPONSET_PLAYER_UPGRADE", "DOOR_1_OPENING", "DOOR_1_CLOSING",
    "DOOR_1_WAITING_OPEN", "DOOR_1_WAITING_TO_CLOSE", "DOOR_2_OPENING", "DOOR_2_CLOSING",
    "DOOR_2_WAITING_OPEN", "DOOR_2_WAITING_TO_CLOSE", "DOOR_3_OPENING", "DOOR_3_CLOSING",
    "DOOR_3_WAITING_OPEN", "DOOR_3_WAITING_TO_CLOSE", "DOOR_4_OPENING", "DOOR_4_CLOSING",
    "DOOR_4_WAITING_OPEN", "DOOR_4_WAITING_TO_CLOSE", "ATTACKING", "PREATTACK_A", "FIRING_A",
    "BETWEEN_FIRING_SHOTS_A", "RELOADING_A", "PREATTACK_B", "FIRING_B", "BETWEEN_FIRING_SHOTS_B",
    "RELOADING_B", "PREATTACK_C", "FIRING_C", "BETWEEN_FIRING_SHOTS_C", "RELOADING_C",
    "TURRET_ROTATE", "POST_COLLAPSE", "MOVING", "DYING", "AWAITING_CONSTRUCTION",
    "PARTIALLY_CONSTRUCTED", "ACTIVELY_BEING_CONSTRUCTED", "PRONE", "FREEFALL", "ACTIVELY_CONSTRUCTING",
    "CONSTRUCTION_COMPLETE", "RADAR_EXTENDING", "RADAR_UPGRADED", "PANICKING", "AFLAME", "SMOLDERING",
    "BURNED", "DOCKING", "DOCKING_BEGINNING", "DOCKING_ACTIVE", "DOCKING_ENDING", "CARRYING", "FLOODED",
    "LOADED", "JETAFTERBURNER", "JETEXHAUST", "PACKING", "UNPACKING", "DEPLOYED", "OVER_WATER",
    "POWER_PLANT_UPGRADED", "CLIMBING", "SOLD", "SURRENDER", "RAPPELLING", "ARMED",
    "POWER_PLANT_UPGRADING", "SPECIAL_CHEERING", "CONTINUOUS_FIRE_SLOW", "CONTINUOUS_FIRE_MEAN",
    "CONTINUOUS_FIRE_FAST", "RAISING_FLAG", "CAPTURED", "EXPLODED_FLAILING", "EXPLODED_BOUNCING",
    "SPLATTED", "USING_WEAPON_A", "USING_WEAPON_B", "USING_WEAPON_C", "PREORDER", "CENTER_TO_LEFT",
    "LEFT_TO_CENTER", "CENTER_TO_RIGHT", "RIGHT_TO_CENTER", "RIDER1", "RIDER2", "RIDER3", "RIDER4",
    "RIDER5", "RIDER6", "RIDER7", "RIDER8", "STUNNED_FLAILING", "STUNNED", "SECOND_LIFE", "JAMMED",
    "ARMORSET_CRATEUPGRADE_ONE", "ARMORSET_CRATEUPGRADE_TWO", "USER_1", "USER_2", "DISGUISED",
}};

uint32_t popcount(uint64_t value) noexcept {
    return static_cast<uint32_t>(std::popcount(value));
}

[[nodiscard]] bool equalsNormalizedConditionName(container::StringView value, container::StringView candidate) noexcept
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    {
        value.remove_suffix(1);
    }
    size_t left = 0;
    size_t right = 0;
    while (left < value.size() || right < candidate.size())
    {
        while (left < value.size() && value[left] == '_')
            ++left;
        while (right < candidate.size() && candidate[right] == '_')
            ++right;
        if (left == value.size() || right == candidate.size())
        {
            return left == value.size() && right == candidate.size();
        }
        const char a = static_cast<char>(std::toupper(static_cast<unsigned char>(value[left++])));
        const char b = static_cast<char>(std::toupper(static_cast<unsigned char>(candidate[right++])));
        if (a != b)
            return false;
    }
    return true;
}

} // namespace

std::optional<uint32_t> tryParseModelConditionFlag(container::StringView name) noexcept
{
    for (uint32_t index = 0; index < kRefCodeConditionNames.size(); ++index)
    {
        if (equalsNormalizedConditionName(name, kRefCodeConditionNames[index]))
        {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<ModelConditionFlag> tryParseModelConditionFlagEnum(
    container::StringView name) noexcept
{
    if (const std::optional<uint32_t> index = tryParseModelConditionFlag(name))
    {
        if (*index < kModelConditionFlagCount)
            return static_cast<ModelConditionFlag>(*index);
    }
    return std::nullopt;
}

container::StringView modelConditionFlagName(ModelConditionFlag flag) noexcept
{
    const uint32_t index = modelConditionFlagIndex(flag);
    if (index >= kRefCodeConditionNames.size())
        return {};
    return kRefCodeConditionNames[index];
}

bool ModelConditionMask::empty() const noexcept {
    return words[0] == 0 && words[1] == 0;
}

uint32_t ModelConditionMask::intersectionCount(const ModelConditionMask& other) const noexcept {
    return popcount(words[0] & other.words[0]) + popcount(words[1] & other.words[1]);
}

uint32_t ModelConditionMask::extraneousCountAgainst(const ModelConditionMask& active) const noexcept {
    return popcount(words[0] & ~active.words[0]) + popcount(words[1] & ~active.words[1]);
}

void ModelConditionMask::clear(const ModelConditionMask& mask) noexcept {
    words[0] &= ~mask.words[0];
    words[1] &= ~mask.words[1];
}

void ModelConditionMask::set(uint32_t index, bool value) noexcept {
    if (index >= kModelConditionFlagCount) return;
    const uint32_t word = index / 64;
    const uint64_t bit = uint64_t{1} << (index % 64);
    if (value) words[word] |= bit;
    else words[word] &= ~bit;
}

ModelConditionMask parseModelConditionMask(container::StringView names) {
    ModelConditionMask result;
    container::String token;
    const auto addToken = [&result](container::StringView value) {
                if (const std::optional<uint32_t> index = tryParseModelConditionFlag(value))
        {
            result.set(* index);
        }
    };
    for (const char character : names) {
        if (std::isspace(static_cast<unsigned char>(character))) {
            if (!token.empty()) {
                addToken(token);
                token.clear();
            }
        } else if (character != '_') {
            token += static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        }
    }
    if (!token.empty()) addToken(token);
    return result;
}

} // namespace game

#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "CombatProfile.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace game
{
namespace
{

using container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView trimAsciiWhitespace(container::StringView text) noexcept
{
    while (!text.empty() &&
           (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
    {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] container::Vector<container::StringView> splitWhitespace(container::StringView text)
{
    container::Vector<container::StringView> tokens;
    text = trimAsciiWhitespace(text);
    while (!text.empty())
    {
        std::size_t end = 0;
        while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '\r' && text[end] != '\n')
        {
            ++end;
        }
        tokens.push_back(text.substr(0, end));
        text.remove_prefix(end);
        text = trimAsciiWhitespace(text);
    }
    return tokens;
}

[[nodiscard]] std::pair<container::StringView, container::StringView> splitFirstToken(container::StringView text) noexcept
{
    text = trimAsciiWhitespace(text);
    std::size_t end = 0;
    while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '\r' && text[end] != '\n')
    {
        ++end;
    }
    return {text.substr(0, end), trimAsciiWhitespace(text.substr(end))};
}

[[nodiscard]] container::String stripOneQuotePair(container::StringView text)
{
    text = trimAsciiWhitespace(text);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
    {
        text.remove_prefix(1);
        text.remove_suffix(1);
    }
    return container::String(text);
}

[[nodiscard]] CombatProfileParseStatus parseFailure(container::String message)
{
    return {
        .success = false,
        .message = std::move(message),
    };
}

template <typename Mask, typename ParseNamedFlag>
[[nodiscard]] CombatProfileParseStatus applyNamedBitString(container::StringView text,
                                                           Mask& inOutMask,
                                                           Mask allowedMask,
                                                           bool preserveUnknownHighBits,
                                                           container::StringView fieldName,
                                                           ParseNamedFlag&& parseNamedFlag)
{
    Mask working = inOutMask;
    bool foundNormal = false;
    bool foundAddOrSubtract = false;

    for (container::StringView token : splitWhitespace(text))
    {
        if (asciiEqualIgnoreCase(token, "NONE"))
        {
            if (foundNormal || foundAddOrSubtract)
            {
                return parseFailure(container::String(fieldName) + " cannot mix NONE with normal or +/- tokens");
            }
            working = 0;
            break; // RefCode's BitFlags parser ignores later tokens after NONE.
        }

        bool add = false;
        bool subtract = false;
        if (!token.empty() && token.front() == '+')
        {
            add = true;
            token.remove_prefix(1);
        }
        else if (!token.empty() && token.front() == '-')
        {
            subtract = true;
            token.remove_prefix(1);
        }

        if (add || subtract)
        {
            if (foundNormal)
            {
                return parseFailure(container::String(fieldName) + " cannot mix normal and +/- tokens");
            }
            foundAddOrSubtract = true;
        }
        else
        {
            if (foundAddOrSubtract)
            {
                return parseFailure(container::String(fieldName) + " cannot mix normal and +/- tokens");
            }
            if (!foundNormal)
                working = 0;
            foundNormal = true;
        }

        const auto parsed = parseNamedFlag(token);
        if (!parsed)
        {
            return parseFailure(container::String(fieldName) + " has unknown token '" + container::String(token) + "'");
        }
        const Mask bit = Mask{1} << static_cast<uint8_t>(*parsed);
        if (add || !subtract)
        {
            working |= bit;
        }
        else
        {
            working &= ~bit;
        }
    }

    inOutMask = preserveUnknownHighBits ? working : (working & allowedMask);
    return {};
}

[[nodiscard]] bool parseBoolean(container::StringView text, bool& outValue) noexcept
{
    const auto [token, trailing] = splitFirstToken(text);
    static_cast<void>(trailing);
    if (asciiEqualIgnoreCase(token, "YES") || asciiEqualIgnoreCase(token, "TRUE") ||
        asciiEqualIgnoreCase(token, "ON") || token == "1")
    {
        outValue = true;
        return true;
    }
    if (asciiEqualIgnoreCase(token, "NO") || asciiEqualIgnoreCase(token, "FALSE") ||
        asciiEqualIgnoreCase(token, "OFF") || token == "0")
    {
        outValue = false;
        return true;
    }
    return false;
}

[[nodiscard]] CombatProfileParseStatus applyPreferredAgainstKinds(container::StringView text,
                                                                  ObjectKindOfMask& inOutKinds)
{
    ObjectKindOfMask working = inOutKinds;
    bool foundNormal = false;
    bool foundAddOrSubtract = false;

    for (container::StringView token : splitWhitespace(text))
    {
        if (asciiEqualIgnoreCase(token, "NONE"))
        {
            if (foundNormal || foundAddOrSubtract)
            {
                return parseFailure("PreferredAgainst cannot mix NONE with normal or +/- tokens");
            }
            working.clear();
            break;
        }

        bool add = false;
        bool subtract = false;
        if (!token.empty() && token.front() == '+')
        {
            add = true;
            token.remove_prefix(1);
        }
        else if (!token.empty() && token.front() == '-')
        {
            subtract = true;
            token.remove_prefix(1);
        }
        if (token.empty())
        {
            return parseFailure("PreferredAgainst has an empty +/- token");
        }

        if (add || subtract)
        {
            if (foundNormal)
            {
                return parseFailure("PreferredAgainst cannot mix normal and +/- tokens");
            }
            foundAddOrSubtract = true;
        }
        else
        {
            if (foundAddOrSubtract)
            {
                return parseFailure("PreferredAgainst cannot mix normal and +/- tokens");
            }
            if (!foundNormal)
                working.clear();
            foundNormal = true;
        }

        const std::optional<ObjectKindOf> kind = parseObjectKindOf(token);
        if (!kind)
            return parseFailure("PreferredAgainst has an unknown KindOf '" +
                                container::String{token} + "'");
        if (subtract)
        {
            working.reset(objectKindOfIndex(*kind));
        }
        else
        {
            setObjectKind(working, *kind);
        }
    }

    inOutKinds = std::move(working);
    return {};
}

void addDiagnostic(CombatProfileCompileResult& result, CombatProfileDiagnosticSeverity severity, container::String message)
{
    if (severity == CombatProfileDiagnosticSeverity::Error)
        result.hasErrors = true;
    result.diagnostics.push_back({
        .severity = severity,
        .message = std::move(message),
    });
}

[[nodiscard]] bool parseWeaponBinding(container::StringView value,
                                      WeaponSetProfile& profile,
                                      CombatProfileCompileResult& result)
{
    const auto [slotToken, templateTail] = splitFirstToken(value);
    const auto slot = tryParseWeaponSlot(slotToken);
    if (!slot)
    {
        addDiagnostic(result,
                      CombatProfileDiagnosticSeverity::Error,
                      "WeaponSet Weapon has an unknown slot '" + container::String(slotToken) + "'");
        return false;
    }
    const auto [templateToken, trailing] = splitFirstToken(templateTail);
    if (templateToken.empty())
    {
        addDiagnostic(result,
                      CombatProfileDiagnosticSeverity::Error,
                      "WeaponSet Weapon for slot '" + container::String(slotToken) + "' has no template name");
        return false;
    }
    if (!trailing.empty())
    {
        addDiagnostic(result,
                      CombatProfileDiagnosticSeverity::Warning,
                      "WeaponSet Weapon for slot '" + container::String(slotToken) +
                          "' ignores trailing tokens after its template name");
    }

    WeaponSlotProfile& destination = profile.slots[static_cast<std::size_t>(*slot)];
    destination.weaponTemplateName =
        asciiEqualIgnoreCase(templateToken, "NONE") ? container::String{} : stripOneQuotePair(templateToken);
    return true;
}

[[nodiscard]] bool parseAutoChooseSources(container::StringView value,
                                          WeaponSetProfile& profile,
                                          CombatProfileCompileResult& result)
{
    const auto [slotToken, maskText] = splitFirstToken(value);
    const auto slot = tryParseWeaponSlot(slotToken);
    if (!slot)
    {
        addDiagnostic(result,
                      CombatProfileDiagnosticSeverity::Error,
                      "WeaponSet AutoChooseSources has an unknown slot '" + container::String(slotToken) + "'");
        return false;
    }
    WeaponAutoChooseSourceMask& destination = profile.slots[static_cast<std::size_t>(*slot)].autoChooseSources;
    const CombatProfileParseStatus status = applyWeaponAutoChooseSources(maskText, destination);
    if (!status.success)
    {
        addDiagnostic(result, CombatProfileDiagnosticSeverity::Error, "WeaponSet AutoChooseSources: " + status.message);
        return false;
    }
    return true;
}

[[nodiscard]] bool parsePreferredAgainst(container::StringView value,
                                         WeaponSetProfile& profile,
                                         CombatProfileCompileResult& result)
{
    const auto [slotToken, kindsText] = splitFirstToken(value);
    const auto slot = tryParseWeaponSlot(slotToken);
    if (!slot)
    {
        addDiagnostic(result,
                      CombatProfileDiagnosticSeverity::Error,
                      "WeaponSet PreferredAgainst has an unknown slot '" + container::String(slotToken) + "'");
        return false;
    }
    ObjectKindOfMask& destination =
        profile.slots[static_cast<std::size_t>(*slot)].preferredAgainstKinds;
    const CombatProfileParseStatus status = applyPreferredAgainstKinds(kindsText, destination);
    if (!status.success)
    {
        addDiagnostic(result, CombatProfileDiagnosticSeverity::Error, "WeaponSet PreferredAgainst: " + status.message);
        return false;
    }
    return true;
}

[[nodiscard]] bool parseWeaponSetBlock(const IniBlock& block,
                                       WeaponSetProfile& profile,
                                       CombatProfileCompileResult& result)
{
    const std::size_t diagnosticsBefore = result.diagnostics.size();
    for (const auto& [key, value] : block.values)
    {
        if (asciiEqualIgnoreCase(key, "Conditions"))
        {
            const CombatProfileParseStatus status = applyWeaponSetConditions(value, profile.conditions);
            if (!status.success)
            {
                addDiagnostic(
                    result, CombatProfileDiagnosticSeverity::Error, "WeaponSet Conditions: " + status.message);
            }
        }
        else if (asciiEqualIgnoreCase(key, "Weapon"))
        {
            static_cast<void>(parseWeaponBinding(value, profile, result));
        }
        else if (asciiEqualIgnoreCase(key, "AutoChooseSources"))
        {
            static_cast<void>(parseAutoChooseSources(value, profile, result));
        }
        else if (asciiEqualIgnoreCase(key, "PreferredAgainst"))
        {
            static_cast<void>(parsePreferredAgainst(value, profile, result));
        }
        else if (asciiEqualIgnoreCase(key, "ShareWeaponReloadTime"))
        {
            if (!parseBoolean(value, profile.shareWeaponReloadTime))
            {
                addDiagnostic(result,
                              CombatProfileDiagnosticSeverity::Error,
                              "WeaponSet ShareWeaponReloadTime must be Yes or No");
            }
        }
        else if (asciiEqualIgnoreCase(key, "WeaponLockSharedAcrossSets"))
        {
            if (!parseBoolean(value, profile.weaponLockSharedAcrossSets))
            {
                addDiagnostic(result,
                              CombatProfileDiagnosticSeverity::Error,
                              "WeaponSet WeaponLockSharedAcrossSets must be Yes or No");
            }
        }
        else
        {
            addDiagnostic(
                result, CombatProfileDiagnosticSeverity::Warning, "WeaponSet ignores unsupported field '" + key + "'");
        }
    }
    for (const IniBlock& child : block.children)
    {
        addDiagnostic(
            result, CombatProfileDiagnosticSeverity::Warning, "WeaponSet ignores nested block '" + child.type + "'");
    }
    return std::none_of(result.diagnostics.begin() + static_cast<std::ptrdiff_t>(diagnosticsBefore),
                        result.diagnostics.end(),
                        [](const CombatProfileDiagnostic& diagnostic)
                        { return diagnostic.severity == CombatProfileDiagnosticSeverity::Error; });
}

[[nodiscard]] bool parseArmorSetBlock(const IniBlock& block,
                                      ArmorSetProfile& profile,
                                      CombatProfileCompileResult& result)
{
    const std::size_t diagnosticsBefore = result.diagnostics.size();
    for (const auto& [key, value] : block.values)
    {
        if (asciiEqualIgnoreCase(key, "Conditions"))
        {
            const CombatProfileParseStatus status = applyArmorSetConditions(value, profile.conditions);
            if (!status.success)
            {
                addDiagnostic(result, CombatProfileDiagnosticSeverity::Error, "ArmorSet Conditions: " + status.message);
            }
        }
        else if (asciiEqualIgnoreCase(key, "Armor"))
        {
            const auto [name, trailing] = splitFirstToken(value);
            if (name.empty())
            {
                addDiagnostic(result, CombatProfileDiagnosticSeverity::Error, "ArmorSet Armor has no template name");
            }
            else
            {
                if (!trailing.empty())
                {
                    addDiagnostic(result,
                                  CombatProfileDiagnosticSeverity::Warning,
                                  "ArmorSet Armor ignores trailing tokens after its template name");
                }
                profile.armorTemplateName =
                    asciiEqualIgnoreCase(name, "NONE") ? container::String{} : stripOneQuotePair(name);
            }
        }
        else if (asciiEqualIgnoreCase(key, "DamageFX"))
        {
            const auto [name, trailing] = splitFirstToken(value);
            if (name.empty())
            {
                addDiagnostic(result, CombatProfileDiagnosticSeverity::Error, "ArmorSet DamageFX has no template name");
            }
            else
            {
                if (!trailing.empty())
                {
                    addDiagnostic(result,
                                  CombatProfileDiagnosticSeverity::Warning,
                                  "ArmorSet DamageFX ignores trailing tokens after its template name");
                }
                profile.damageFxName = asciiEqualIgnoreCase(name, "NONE") ? container::String{} : stripOneQuotePair(name);
            }
        }
        else
        {
            addDiagnostic(
                result, CombatProfileDiagnosticSeverity::Warning, "ArmorSet ignores unsupported field '" + key + "'");
        }
    }
    for (const IniBlock& child : block.children)
    {
        addDiagnostic(
            result, CombatProfileDiagnosticSeverity::Warning, "ArmorSet ignores nested block '" + child.type + "'");
    }
    return std::none_of(result.diagnostics.begin() + static_cast<std::ptrdiff_t>(diagnosticsBefore),
                        result.diagnostics.end(),
                        [](const CombatProfileDiagnostic& diagnostic)
                        { return diagnostic.severity == CombatProfileDiagnosticSeverity::Error; });
}

template <typename Set, typename Mask>
[[nodiscard]] const Set* findBestConditionSet(container::Span<const Set> sets, Mask activeConditions, Mask validMask) noexcept
{
    const Set* best = nullptr;
    int bestMatched = -1;
    int bestMissing = std::numeric_limits<int>::max();
    activeConditions &= validMask;

    for (const Set& candidate : sets)
    {
        const Mask conditions = candidate.conditions & validMask;
        const int matched = std::popcount(activeConditions & conditions);
        const int missing = std::popcount((~activeConditions) & conditions);
        if (matched > bestMatched || (matched == bestMatched && missing < bestMissing))
        {
            best = &candidate;
            bestMatched = matched;
            bestMissing = missing;
        }
    }
    return best;
}

template <typename Set>
void validateDuplicateConditions(container::Span<const Set> sets, container::StringView label, CombatProfileCompileResult& result)
{
    for (std::size_t left = 0; left < sets.size(); ++left)
    {
        for (std::size_t right = left + 1; right < sets.size(); ++right)
        {
            if (sets[left].conditions == sets[right].conditions)
            {
                addDiagnostic(result,
                              CombatProfileDiagnosticSeverity::Error,
                              container::String(label) + " has duplicate Conditions at authored indices " +
                                  std::to_string(left) + " and " + std::to_string(right));
            }
        }
    }
}

} // namespace

std::optional<WeaponSetCondition> tryParseWeaponSetCondition(container::StringView token) noexcept
{
    constexpr container::Array<container::StringView, static_cast<std::size_t>(WeaponSetCondition::Count)> names = {
        "VETERAN",
        "ELITE",
        "HERO",
        "PLAYER_UPGRADE",
        "CRATEUPGRADE_ONE",
        "CRATEUPGRADE_TWO",
        "VEHICLE_HIJACK",
        "CARBOMB",
        "MINE_CLEARING_DETAIL",
        "WEAPON_RIDER1",
        "WEAPON_RIDER2",
        "WEAPON_RIDER3",
        "WEAPON_RIDER4",
        "WEAPON_RIDER5",
        "WEAPON_RIDER6",
        "WEAPON_RIDER7",
        "WEAPON_RIDER8",
    };
    for (std::size_t index = 0; index < names.size(); ++index)
    {
        if (asciiEqualIgnoreCase(token, names[index]))
        {
            return static_cast<WeaponSetCondition>(index);
        }
    }
    return std::nullopt;
}

std::optional<ArmorSetCondition> tryParseArmorSetCondition(container::StringView token) noexcept
{
    constexpr container::Array<container::StringView, static_cast<std::size_t>(ArmorSetCondition::Count)> names = {
        "VETERAN",
        "ELITE",
        "HERO",
        "PLAYER_UPGRADE",
        "WEAK_VERSUS_BASEDEFENSES",
        "SECOND_LIFE",
        "CRATE_UPGRADE_ONE",
        "CRATE_UPGRADE_TWO",
    };
    for (std::size_t index = 0; index < names.size(); ++index)
    {
        if (asciiEqualIgnoreCase(token, names[index]))
        {
            return static_cast<ArmorSetCondition>(index);
        }
    }
    return std::nullopt;
}

std::optional<WeaponSlot> tryParseWeaponSlot(container::StringView token) noexcept
{
    if (asciiEqualIgnoreCase(token, "PRIMARY"))
        return WeaponSlot::Primary;
    if (asciiEqualIgnoreCase(token, "SECONDARY"))
        return WeaponSlot::Secondary;
    if (asciiEqualIgnoreCase(token, "TERTIARY"))
        return WeaponSlot::Tertiary;
    return std::nullopt;
}

std::optional<WeaponCommandSource> tryParseWeaponCommandSource(container::StringView token) noexcept
{
    if (asciiEqualIgnoreCase(token, "FROM_PLAYER"))
        return WeaponCommandSource::Player;
    if (asciiEqualIgnoreCase(token, "FROM_SCRIPT"))
        return WeaponCommandSource::Script;
    if (asciiEqualIgnoreCase(token, "FROM_AI"))
        return WeaponCommandSource::AI;
    if (asciiEqualIgnoreCase(token, "FROM_DOZER"))
        return WeaponCommandSource::Dozer;
    if (asciiEqualIgnoreCase(token, "DEFAULT_SWITCH_WEAPON"))
    {
        return WeaponCommandSource::DefaultSwitchWeapon;
    }
    return std::nullopt;
}

CombatProfileParseStatus applyWeaponSetConditions(container::StringView text, WeaponSetConditionMask& inOutMask)
{
    return applyNamedBitString(
        text, inOutMask, kAllWeaponSetConditions, false, "WeaponSet Conditions", tryParseWeaponSetCondition);
}

CombatProfileParseStatus applyArmorSetConditions(container::StringView text, ArmorSetConditionMask& inOutMask)
{
    return applyNamedBitString(
        text, inOutMask, kAllArmorSetConditions, false, "ArmorSet Conditions", tryParseArmorSetCondition);
}

CombatProfileParseStatus applyWeaponAutoChooseSources(container::StringView text, WeaponAutoChooseSourceMask& inOutMask)
{
    return applyNamedBitString(
        text, inOutMask, kAllWeaponAutoChooseSources, true, "WeaponSet AutoChooseSources", tryParseWeaponCommandSource);
}

bool WeaponSlotProfile::allowsAutoChoose(WeaponCommandSource source) const noexcept
{
    const WeaponAutoChooseSourceMask requested = weaponAutoChooseSourceBit(source);
    const WeaponAutoChooseSourceMask fallback = weaponAutoChooseSourceBit(WeaponCommandSource::DefaultSwitchWeapon);
    return (autoChooseSources & (requested | fallback)) != 0;
}

WeaponSlotSelection chooseBestWeaponSlot(const WeaponSetProfile& profile,
                                         const container::Array<WeaponSlotSelectionCandidate, kWeaponSlotCount>& candidates,
                                         WeaponCommandSource commandSource,
                                         WeaponChoiceCriterion criterion,
                                         bool hasTarget,
                                         std::optional<WeaponSlot> lockedSlot) noexcept
{
    WeaponSlotSelection result;
    if (lockedSlot)
    {
        // RefCode returns immediately while locked; the caller that created
        // the lock owns its validity and releases it when the set changes.
        // Selection still has to report the current weapon's readiness: the
        // legacy caller queried the locked Weapon object after this early
        // return, whereas the ECS runtime carries that fact in the result.
        result.slot = *lockedSlot;
        result.found = true;
        result.heldByLock = true;
        const std::size_t index = static_cast<std::size_t>(*lockedSlot);
        result.usesReadyWeapon = index < candidates.size() &&
            candidates[index].present && candidates[index].readyToFire;
        return result;
    }

    if (!hasTarget)
    {
        // The original intentionally selects PRIMARY for ground attacks even
        // for spell-only units that do not physically have a primary weapon.
        result.found = true;
        return result;
    }

    bool foundReady = false;
    bool foundBackup = false;
    math::q32_32 bestReadyScore{};
    math::q32_32 bestBackupScore{};
    WeaponSlot readySlot = WeaponSlot::Primary;
    WeaponSlot backupSlot = WeaponSlot::Primary;

    for (int index = static_cast<int>(WeaponSlot::Tertiary); index >= static_cast<int>(WeaponSlot::Primary); --index)
    {
        const WeaponSlot slot = static_cast<WeaponSlot>(index);
        const WeaponSlotProfile& definition = profile.slots[static_cast<std::size_t>(index)];
        const WeaponSlotSelectionCandidate& candidate = candidates[static_cast<std::size_t>(index)];

        if (!definition.hasWeapon() || !candidate.present)
            continue;
        if (!definition.allowsAutoChoose(commandSource))
            continue;
        if (candidate.outOfAmmo && !candidate.autoReloadsClip)
            continue;
        if (!candidate.canTarget || !candidate.withinTargetPitch)
            continue;

        math::q32_32 damage = candidate.estimatedDamage;
        math::q32_32 range = candidate.attackRange;
        if (damage <= math::q32_32{} && !candidate.permitsZeroDamage)
            continue;

        bool ready = candidate.readyToFire;
        if (candidate.preferredAgainstTarget)
        {
            // Exact legacy behavior: preferred slots outrank all normal
            // scores and remain preferred while reloading, but not empty.
            damage = math::q32_32::from_raw(
                std::numeric_limits<int64_t>::max());
            range = damage;
            ready = !candidate.outOfAmmo;
        }

        const math::q32_32 score =
            criterion == WeaponChoiceCriterion::MostDamage ? damage : range;
        if (ready)
        {
            // RefCode uses >= for damage and > for range. Since iteration is
            // TERTIARY -> PRIMARY, that means damage ties prefer PRIMARY,
            // while range ties retain the first (highest numbered) slot.
            const bool wins =
                criterion == WeaponChoiceCriterion::MostDamage ? score >= bestReadyScore : score > bestReadyScore;
            if (wins)
            {
                bestReadyScore = score;
                readySlot = slot;
                foundReady = true;
            }
        }
        else
        {
            const bool wins =
                criterion == WeaponChoiceCriterion::MostDamage ? score >= bestBackupScore : score > bestBackupScore;
            if (wins)
            {
                bestBackupScore = score;
                backupSlot = slot;
                foundBackup = true;
            }
        }
    }

    if (foundReady)
    {
        result.slot = readySlot;
        result.found = true;
        result.usesReadyWeapon = true;
    }
    else if (foundBackup)
    {
        result.slot = backupSlot;
        result.found = true;
    }
    return result;
}

CombatProfile::CombatProfile(container::Vector<WeaponSetProfile> weaponSets, container::Vector<ArmorSetProfile> armorSets) noexcept
    : m_weaponSets(std::move(weaponSets))
    , m_armorSets(std::move(armorSets))
{
}

container::Span<const WeaponSetProfile> CombatProfile::weaponSets() const noexcept
{
    return m_weaponSets;
}

container::Span<const ArmorSetProfile> CombatProfile::armorSets() const noexcept
{
    return m_armorSets;
}

const WeaponSetProfile* CombatProfile::findBestWeaponSet(WeaponSetConditionMask activeConditions) const noexcept
{
    return findBestConditionSet<WeaponSetProfile>(m_weaponSets, activeConditions, kAllWeaponSetConditions);
}

const ArmorSetProfile* CombatProfile::findBestArmorSet(ArmorSetConditionMask activeConditions) const noexcept
{
    return findBestConditionSet<ArmorSetProfile>(m_armorSets, activeConditions, kAllArmorSetConditions);
}

bool CombatProfile::hasAnyWeapons() const noexcept
{
    return std::any_of(m_weaponSets.begin(),
                       m_weaponSets.end(),
                       [](const WeaponSetProfile& set)
                       {
                           return std::any_of(set.slots.begin(),
                                              set.slots.end(),
                                              [](const WeaponSlotProfile& slot) { return slot.hasWeapon(); });
                       });
}

CombatProfileCompileResult compileCombatProfile(container::Span<const IniBlock> directObjectChildren,
                                                CombatProfileCompileOptions options)
{
    CombatProfileCompileResult result;
    container::Vector<WeaponSetProfile> weaponSets;
    container::Vector<ArmorSetProfile> armorSets;
    if (options.inherited != nullptr)
    {
        weaponSets.assign(options.inherited->weaponSets().begin(), options.inherited->weaponSets().end());
        armorSets.assign(options.inherited->armorSets().begin(), options.inherited->armorSets().end());
    }

    bool sawLocalWeaponSet = false;
    bool sawLocalArmorSet = false;
    for (const IniBlock& child : directObjectChildren)
    {
        if (asciiEqualIgnoreCase(child.type, "WeaponSet"))
        {
            if (!sawLocalWeaponSet &&
                options.inheritanceMode == CombatProfileInheritanceMode::ReplaceInheritedOnFirstLocalSet)
            {
                weaponSets.clear();
            }
            sawLocalWeaponSet = true;
            WeaponSetProfile parsed;
            if (parseWeaponSetBlock(child, parsed, result))
            {
                weaponSets.push_back(std::move(parsed));
            }
        }
        else if (asciiEqualIgnoreCase(child.type, "ArmorSet"))
        {
            if (!sawLocalArmorSet &&
                options.inheritanceMode == CombatProfileInheritanceMode::ReplaceInheritedOnFirstLocalSet)
            {
                armorSets.clear();
            }
            sawLocalArmorSet = true;
            ArmorSetProfile parsed;
            if (parseArmorSetBlock(child, parsed, result))
            {
                armorSets.push_back(std::move(parsed));
            }
        }
    }

    validateDuplicateConditions<WeaponSetProfile>(weaponSets, "WeaponSet", result);
    validateDuplicateConditions<ArmorSetProfile>(armorSets, "ArmorSet", result);
    result.profile =
        container::SharedPtr<const CombatProfile>(new CombatProfile(std::move(weaponSets), std::move(armorSets)));
    return result;
}

} // namespace game

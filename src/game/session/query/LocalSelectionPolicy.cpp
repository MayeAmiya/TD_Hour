#include "game/session/query/LocalSelectionPolicy.h"

#include "game/selection/LocalSelectionState.h"
#include "core/container/string_utils.h"
#include "game/session/core/GameSession.h"
#include "game/session/query/LocalSelectionQueryPort.h"
#include "game/session/query/SessionPlayerQuery.h"

#include <algorithm>
#include <utility>

namespace engine::selection {
namespace {

[[nodiscard]] container::Vector<ObjectId> canonical(
    container::Vector<ObjectId> objects) {
    objects.erase(
        std::remove(objects.begin(), objects.end(), INVALID_OBJECT_ID),
        objects.end());
    std::sort(objects.begin(), objects.end(),
              [](ObjectId left, ObjectId right) {
                  return left.value < right.value;
              });
    objects.erase(std::unique(objects.begin(), objects.end()), objects.end());
    return objects;
}

[[nodiscard]] bool currentIsOnlyLocalUnits(
    const LocalSelectionQueryPort& source,
    const LocalSelectionState& selection,
    PlayerId localPlayer) noexcept {
    if (selection.selected().empty()) return true;
    for (const ObjectId object : selection.selected()) {
        const LocalSelectionObjectSnapshot facts =
            source.inspect(localPlayer, object);
        if (!facts.selectable || !facts.local || facts.structure) return false;
    }
    return true;
}

[[nodiscard]] container::Vector<ObjectId> allMatchingType(
    const LocalSelectionQueryPort& source, PlayerId localPlayer,
    container::StringView type, bool includeCarBombs = false) {
    container::Vector<ObjectId> result;
    if (type.empty()) return result;
    for (const ObjectId object : source.allObjects()) {
        const LocalSelectionObjectSnapshot facts =
            source.inspect(localPlayer, object);
        if (facts.selectable && facts.local && !facts.structure &&
            (source.matchesType(facts.object, type) ||
             (includeCarBombs && facts.carBomb))) {
            result.push_back(object);
        }
    }
    return canonical(std::move(result));
}

[[nodiscard]] container::Vector<ObjectId> filterLocalUnits(
    const LocalSelectionQueryPort& source, PlayerId localPlayer,
    container::Span<const ObjectId> candidates,
    container::StringView matchingType = {},
    bool includeCarBombs = false) {
    container::Vector<ObjectId> result;
    result.reserve(candidates.size());
    for (const ObjectId object : candidates) {
        const LocalSelectionObjectSnapshot facts =
            source.inspect(localPlayer, object);
        if (!facts.selectable || !facts.local || facts.structure ||
            (!source.matchesType(facts.object, matchingType) &&
             !(includeCarBombs && facts.carBomb))) {
            continue;
        }
        result.push_back(object);
    }
    return canonical(std::move(result));
}

[[nodiscard]] bool matchesAnyType(
    const LocalSelectionQueryPort& source,
    const LocalSelectionObjectSnapshot& facts,
    container::Span<const container::String> types) noexcept {
    return std::any_of(
        types.begin(), types.end(),
        [&source, &facts](const container::String& type) noexcept {
            return source.matchesType(facts.object, type);
        });
}

[[nodiscard]] container::Vector<ObjectId> matchingLocalUnits(
    const LocalSelectionQueryPort& source, PlayerId localPlayer,
    container::Span<const ObjectId> candidates,
    container::Span<const container::String> types,
    bool includeCarBombs) {
    container::Vector<ObjectId> result;
    result.reserve(candidates.size());
    for (const ObjectId object : candidates) {
        const LocalSelectionObjectSnapshot facts =
            source.inspect(localPlayer, object);
        if (!facts.selectable || !facts.local || facts.structure ||
            (!matchesAnyType(source, facts, types) &&
             !(includeCarBombs && facts.carBomb))) {
            continue;
        }
        result.push_back(object);
    }
    return canonical(std::move(result));
}

[[nodiscard]] container::Vector<ObjectId> allMatchingLocalUnits(
    const LocalSelectionQueryPort& source, PlayerId localPlayer,
    container::Span<const container::String> types,
    bool includeCarBombs) {
    container::Vector<ObjectId> result;
    for (const ObjectId object : source.allObjects()) {
        const LocalSelectionObjectSnapshot facts =
            source.inspect(localPlayer, object);
        if (!facts.selectable || !facts.local || facts.structure ||
            (!matchesAnyType(source, facts, types) &&
             !(includeCarBombs && facts.carBomb))) {
            continue;
        }
        result.push_back(object);
    }
    return canonical(std::move(result));
}

[[nodiscard]] container::Vector<ObjectId> filterControlGroupObjects(
    const LocalSelectionQueryPort& source, PlayerId localPlayer,
    container::Span<const ObjectId> candidates) {
    container::Vector<ObjectId> units;
    container::Vector<ObjectId> structures;
    for (const ObjectId object : candidates) {
        const LocalSelectionObjectSnapshot facts =
            source.inspect(localPlayer, object);
        if (!facts.selectable || !facts.live || !facts.local) continue;
        (facts.structure ? structures : units).push_back(object);
    }
    units = canonical(std::move(units));
    structures = canonical(std::move(structures));
    if (!units.empty()) return units;
    if (structures.size() == 1u) return structures;
    return {};
}

[[nodiscard]] bool applyUnitSet(
    const LocalSelectionQueryPort& source, LocalSelectionState& selection,
    PlayerId localPlayer, container::Vector<ObjectId> objects,
    bool additive) {
    if (objects.empty()) return false;
    if (!additive ||
        !currentIsOnlyLocalUnits(source, selection, localPlayer)) {
        return selection.replace(objects);
    }
    bool changed = false;
    for (const ObjectId object : objects) {
        changed = selection.add(object) || changed;
    }
    return changed;
}

} // namespace

bool LocalSelectionPolicy::isRetainedSelectionObject(
    const GameSession& session, ObjectId object,
    bool requireLocalLiveObject) noexcept {
    const LocalSelectionQueryPort source = session.localSelectionQuery();
    PlayerId localPlayer = INVALID_PLAYER_ID;
    if (requireLocalLiveObject) {
        const auto local = session.playerQuery().localPlayer();
        if (!local) return false;
        localPlayer = local->id;
    }
    const LocalSelectionObjectSnapshot facts =
        source.inspect(localPlayer, object);
    return facts.selectable &&
        (!requireLocalLiveObject || (facts.live && facts.local));
}

LocalSelectionPolicyResult LocalSelectionPolicy::applyGesture(
    const GameSession& session, LocalSelectionState& selection,
    LocalSelectionGesture gesture) {
    LocalSelectionPolicyResult result;
    const auto local = session.playerQuery().localPlayer();
    if (!local || !local->commandPlayer) return result;
    if (gesture.kind ==
            LocalSelectionGestureKind::ExplicitTypeAcrossMap &&
        gesture.objectType.empty()) {
        return result;
    }
    const PlayerId localPlayer = local->id;
    const LocalSelectionQueryPort source = session.localSelectionQuery();
    result.accepted = true;

    if (gesture.kind == LocalSelectionGestureKind::Point) {
        const LocalSelectionObjectSnapshot facts =
            source.inspect(localPlayer, gesture.anchor);
        if (!facts.selectable) {
            if (!gesture.additive) result.changed = selection.clear();
            return result;
        }
        const container::Array<ObjectId, 1> one{facts.object};
        if (!gesture.additive || !facts.local || facts.structure ||
            !currentIsOnlyLocalUnits(source, selection, localPlayer)) {
            result.changed = selection.replace(one);
        } else {
            result.changed = selection.toggle(facts.object);
        }
        return result;
    }

    if (gesture.kind == LocalSelectionGestureKind::Rectangle) {
        container::Vector<ObjectId> units = filterLocalUnits(
            source, localPlayer, gesture.candidates);
        if (!units.empty()) {
            result.changed = applyUnitSet(
                source, selection, localPlayer, std::move(units),
                gesture.additive);
            return result;
        }
        container::Vector<ObjectId> structures;
        for (const ObjectId object : gesture.candidates) {
            const LocalSelectionObjectSnapshot facts =
                source.inspect(localPlayer, object);
            if (facts.selectable && facts.local && facts.structure) {
                structures.push_back(object);
            }
        }
        structures = canonical(std::move(structures));
        if (structures.size() == 1u) {
            result.changed = selection.replace(structures);
        } else if (!gesture.additive) {
            result.changed = selection.clear();
        }
        return result;
    }

    container::String matchingType = gesture.objectType;
    bool includeCarBombs = false;
    container::Vector<container::String> matchingTypes;
    if (gesture.kind != LocalSelectionGestureKind::ExplicitTypeAcrossMap) {
        const LocalSelectionObjectSnapshot anchor =
            source.inspect(localPlayer, gesture.anchor);
        if (!anchor.selectable || !anchor.live || !anchor.local ||
            anchor.structure || anchor.type.empty()) {
            return result;
        }
        matchingType = anchor.type;
        includeCarBombs = anchor.carBomb;
        matchingTypes.emplace_back(anchor.type);
        // Prefer-selection (Shift) restores the previous selection before
        // ZH's selectMatchingAcrossRegion(), so every selected local unit type
        // contributes, not only the object beneath the second click.
        if (gesture.additive) {
            for (const ObjectId object : selection.selected()) {
                const LocalSelectionObjectSnapshot facts =
                    source.inspect(localPlayer, object);
                if (!facts.selectable || !facts.live || !facts.local ||
                    facts.structure || facts.type.empty()) {
                    continue;
                }
                includeCarBombs = includeCarBombs || facts.carBomb;
                const bool known = std::any_of(
                    matchingTypes.begin(), matchingTypes.end(),
                    [&facts](const container::String& type) noexcept {
                        return container::asciiEqualIgnoreCase(
                            type, facts.type);
                    });
                if (!known) matchingTypes.emplace_back(facts.type);
            }
        }
    } else {
        matchingTypes.emplace_back(matchingType);
    }

    container::Vector<ObjectId> matches;
    if (gesture.kind == LocalSelectionGestureKind::MatchingTypeVisible) {
        matches = matchingLocalUnits(
            source, localPlayer, gesture.candidates, matchingTypes,
            includeCarBombs);
    } else if (gesture.kind ==
               LocalSelectionGestureKind::MatchingTypeAcrossMap) {
        matches = allMatchingLocalUnits(
            source, localPlayer, matchingTypes, includeCarBombs);
    } else {
        matches = allMatchingType(
            source, localPlayer, matchingType, includeCarBombs);
    }
    if (gesture.kind ==
            LocalSelectionGestureKind::ExplicitTypeAcrossMap &&
        matches.empty()) {
        result.changed = selection.clear();
    } else {
        result.changed = applyUnitSet(
            source, selection, localPlayer, std::move(matches),
            gesture.additive);
    }
    return result;
}

LocalSelectionPolicyResult LocalSelectionPolicy::applyControlGroup(
    const GameSession& session, LocalSelectionState& selection,
    LocalControlGroupRequest request) {
    LocalSelectionPolicyResult result;
    if (request.index >= LOCAL_CONTROL_GROUP_COUNT) return result;
    const auto local = session.playerQuery().localPlayer();
    if (!local || !local->commandPlayer) return result;
    const LocalSelectionQueryPort source = session.localSelectionQuery();
    result.accepted = true;

    if (request.operation == LocalControlGroupOperation::Save) {
        container::Vector<ObjectId> filtered = filterControlGroupObjects(
            source, local->id, selection.selected());
        result.changed = selection.saveControlGroup(
            request.index, filtered);
        return result;
    }

    if (request.operation == LocalControlGroupOperation::Append) {
        container::Vector<ObjectId> incoming = filterControlGroupObjects(
            source, local->id, selection.selected());
        if (incoming.empty()) return result;
        container::Vector<ObjectId> existing = filterControlGroupObjects(
            source, local->id, selection.controlGroup(request.index));
        const LocalSelectionObjectSnapshot incomingFacts =
            source.inspect(local->id, incoming.front());
        const bool incomingStructure = incomingFacts.structure;
        const bool existingStructure = !existing.empty() &&
            source.inspect(local->id, existing.front()).structure;
        if (incomingStructure || existingStructure) {
            result.changed = selection.saveControlGroup(
                request.index, incoming);
        } else {
            result.changed = selection.saveControlGroup(
                request.index, existing) || result.changed;
            result.changed = selection.appendToControlGroup(
                request.index, incoming) || result.changed;
        }
        return result;
    }

    container::Vector<ObjectId> recalled = filterControlGroupObjects(
        source, local->id, selection.controlGroup(request.index));
    result.changed = selection.saveControlGroup(
        request.index, recalled) || result.changed;
    if (request.operation == LocalControlGroupOperation::View) {
        if (!recalled.empty()) {
            result.cameraTarget = source.cameraTarget(recalled.back());
        }
        return result;
    }
    result.changed = selection.replace(recalled) || result.changed;
    if (request.focusCamera && !recalled.empty()) {
        result.cameraTarget = source.cameraTarget(recalled.back());
    }
    return result;
}

LocalSelectionPolicyResult LocalSelectionPolicy::applyShortcut(
    const GameSession& session, LocalSelectionState& selection,
    LocalSelectionShortcut shortcut) {
    LocalSelectionPolicyResult result;
    const auto local = session.playerQuery().localPlayer();
    if (!local || !local->commandPlayer) return result;
    const LocalSelectionQueryPort source = session.localSelectionQuery();

    const bool idleWorker =
        shortcut == LocalSelectionShortcut::NextIdleWorker;
    const bool worker = shortcut == LocalSelectionShortcut::NextWorker ||
        shortcut == LocalSelectionShortcut::PreviousWorker || idleWorker;
    const bool hero = shortcut == LocalSelectionShortcut::Hero;
    const bool commandCenter = shortcut == LocalSelectionShortcut::CommandCenter;
    const bool previous = shortcut == LocalSelectionShortcut::PreviousUnit ||
        shortcut == LocalSelectionShortcut::PreviousWorker;

    container::Vector<ObjectId> candidates;
    for (const ObjectId object : source.allObjects()) {
        const LocalSelectionObjectSnapshot facts =
            source.inspect(local->id, object);
        if (!facts.selectable || !facts.local) continue;
        // RefCode's MSG_META_SELECT_NEXT_UNIT / _PREV_UNIT rosters require
        // isMobile() && isLocallyControlled() && !isContained() &&
        // !isKindOf(KINDOF_NO_SELECT). Only that cycling family consults
        // NO_SELECT; the worker, hero and command-center shortcuts are
        // separate messages that do not, and stock drones are authored
        // SELECTABLE + NO_SELECT precisely so they stay box-selectable while
        // dropping out of this cycle.
        const bool cyclesAsPlainUnit =
            !worker && !hero && !commandCenter && !facts.structure &&
            !source.hasKind(object, game::ObjectKindOf::NoSelect);
        const bool matches =
            (worker && source.hasKind(object, game::ObjectKindOf::Dozer)) ||
            (hero && source.hasKind(object, game::ObjectKindOf::Hero)) ||
            (commandCenter && source.hasKind(
                object, game::ObjectKindOf::CommandCenter)) ||
             cyclesAsPlainUnit;
        if (matches && idleWorker) {
            // RefCode's idle-worker roster is driven by
            // AIUpdateInterface::isIdle(), not by whether the command queue
            // happens to be empty.  A completed order can leave the queue
            // empty while its AI state is still moving/building; conversely,
            // the authoritative runtime is the only owner that knows when
            // that state has actually returned to Idle.
            if (!source.isIdleAiObject(object)) continue;
        }
        if (matches) candidates.push_back(object);
    }
    candidates = canonical(std::move(candidates));
    if (candidates.empty()) return result;

    ObjectId current = INVALID_OBJECT_ID;
    if (!selection.selected().empty()) current = selection.selected().front();
    auto found = std::find(candidates.begin(), candidates.end(), current);
    size_t index = found == candidates.end()
        ? 0u : static_cast<size_t>(std::distance(candidates.begin(), found));
    if (previous) {
        index = index == 0u ? candidates.size() - 1u : index - 1u;
    } else if (found != candidates.end()) {
        index = (index + 1u) % candidates.size();
    }
    const ObjectId chosen = candidates[index];
    result.accepted = true;
    result.changed = selection.replace(container::Span<const ObjectId>{
        &chosen, 1u});
    result.cameraTarget = source.cameraTarget(chosen);
    return result;
}

LocalUnitVoiceRequest LocalSelectionPolicy::selectionVoice(
    const GameSession& session, const LocalSelectionState& selection) {
    const auto local = session.playerQuery().localPlayer();
    if (!local || !local->commandPlayer) return {};
    if (selection.selected().empty()) return {};
    const LocalSelectionQueryPort source = session.localSelectionQuery();

    // Pick one speaker and count how many share its type, so a mixed
    // selection still answers with something rather than nothing. Only
    // locally controlled, non-structure units speak: a captured enemy tank
    // the player can merely see must stay quiet, and buildings have no
    // select voice in shipped content.
    ObjectId speaker = INVALID_OBJECT_ID;
    container::StringView speakerType;
    size_t sameTypeCount = 0;
    for (const ObjectId object : selection.selected()) {
        const LocalSelectionObjectSnapshot facts =
            source.inspect(local->id, object);
        if (!facts.selectable || !facts.local || facts.structure) continue;
        if (speaker == INVALID_OBJECT_ID || object.value < speaker.value) {
            speaker = object;
            speakerType = facts.type;
        }
    }
    if (speaker == INVALID_OBJECT_ID) return {};
    for (const ObjectId object : selection.selected()) {
        const LocalSelectionObjectSnapshot facts =
            source.inspect(local->id, object);
        if (facts.selectable && facts.local && !facts.structure &&
            container::asciiEqualIgnoreCase(facts.type, speakerType)) {
            ++sameTypeCount;
        }
    }
    container::String eventName = source.voiceCue(
        speaker,
        sameTypeCount > 1u ? LocalUnitVoiceCue::GroupSelect
                           : LocalUnitVoiceCue::Select);
    if (eventName.empty()) return {};
    return {.eventName = std::move(eventName), .object = speaker};
}

} // namespace engine::selection

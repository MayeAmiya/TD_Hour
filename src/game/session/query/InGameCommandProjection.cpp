#include "game/session/query/InGameCommandProjection.h"

#include "game/session/query/LocalSelectionCommandBarPresentationConsumer.h"
#include "game/selection/LocalSelectionState.h"
#include "game/session/core/GameSession.h"
#include "game/session/query/InGameCommandQuerySource.h"
#include "game/session/query/GameSessionCommandQueryPort.h"
#include "game/session/query/GameSessionEconomyQueryPort.h"
#include "game/session/query/GameSessionObjectQueryPort.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace engine::session_query {
namespace {

[[nodiscard]] bool sameStamp(
    const game::CommandBarOverrideMutationStamp& lhs,
    const game::CommandBarOverrideMutationStamp& rhs) noexcept {
    return lhs.presentationEpoch == rhs.presentationEpoch &&
        lhs.sequence == rhs.sequence &&
        lhs.confirmedTick == rhs.confirmedTick &&
        lhs.sourceScriptId == rhs.sourceScriptId &&
        lhs.ordinal == rhs.ordinal;
}

[[nodiscard]] bool sameCommandBar(
    const InGameCommandProjection& lhs,
    const InGameCommandProjection& rhs) noexcept {
    return lhs.hasSelection == rhs.hasSelection &&
        lhs.multiSelection == rhs.multiSelection &&
        lhs.selectedCount == rhs.selectedCount &&
        lhs.selectedObject == rhs.selectedObject &&
        lhs.selectedObjectType == rhs.selectedObjectType &&
        lhs.selectedPortraitImage == rhs.selectedPortraitImage &&
        lhs.upgradeCameos == rhs.upgradeCameos &&
        lhs.selectedUnderConstruction == rhs.selectedUnderConstruction &&
        lhs.selectedOrderWaypoint == rhs.selectedOrderWaypoint &&
        lhs.selectedOrderWaypointSourceSequence ==
            rhs.selectedOrderWaypointSourceSequence &&
        lhs.selectedOrderWaypointKind == rhs.selectedOrderWaypointKind &&
        lhs.hasCommandSet == rhs.hasCommandSet &&
        lhs.slots == rhs.slots &&
        lhs.availability == rhs.availability &&
        lhs.inventorySlots == rhs.inventorySlots &&
        lhs.inventoryPassengers == rhs.inventoryPassengers &&
        lhs.actionActors == rhs.actionActors &&
        lhs.selectionRevision == rhs.selectionRevision &&
        sameStamp(lhs.commandBarMutation, rhs.commandBarMutation) &&
        lhs.objectCommandSetRevision == rhs.objectCommandSetRevision;
}

void buildActionTokens(const engine::GameSession& session,
                       InGameCommandProjection& projection) {
    for (size_t slot = 0; slot < projection.slots.size(); ++slot) {
        const engine::script::ScriptCommandBarUiSlot& source =
            projection.slots[slot];
        InGameCommandActionToken& token = projection.actionTokens[slot];
        token.slot = static_cast<uint32_t>(slot);
        if (!projection.hasSelection || !source.visible ||
            source.commandButtonName.empty() ||
            !projection.availability[slot].visible ||
            !projection.availability[slot].enabled) {
            continue;
        }
        const game::CommandButtonTemplate* button =
            inGameCommandQuerySource(session).findCommandButton(
                source.commandButtonName);
        if (!button || !button->descriptor.userActivatable()) continue;
        token.commandBarRevision = projection.commandBarRevision;
        token.selectionRevision = projection.selectionRevision;
        token.selectedObject = projection.actionActors[slot]
            ? projection.actionActors[slot] : projection.selectedObject;
        token.targetObject = projection.inventoryPassengers[slot];
        token.orderWaypoint = projection.selectedOrderWaypoint;
        token.orderWaypointSourceSequence =
            projection.selectedOrderWaypointSourceSequence;
        token.orderWaypointKind = projection.selectedOrderWaypointKind;
        token.descriptor = button->descriptor;
        token.availability = projection.availability[slot];
        token.unitSpecificSound = button->unitSpecificSound;
    }
}

[[nodiscard]] bool resolveCommandBarActor(
    const engine::GameSession& session, engine::ObjectId object) {
    return inGameCommandQuerySource(session).isCommandBarActor(
        object, false);
}

[[nodiscard]] uint64_t appendRevisionHash(
    uint64_t hash, uint64_t value) noexcept {
    constexpr uint64_t kPrime = 1099511628211ull;
    for (size_t byte = 0; byte < sizeof(value); ++byte) {
        hash ^= static_cast<uint8_t>(value >> (byte * 8u));
        hash *= kPrime;
    }
    return hash;
}

void buildSelectionSlots(
    const engine::GameSession& session,
    const engine::selection::LocalSelectionState& selection,
    InGameCommandProjection& projection,
    bool& selectedActor) {
    const container::Span<const engine::ObjectId> selected =
        selection.selected();
    const engine::selection::LocalOrderWaypointSelection waypoint =
        selection.selectedOrderWaypoint();
    if (waypoint) {
        const InGameOrderWaypointReadModel read =
            inGameCommandQuerySource(session).orderWaypoint(
                waypoint.actor, waypoint.sourceSequence);
        if (!read.exists || read.kind != waypoint.kind) return;
        projection.selectedCount = 1;
        projection.hasSelection = true;
        projection.selectedObject = waypoint.actor;
        projection.selectedObjectType = read.objectType;
        projection.selectedPortraitImage = read.portraitImage;
        projection.selectedOrderWaypoint = true;
        projection.selectedOrderWaypointSourceSequence =
            waypoint.sourceSequence;
        projection.selectedOrderWaypointKind = waypoint.kind;
        projection.selectedUnderConstruction =
            waypoint.kind ==
                engine::selection::LocalOrderWaypointKind::Build;
        if (const game::CommandButtonTemplate* cancel =
                inGameCommandQuerySource(session).findCommandButton(
                    "Command_CancelConstruction");
            cancel && cancel->descriptor.kind ==
                game::CommandButtonKind::DozerConstructCancel) {
            projection.hasCommandSet = true;
            projection.slots[0] = {
                .visible = true,
                .commandButtonName = cancel->name,
                .buttonImage = cancel->buttonImage,
                .textLabel = cancel->textLabel,
                .descriptionLabel = cancel->descriptionLabel,
                .borderType = cancel->borderType,
            };
        }
        projection.objectCommandSetRevision = appendRevisionHash(
            appendRevisionHash(14695981039346656037ull,
                               waypoint.actor.value),
            waypoint.sourceSequence);
        return;
    }
    projection.selectedCount = static_cast<uint32_t>(std::min<size_t>(
        selected.size(), std::numeric_limits<uint32_t>::max()));
    if (selected.empty()) return;

    container::Vector<engine::ObjectId> commandBarActors;

    if (selected.size() == 1) {
        const std::optional<engine::selection::LocalCommandBarSelection>
            resolved = engine::selection::
                LocalSelectionCommandBarPresentationConsumer::
                    resolveSingleObject(session, selection);
        if (!resolved) return;
        projection.hasSelection = true;
        projection.selectedObject = resolved->object;
        projection.selectedObjectType = resolved->objectType;
        const InGameSelectionPresentationReadModel selectionPresentation =
            inGameCommandQuerySource(session).objectSelectionPresentation(
                resolved->object);
        projection.selectedPortraitImage =
            selectionPresentation.portraitImage;
        for (size_t index = 0;
             index < projection.upgradeCameos.size(); ++index) {
            projection.upgradeCameos[index] = {
                .buttonImage =
                    selectionPresentation.upgrades[index].buttonImage,
                .visible = selectionPresentation.upgrades[index].visible,
                .complete = selectionPresentation.upgrades[index].complete,
            };
        }
        selectedActor = resolveCommandBarActor(session, resolved->object);
        const InGameConstructionReadModel construction =
            inGameCommandQuerySource(session).objectConstruction(
                resolved->object);
        projection.selectedUnderConstruction =
            construction.underConstruction;
        projection.constructionProgressPermille =
            construction.progressPermille;
    } else {
        projection.multiSelection = true;
        for (const engine::ObjectId object : selected) {
            if (!inGameCommandQuerySource(session).isCommandBarActor(
                    object, true)) {
                continue;
            }
            commandBarActors.push_back(object);
        }
        if (commandBarActors.empty()) return;
        projection.hasSelection = true;
        projection.selectedObject = commandBarActors.front();
        selectedActor = true;
        projection.selectedObjectType.assign(
            inGameCommandQuerySource(session).objectTypeName(commandBarActors.front()));
        const bool homogeneousType = std::all_of(
            commandBarActors.begin() + 1, commandBarActors.end(),
            [&session, &projection](engine::ObjectId object) {
                return inGameCommandQuerySource(session).objectTypeName(object) ==
                    projection.selectedObjectType;
            });
        if (!homogeneousType) projection.selectedObjectType.clear();
    }

    if (projection.selectedUnderConstruction) {
        // CP_UNDER_CONSTRUCTION is a dedicated one-command context in RefCode;
        // it does not inherit the finished structure's CommandSet.
        if (const game::CommandButtonTemplate* cancel =
                inGameCommandQuerySource(session).findCommandButton(
                    "Command_CancelConstruction");
            cancel && cancel->descriptor.kind ==
                game::CommandButtonKind::DozerConstructCancel) {
            projection.hasCommandSet = true;
            projection.slots[0] = {
                .visible = true,
                .commandButtonName = cancel->name,
                .buttonImage = cancel->buttonImage,
                .textLabel = cancel->textLabel,
                .descriptionLabel = cancel->descriptionLabel,
                .borderType = cancel->borderType,
            };
        }
    } else {
        engine::script::ScriptCommandBarPresentationConsumer consumer;
        static_cast<void>(synchronizeEffectiveCommandBar(
            consumer, session, projection.selectedObject));
        projection.hasCommandSet = consumer.hasCommandSet();
        projection.slots = consumer.slots();
    }

    if (!projection.multiSelection && selectedActor) {
        const GameSessionCommandQueryPort commandQuery =
            session.commandQuery();
        const container::Vector<engine::ObjectId> passengers =
            commandQuery.containmentPassengers(projection.selectedObject);
        size_t inventoryOrdinal = 0;
        for (size_t slot = 0; slot < projection.slots.size(); ++slot) {
            const engine::script::ScriptCommandBarUiSlot& source =
                projection.slots[slot];
            const game::CommandButtonTemplate* inventoryButton =
                source.visible && !source.commandButtonName.empty()
                ? inGameCommandQuerySource(session).findCommandButton(
                      source.commandButtonName)
                : nullptr;
            if (!inventoryButton || inventoryButton->descriptor.kind !=
                    game::CommandButtonKind::ExitContainer) {
                continue;
            }
            projection.inventorySlots[slot] = true;
            const engine::ObjectId passenger =
                inventoryOrdinal < passengers.size()
                ? passengers[inventoryOrdinal]
                : engine::INVALID_OBJECT_ID;
            ++inventoryOrdinal;
            projection.inventoryPassengers[slot] = passenger;
            if (!passenger) continue;
            container::String image =
                inGameCommandQuerySource(session).objectButtonImage(passenger);
            if (!image.empty())
                projection.slots[slot].buttonImage = std::move(image);
        }
    }

    constexpr uint64_t kRevisionHashBasis = 14695981039346656037ull;
    if (projection.selectedUnderConstruction) {
        // Construction progress changes the object's simulation revision every
        // tick, but CP_UNDER_CONSTRUCTION remains the same one-button command
        // context until construction completes. Keep its layout identity
        // stable so a valid Cancel click is not rejected as a stale layout.
        constexpr uint64_t kUnderConstructionContextMarker =
            0x43505F554E444552ull; // "CP_UNDER"
        projection.objectCommandSetRevision = appendRevisionHash(
            appendRevisionHash(kRevisionHashBasis,
                               projection.selectedObject.value),
            kUnderConstructionContextMarker);
    } else {
        uint64_t commandSetRevisionHash = kRevisionHashBasis;
        const container::Span<const engine::ObjectId> commandSetActors =
            projection.multiSelection
            ? container::Span<const engine::ObjectId>{
                  commandBarActors.data(), commandBarActors.size()}
            : selected;
        for (const engine::ObjectId object : commandSetActors) {
            const uint64_t revision =
                inGameCommandQuerySource(session).commandSetRevision(object);
            commandSetRevisionHash = appendRevisionHash(
                appendRevisionHash(commandSetRevisionHash, object.value),
                revision);
        }
        projection.objectCommandSetRevision = commandSetRevisionHash;
    }

    if (!projection.multiSelection) return;

    using SlotArray = engine::script::
        ScriptCommandBarPresentationConsumer::SlotArray;
    container::Vector<SlotArray> actorSlots;
    actorSlots.reserve(commandBarActors.size());
    actorSlots.push_back(projection.slots);
    for (size_t actorIndex = 1; actorIndex < commandBarActors.size();
         ++actorIndex) {
        engine::script::ScriptCommandBarPresentationConsumer actorConsumer;
        static_cast<void>(synchronizeEffectiveCommandBar(
            actorConsumer, session, commandBarActors[actorIndex]));
        actorSlots.push_back(actorConsumer.slots());
    }

    bool anyCommonCommand = false;
    for (size_t slot = 0; slot < projection.slots.size(); ++slot) {
        engine::script::ScriptCommandBarUiSlot common = actorSlots.front()[slot];
        const game::CommandButtonTemplate* commonButton =
            common.visible && !common.commandButtonName.empty()
            ? inGameCommandQuerySource(session).findCommandButton(
                  common.commandButtonName)
            : nullptr;
        if (!commonButton || !game::hasCommandButtonOption(
                commonButton->descriptor.options,
                game::CommandButtonOption::OkForMultiSelect)) {
            common = {};
            commonButton = nullptr;
        }

        for (size_t actorIndex = 1; actorIndex < actorSlots.size();
             ++actorIndex) {
            const engine::script::ScriptCommandBarUiSlot& candidateSlot =
                actorSlots[actorIndex][slot];
            const game::CommandButtonTemplate* candidate =
                candidateSlot.visible &&
                    !candidateSlot.commandButtonName.empty()
                ? inGameCommandQuerySource(session).findCommandButton(
                      candidateSlot.commandButtonName)
                : nullptr;
            const bool candidateAttackMove = candidate &&
                candidate->descriptor.kind ==
                    game::CommandButtonKind::AttackMove;
            const bool commonAttackMove = commonButton &&
                commonButton->descriptor.kind ==
                    game::CommandButtonKind::AttackMove;

            // Retail preserves AttackMove in a physical slot when any member
            // contributes it, allowing combat units to remain mixed with a
            // dozer or pilot. If the first member had no common command, adopt
            // the later AttackMove slot as the visible source.
            if (candidateAttackMove || commonAttackMove) {
                if (!commonButton && candidateAttackMove) {
                    common = candidateSlot;
                    commonButton = candidate;
                }
                continue;
            }
            if (!commonButton || !candidate ||
                candidate->descriptor.stableId !=
                    commonButton->descriptor.stableId ||
                !game::hasCommandButtonOption(
                    candidate->descriptor.options,
                    game::CommandButtonOption::OkForMultiSelect)) {
                common = {};
                commonButton = nullptr;
            }
        }
        projection.slots[slot] = std::move(common);
        anyCommonCommand = anyCommonCommand ||
            projection.slots[slot].visible;
    }
    projection.hasCommandSet = anyCommonCommand;
}

void buildProductionQueue(
    const engine::GameSession& session,
    bool selectedActor,
    InGameCommandProjection& projection) {
    if (!projection.hasSelection || projection.multiSelection ||
        !projection.selectedObject || !selectedActor) {
        return;
    }
    const InGameProductionReadModel source =
        inGameCommandQuerySource(session).productionQueue(
            projection.selectedObject,
            InGameProductionQueueProjection::kMaximumVisibleItems);
    if (source.items.empty()) return;

    InGameProductionQueueProjection& queue = projection.productionQueue;
    queue.revision = source.revision;
    queue.producer = projection.selectedObject;
    queue.count = static_cast<uint16_t>(std::min<size_t>(
        source.totalCount, std::numeric_limits<uint16_t>::max()));
    queue.capacity = static_cast<uint16_t>(std::min<uint32_t>(
        source.capacity, std::numeric_limits<uint16_t>::max()));
    size_t visibleIndex = 0;
    for (const InGameProductionReadItem& input : source.items) {
        // A production item without an authored cameo cannot be represented
        // by this WND row.  Do not let it keep the queue context alive or
        // leave an empty/stale slot; no placeholder image is fabricated.
        if (input.buttonImage.empty()) continue;
        if (visibleIndex >= queue.items.size()) break;
        InGameProductionQueueItemProjection& item = queue.items[visibleIndex++];
        item.action = {
            .selectionRevision = projection.selectionRevision,
            .producer = projection.selectedObject,
            .productionId = input.productionId,
            .kind = input.kind == InGameProductionReadKind::Unit
                ? InGameProductionQueueItemKind::Unit
                : input.kind == InGameProductionReadKind::PlayerUpgrade
                    ? InGameProductionQueueItemKind::PlayerUpgrade
                    : InGameProductionQueueItemKind::ObjectUpgrade,
            .upgradeName = input.upgradeName,
            .cancellationProductionIds =
                input.cancellationProductionIds,
            .cancellationProductionIdCount =
                input.cancellationProductionIdCount,
        };
        item.buttonImage = input.buttonImage;
        item.textLabel = input.textLabel;
        item.progressPermille = input.progressPermille;
        item.queuedCount = input.queuedCount;
    }
    queue.visibleItemCount = static_cast<uint16_t>(visibleIndex);
}

[[nodiscard]] InGameCommandAggregateAvailability aggregateAvailability(
    const engine::GameSession& session,
    const engine::selection::LocalSelectionState& selection,
    const game::CommandButtonTemplate& button) {
    const container::Span<const engine::ObjectId> selected =
        selection.selected();
    InGameCommandAggregateAvailability aggregate;
    InGameCommandSlotAvailability& result = aggregate.availability;
    bool initialized = false;
    bool anyVisible = false;
    bool anyEnabled = false;
    bool anyActive = false;
    for (const engine::ObjectId actor : selected) {
        InGameCommandSlotAvailability candidate =
            evaluateInGameCommandAvailability(
                session, selection, actor, button, true);
        if (!initialized || (!anyEnabled && candidate.enabled)) {
            result = candidate;
            aggregate.actor = actor;
            initialized = true;
        }
        anyVisible = anyVisible || candidate.visible;
        anyEnabled = anyEnabled || candidate.enabled;
        anyActive = anyActive || candidate.active;
    }
    result.visible = initialized && anyVisible;
    result.enabled = initialized && anyEnabled;
    result.active = initialized && anyActive;
    if (result.enabled) result.reason =
        InGameCommandAvailabilityReason::None;
    return aggregate;
}

void buildAvailability(
    const engine::GameSession& session,
    const engine::selection::LocalSelectionState& selection,
    const InGameCommandProjection* previous,
    container::Array<uint64_t, InGameCommandProjection::kSlotCount>&
        nextRevisions,
    InGameCommandProjection& projection) {
    for (size_t slot = 0; slot < projection.slots.size(); ++slot) {
        const engine::script::ScriptCommandBarUiSlot& source =
            projection.slots[slot];
        InGameCommandSlotAvailability& availability =
            projection.availability[slot];
        if (projection.hasSelection && source.visible &&
            !source.commandButtonName.empty()) {
            const game::CommandButtonTemplate* button =
                inGameCommandQuerySource(session).findCommandButton(
                    source.commandButtonName);
            if (button) {
                if (projection.selectedOrderWaypoint) {
                    availability.visible = true;
                    availability.enabled = true;
                    availability.reason =
                        InGameCommandAvailabilityReason::None;
                    projection.actionActors[slot] =
                        projection.selectedObject;
                } else if (projection.multiSelection) {
                    InGameCommandAggregateAvailability aggregate =
                        aggregateAvailability(session, selection, *button);
                    availability = aggregate.availability;
                    projection.actionActors[slot] = aggregate.actor;
                } else {
                    availability = evaluateInGameCommandAvailability(
                        session, selection, projection.selectedObject,
                        *button, true,
                        projection.inventoryPassengers[slot]);
                    projection.actionActors[slot] = projection.selectedObject;
                }
            } else {
                availability.reason =
                    InGameCommandAvailabilityReason::MissingButton;
            }
        }

        const bool same = previous &&
            previous->slots[slot] == projection.slots[slot] &&
            sameInGameCommandAvailabilityState(
                previous->availability[slot], availability);
        if (same) {
            availability.revision =
                previous->availability[slot].revision;
            continue;
        }
        uint64_t& next = nextRevisions[slot];
        if (next == 0) next = 1;
        availability.revision = next++;
        if (next == 0) next = 1;
    }
}

void buildBeacon(
    const engine::GameSession& session, engine::ObjectId selectedObject,
    bool selectedActor, InGameBeaconProjection& beacon) {
    beacon.object = selectedObject;
    if (!selectedActor) return;
    const InGameBeaconReadModel source =
        inGameCommandQuerySource(session).beacon(selectedObject);
    beacon.isBeacon = source.isBeacon;
    if (!beacon.isBeacon) return;
    const engine::PlayerState* local =
        inGameCommandQuerySource(session).localPlayer();
    beacon.locallyControlled = local && local->isCommandPlayer() &&
        inGameCommandQuerySource(session).ownerOf(selectedObject) ==
            std::optional<engine::PlayerId>{local->id};
    beacon.caption = source.caption;
    beacon.revision = source.revision;
}

} // namespace

bool synchronizeEffectiveCommandBar(
    engine::script::ScriptCommandBarPresentationConsumer& consumer,
    const engine::GameSession& session,
    engine::ObjectId object) {
    engine::script::ScriptCommandBarPresentationConsumer::ButtonNameArray
        effectiveButtonNames{};
    for (size_t slot = 0; slot < effectiveButtonNames.size(); ++slot) {
        effectiveButtonNames[slot].assign(
            inGameCommandQuerySource(session).effectiveObjectCommandBarButton(object, slot));
    }
    return inGameCommandQuerySource(session).synchronizeCommandBar(
        consumer, object,
        container::Span<const container::String>{
            effectiveButtonNames.data(), effectiveButtonNames.size()});
}

InGameCommandAggregateAvailability
evaluateInGameMultiCommandAvailability(
    const engine::GameSession& session,
    const engine::selection::LocalSelectionState& selection,
    const game::CommandButtonTemplate& button) {
    return aggregateAvailability(session, selection, button);
}

InGameCommandAggregateAvailability evaluateInGameShortcutAvailability(
    const InGameCommandEvaluationContext& context,
    engine::PlayerId player,
    const game::CommandButtonTemplate& button) {
    InGameCommandAggregateAvailability best;
    bool found = false;
    const auto providerClass = [](const InGameCommandSlotAvailability& value) noexcept {
        if (value.enabled) return 0u;
        if (value.reason == InGameCommandAvailabilityReason::Cooldown) return 1u;
        return 2u;
    };
    for (const engine::ObjectId actor :
         context.source.ownedObjects(player)) {
        engine::selection::LocalSelectionState providerSelection;
        const container::Array<engine::ObjectId, 1> source{actor};
        static_cast<void>(providerSelection.replace(source));
        InGameCommandSlotAvailability availability =
            evaluateInGameCommandAvailability(
                context, providerSelection, actor, button, true);
        switch (availability.reason) {
        case InGameCommandAvailabilityReason::MissingCapability:
        case InGameCommandAvailabilityReason::MissingContentReference:
        case InGameCommandAvailabilityReason::UnauthorizedActor:
        case InGameCommandAvailabilityReason::ActorUnavailable:
        case InGameCommandAvailabilityReason::MissingPrerequisiteScience:
        case InGameCommandAvailabilityReason::MissingPrerequisiteUpgrade:
            continue;
        default:
            break;
        }
        if (availability.enabled && best.availableActorCount != UINT16_MAX) {
            ++best.availableActorCount;
        }
        const uint32_t candidateClass = providerClass(availability);
        const uint32_t bestClass = found ? providerClass(best.availability) : 3u;
        const bool better = !found || candidateClass < bestClass ||
            (candidateClass == bestClass &&
             availability.cooldown.remainingTicks <
                 best.availability.cooldown.remainingTicks) ||
            (candidateClass == bestClass &&
             availability.cooldown.remainingTicks ==
                 best.availability.cooldown.remainingTicks &&
             actor.value < best.actor.value);
        if (!better) continue;
        found = true;
        best.actor = actor;
        best.availability = availability;
    }
    return best;
}

InGameCommandAggregateAvailability evaluateInGameShortcutAvailability(
    const engine::GameSession& session,
    engine::PlayerId player,
    const game::CommandButtonTemplate& button) {
    const InGameCommandQuerySource source =
        inGameCommandQuerySource(session);
    const GameSessionCommandQueryPort commands = session.commandQuery();
    const GameSessionEconomyQueryPort economy = session.economyQuery();
    return evaluateInGameShortcutAvailability(
        {
            .source = source,
            .commands = commands,
            .economy = economy,
            .confirmedTick = session.confirmedTick(),
            .logicFramesPerSecond = session.logicFramesPerSecond(),
        },
        player, button);
}

PendingWorldCommandRevalidation revalidatePendingWorldCommand(
    const engine::GameSession& session,
    const engine::selection::LocalSelectionState& selection,
    const engine::selection::PendingWorldCommandMode& mode) {
    PendingWorldCommandRevalidation result;
    const game::CommandButtonTemplate* button =
        mode.commandButtonName.empty() ? nullptr
            : inGameCommandQuerySource(session).findCommandButton(
                  mode.commandButtonName);
    result.descriptorCurrent = button &&
        button->descriptor.stableId == mode.buttonStableId &&
        button->descriptor.kind == mode.commandKind &&
        button->descriptor.userActivatable();
    if (!result.descriptorCurrent) return result;

    engine::selection::LocalSelectionState sourceSelection;
    const engine::selection::LocalSelectionState* effectiveSelection =
        &selection;
    if (mode.sourceMayBeUnselected) {
        const container::Array<engine::ObjectId, 1> source{
            mode.sourceObject};
        static_cast<void>(sourceSelection.replace(source));
        effectiveSelection = &sourceSelection;
    }
    result.availability = evaluateInGameCommandAvailability(
        session, *effectiveSelection, mode.sourceObject, *button, true);
    return result;
}

engine::ObjectId resolveInGameContainmentPassenger(
    const engine::GameSession& session, engine::ObjectId container,
    container::Span<const engine::script::ScriptCommandBarUiSlot> slots,
    size_t slotIndex) {
    if (!container || slotIndex >= slots.size())
        return engine::INVALID_OBJECT_ID;
    const container::Vector<engine::ObjectId> contents =
        session.commandQuery().containmentPassengers(container);
    size_t inventoryOrdinal = 0;
    for (size_t index = 0; index <= slotIndex; ++index) {
        const engine::script::ScriptCommandBarUiSlot& slot = slots[index];
        const game::CommandButtonTemplate* button = slot.visible &&
                !slot.commandButtonName.empty()
            ? inGameCommandQuerySource(session).findCommandButton(
                  slot.commandButtonName)
            : nullptr;
        if (!button || button->descriptor.kind !=
                game::CommandButtonKind::ExitContainer) {
            if (index == slotIndex) return engine::INVALID_OBJECT_ID;
            continue;
        }
        if (index == slotIndex) {
            return inventoryOrdinal < contents.size()
                ? contents[inventoryOrdinal]
                : engine::INVALID_OBJECT_ID;
        }
        ++inventoryOrdinal;
    }
    return engine::INVALID_OBJECT_ID;
}

bool isCurrentProductionQueueAction(
    const engine::GameSession& session, engine::PlayerId player,
    const InGameProductionQueueActionToken& token) noexcept {
    if (!token.isValid() ||
        inGameCommandQuerySource(session).ownerOf(token.producer) !=
            std::optional<engine::PlayerId>{player}) {
        return false;
    }
    const InGameProductionReadKind kind =
        token.kind == InGameProductionQueueItemKind::Unit
            ? InGameProductionReadKind::Unit
            : token.kind == InGameProductionQueueItemKind::PlayerUpgrade
                ? InGameProductionReadKind::PlayerUpgrade
                : InGameProductionReadKind::ObjectUpgrade;
    return inGameCommandQuerySource(session).productionQueueActionCurrent(
        token.producer, token.productionId, kind, token.upgradeName);
}

InGameCommandProjection InGameCommandProjectionPublisher::build(
    const engine::GameSession& session,
    const engine::selection::LocalSelectionState& selection) {
    InGameCommandProjection projection;
    projection.selectionRevision = selection.revision();
    projection.commandBarMutation =
        inGameCommandQuerySource(session).commandBarMutation();

    bool selectedActor = false;
    buildSelectionSlots(session, selection, projection, selectedActor);
    buildProductionQueue(session, selectedActor, projection);
    buildAvailability(session, selection,
                      m_hasPrevious ? &m_previous : nullptr,
                      m_nextSlotRevision, projection);

    buildBeacon(session,
                projection.multiSelection
                    ? engine::INVALID_OBJECT_ID
                    : projection.selectedObject,
                !projection.multiSelection && selectedActor,
                projection.beacon);

    if (!m_hasPrevious || !sameCommandBar(projection, m_previous)) {
        projection.commandBarRevision = m_nextCommandBarRevision++;
        if (m_nextCommandBarRevision == 0) m_nextCommandBarRevision = 1;
    } else {
        projection.commandBarRevision = m_previous.commandBarRevision;
    }
    buildActionTokens(session, projection);

    m_previous = projection;
    m_hasPrevious = true;
    return projection;
}

} // namespace engine::session_query

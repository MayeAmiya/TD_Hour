#include "game/session/query/BeaconTextCommandComposer.h"
#include "game/session/query/GameSessionCommandQueryPort.h"
#include "game/text/LanguageFilter.h"

namespace engine::selection {
namespace {

[[nodiscard]] container::String canonicalCaption(container::StringView source) {
    container::String result;
    result.reserve(ObjectDrawableCaptionComponent::MaximumCodePoints * 4u);
    size_t offset = 0;
    size_t codePoints = 0;
    while (offset < source.size() &&
           codePoints < ObjectDrawableCaptionComponent::MaximumCodePoints) {
        const unsigned char lead = static_cast<unsigned char>(source[offset]);
        if (lead == 0u) break;
        size_t bytes = 1;
        if ((lead & 0xe0u) == 0xc0u) bytes = 2;
        else if ((lead & 0xf0u) == 0xe0u) bytes = 3;
        else if ((lead & 0xf8u) == 0xf0u) bytes = 4;
        if (offset + bytes > source.size()) bytes = 1;
        for (size_t index = 1; index < bytes; ++index) {
            if ((static_cast<unsigned char>(source[offset + index]) & 0xc0u)
                    != 0x80u) {
                bytes = 1;
                break;
            }
        }
        if (bytes == 1 && lead < 0x20u)
            result.push_back(' ');
        else
            result.append(source.substr(offset, bytes));
        offset += bytes;
        ++codePoints;
    }
    return result;
}

} // namespace

BeaconTextComposeResult BeaconTextCommandComposer::compose(
    const LocalSelectionState& selection,
    const GameSessionCommandQueryPort& commands,
    PlayerId localPlayer, GameTick tick, uint32_t sequence,
    container::StringView caption) {
    BeaconTextComposeResult result;
    if (!commands.isCommandPlayer(localPlayer)) {
        result.rejection = BeaconTextComposeRejection::InvalidLocalPlayer;
        result.message = "local view has no live command player";
        return result;
    }
    if (selection.selected().size() != 1u) {
        result.rejection = BeaconTextComposeRejection::RequiresSingleSelection;
        result.message = "beacon text requires exactly one selected object";
        return result;
    }

    const ObjectId actor = selection.selected().front();
    if (!commands.isControllableBeacon(localPlayer, actor)) {
        result.rejection = BeaconTextComposeRejection::SelectionNotControllable;
        result.message = "selected object is not a live controllable beacon";
        return result;
    }

    GameCommand command;
    command.tick = tick;
    command.sequence = sequence;
    command.player = localPlayer;
    command.source = CommandSource::Local;
    command.type = GameCommandType::SetBeaconText;
    command.actors.push_back(actor);
    static_cast<void>(text::LanguageFilter::instance().ensureLoaded());
    command.commandName = canonicalCaption(
        text::LanguageFilter::instance().filterLine(caption));
    result.command = std::move(command);
    return result;
}

} // namespace engine::selection

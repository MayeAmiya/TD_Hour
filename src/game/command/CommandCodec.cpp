#include "core/container/container_types.h"
#include "CommandCodec.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace engine {
namespace {

void writeU8(container::Vector<uint8_t>& out, uint8_t value) { out.push_back(value); }

void writeU16(container::Vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
}

void writeU32(container::Vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
}

void writeU64(container::Vector<uint8_t>& out, uint64_t value) {
    writeU32(out, static_cast<uint32_t>(value));
    writeU32(out, static_cast<uint32_t>(value >> 32u));
}

void writeFixed(container::Vector<uint8_t>& out, math::q32_32 value) {
    uint64_t bits = 0;
    const int64_t raw = value.raw();
    static_assert(sizeof(bits) == sizeof(raw));
    std::memcpy(&bits, &raw, sizeof(bits));
    writeU64(out, bits);
}

class Reader final {
public:
    Reader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

    bool readU8(uint8_t& value) {
        if (m_offset >= m_size) return false;
        value = m_data[m_offset++];
        return true;
    }
    bool readU16(uint16_t& value) {
        if (m_size - m_offset < 2) return false;
        value = static_cast<uint16_t>(m_data[m_offset]) |
                (static_cast<uint16_t>(m_data[m_offset + 1]) << 8u);
        m_offset += 2;
        return true;
    }
    bool readU32(uint32_t& value) {
        if (m_size - m_offset < 4) return false;
        value = static_cast<uint32_t>(m_data[m_offset]) |
                (static_cast<uint32_t>(m_data[m_offset + 1]) << 8u) |
                (static_cast<uint32_t>(m_data[m_offset + 2]) << 16u) |
                (static_cast<uint32_t>(m_data[m_offset + 3]) << 24u);
        m_offset += 4;
        return true;
    }
    bool readU64(uint64_t& value) {
        uint32_t low = 0;
        uint32_t high = 0;
        if (!readU32(low) || !readU32(high)) return false;
        value = static_cast<uint64_t>(low) |
            (static_cast<uint64_t>(high) << 32u);
        return true;
    }
    bool readFixed(math::q32_32& value) {
        uint64_t bits = 0;
        if (!readU64(bits)) return false;
        int64_t raw = 0;
        static_assert(sizeof(bits) == sizeof(raw));
        std::memcpy(&raw, &bits, sizeof(raw));
        value = math::q32_32::from_raw(raw);
        return true;
    }
    bool readShortString(container::String& value) {
        uint16_t length = 0;
        if (!readU16(length) || length > CommandCodec::MaximumCommandNameBytes ||
            m_size - m_offset < length) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(m_data + m_offset), length);
        m_offset += length;
        return true;
    }
    [[nodiscard]] size_t offset() const noexcept { return m_offset; }
    [[nodiscard]] bool atEnd() const noexcept { return m_offset == m_size; }

private:
    const uint8_t* m_data = nullptr;
    size_t m_size = 0;
    size_t m_offset = 0;
};

[[nodiscard]] bool validSource(uint8_t source) noexcept {
    return source <= static_cast<uint8_t>(CommandSource::Replay);
}

[[nodiscard]] bool validType(uint8_t type) noexcept {
    return type <= static_cast<uint8_t>(GameCommandType::CancelOrderWaypoint);
}

[[nodiscard]] bool validActivation(const GameCommand& command) noexcept {
    const CommandActivationContext& activation = command.activation;
    if (!activation.present()) {
        if (!activation.hasPostAcceptAction()) {
            return activation.buttonStableId == 0 &&
                activation.commandKind == game::CommandButtonKind::Unknown &&
                !activation.postAcceptActor;
        }
        // A local target/placement mode has already produced its terminal UI
        // receipt, but the eventual gameplay command must still carry the
        // deterministic post-accept transition to every simulation peer.
        // Such hook-only contexts deliberately have no requestSequence.
        return activation.buttonStableId != 0 &&
            activation.commandKind != game::CommandButtonKind::Unknown &&
            activation.commandKind != game::CommandButtonKind::None &&
            activation.commandKind <= game::CommandButtonKind::SelectAllUnitsOfType &&
            activation.postAccept ==
                CommandPostAcceptAction::MarkSingleUseCommandUsed &&
            static_cast<bool>(activation.postAcceptActor);
    }
    // Clicking one of the retail production-queue cells is a direct UI
    // transaction, not an authored CommandButton activation.  It still needs
    // request correlation, but deliberately has no stable button identity.
    const bool directQueueCancellation =
        activation.buttonStableId == 0 &&
        activation.postAccept == CommandPostAcceptAction::None &&
        !activation.postAcceptActor &&
        ((command.type == GameCommandType::CancelProduction &&
          activation.commandKind ==
              game::CommandButtonKind::CancelUnitBuild) ||
         (command.type == GameCommandType::CancelPlayerUpgrade &&
          activation.commandKind == game::CommandButtonKind::CancelUpgrade));
    if (directQueueCancellation) return true;
    // A selected path node is not an authored CommandButton either.  Keep
    // its stable UI identity solely for terminal outcome projection; the
    // confirmed CancelOrderWaypoint executor is the authority for whether
    // that node still exists.
    const bool directWaypointCancellation =
        command.type == GameCommandType::CancelOrderWaypoint &&
        activation.postAccept == CommandPostAcceptAction::None &&
        !activation.postAcceptActor && activation.buttonStableId != 0 &&
        activation.commandKind != game::CommandButtonKind::Unknown &&
        activation.commandKind != game::CommandButtonKind::None &&
        activation.commandKind <= game::CommandButtonKind::SelectAllUnitsOfType;
    if (directWaypointCancellation) return true;
    if (activation.buttonStableId == 0 ||
        activation.commandKind == game::CommandButtonKind::Unknown ||
        activation.commandKind == game::CommandButtonKind::None ||
        activation.commandKind >
            game::CommandButtonKind::SelectAllUnitsOfType ||
        activation.postAccept >
            CommandPostAcceptAction::MarkSingleUseCommandUsed) {
        return false;
    }
    return activation.postAccept == CommandPostAcceptAction::None
        ? !activation.postAcceptActor
        : static_cast<bool>(activation.postAcceptActor);
}

[[nodiscard]] bool canonicalActors(container::Vector<ObjectId>& actors) {
    if (actors.size() > CommandCodec::MaximumActors) return false;
    std::sort(actors.begin(), actors.end());
    return std::none_of(actors.begin(), actors.end(), [](ObjectId actor) { return !actor; }) &&
        std::adjacent_find(actors.begin(), actors.end()) == actors.end();
}

[[nodiscard]] bool hasNoPositionPayload(const CommandPosition& position) noexcept {
    return !position.valid && position.x.raw() == 0 &&
        position.y.raw() == 0 && position.z.raw() == 0;
}

[[nodiscard]] bool validPlacementPayload(const GameCommand& command) noexcept {
    const bool canonicalAbsentEnd =
        hasNoPositionPayload(command.placementEndPosition);
    if (command.type == GameCommandType::Build) {
        return command.placementEndPosition.valid || canonicalAbsentEnd;
    }
    if (command.type == GameCommandType::SpecialPower) {
        return canonicalAbsentEnd;
    }
    return command.placementYawRadians.raw() == 0 && canonicalAbsentEnd;
}

[[nodiscard]] bool validForceAttackPayload(
    const GameCommand& command) noexcept {
    return !command.forceAttack || command.type == GameCommandType::Attack;
}

// Production commands are a closed, dedicated wire contract.  In particular,
// do not let a stale Move/Attack payload hitch a ride on a factory transaction:
// it would make replay/network intent ambiguous before GameSession validates
// the producer and product.
[[nodiscard]] bool validSpecializedPayload(const GameCommand& command,
                                           const container::Vector<ObjectId>& actors) noexcept {
    switch (command.type) {
    case GameCommandType::QueueProduction:
        return actors.size() == 1 && !command.commandName.empty() && !command.productionId &&
            !command.targetObject && !command.queued && hasNoPositionPayload(command.targetPosition);
    case GameCommandType::CancelProduction:
        return actors.size() == 1 && command.commandName.empty() && command.productionId != 0 &&
            !command.targetObject && !command.queued && hasNoPositionPayload(command.targetPosition);
    case GameCommandType::QueuePlayerUpgrade:
    case GameCommandType::CancelPlayerUpgrade:
        return actors.size() == 1 && !command.commandName.empty() && !command.productionId &&
            !command.targetObject && !command.queued && hasNoPositionPayload(command.targetPosition);
    case GameCommandType::SetFactoryRallyPoint:
        return actors.size() == 1 && command.commandName.empty() && !command.productionId &&
            !command.targetObject && !command.queued && command.targetPosition.valid;
    case GameCommandType::SetBeaconText:
        return actors.size() == 1u && !command.productionId && !command.targetObject &&
            !command.queued && hasNoPositionPayload(command.targetPosition);
    case GameCommandType::Repair:
        return !actors.empty() && command.targetObject && command.commandName.empty() &&
            !command.productionId && !command.queued &&
            hasNoPositionPayload(command.targetPosition);
    case GameCommandType::Sell:
        return !actors.empty() && command.commandName.empty() &&
            !command.productionId && !command.targetObject &&
            !command.queued && hasNoPositionPayload(command.targetPosition);
    case GameCommandType::CancelConstruction:
        return actors.size() == 1 && command.commandName.empty() &&
            !command.productionId && !command.targetObject &&
            !command.queued && hasNoPositionPayload(command.targetPosition);
    case GameCommandType::CancelOrderWaypoint:
        return actors.size() == 1 && command.commandName.empty() &&
            command.productionId != 0 && !command.targetObject &&
            !command.queued && hasNoPositionPayload(command.targetPosition);
    case GameCommandType::ExitContainer:
        return actors.size() == 1 && command.targetObject &&
            command.targetObject != actors.front() &&
            command.commandName.empty() && !command.productionId &&
            !command.queued && hasNoPositionPayload(command.targetPosition);
    case GameCommandType::EnterContainer:
        return !actors.empty() && command.targetObject &&
            command.commandName.empty() && !command.productionId &&
            !command.queued && hasNoPositionPayload(command.targetPosition);
    case GameCommandType::CombatDrop:
        return !actors.empty() && !command.commandName.empty() &&
            !command.productionId && !command.queued &&
            command.targetPosition.valid;
    case GameCommandType::PurchaseScience:
        return actors.empty() && !command.commandName.empty() &&
            !command.productionId && !command.targetObject &&
            !command.queued && hasNoPositionPayload(command.targetPosition);
    case GameCommandType::Scatter:
        return !actors.empty() && command.commandName.empty() &&
            !command.productionId && !command.targetObject &&
            !command.queued && hasNoPositionPayload(command.targetPosition);
    case GameCommandType::CreateFormation:
        return !actors.empty() && command.commandName.empty() &&
            !command.productionId && !command.targetObject &&
            !command.queued && !command.forceAttack &&
            hasNoPositionPayload(command.targetPosition);
    case GameCommandType::Guard:
    case GameCommandType::GuardWithoutPursuit:
    case GameCommandType::GuardFlyingUnitsOnly:
        return !actors.empty() && command.commandName.empty() &&
            !command.productionId &&
            (static_cast<bool>(command.targetObject) !=
             command.targetPosition.valid);
    case GameCommandType::ToggleOvercharge:
        return !actors.empty() && command.commandName.empty() &&
            !command.productionId && !command.targetObject &&
            !command.queued && hasNoPositionPayload(command.targetPosition);
    case GameCommandType::SpecialPower: {
        const game::CommandButtonKind kind =
            command.activation.commandKind;
        const bool construct =
            kind == game::CommandButtonKind::SpecialPowerConstruct ||
            kind == game::CommandButtonKind::
                SpecialPowerConstructFromShortcut;
        if (!construct) return true;
        return actors.size() == 1u && !command.commandName.empty() &&
            command.activation.present() &&
            command.activation.buttonStableId != 0 &&
            !command.productionId && !command.targetObject &&
            !command.queued && command.targetPosition.valid &&
            hasNoPositionPayload(command.placementEndPosition);
    }
    case GameCommandType::Evacuate:
    case GameCommandType::ExecuteRailedTransport:
        return !actors.empty() && command.commandName.empty() &&
            !command.productionId && !command.targetObject &&
            !command.queued && hasNoPositionPayload(command.targetPosition);
    case GameCommandType::CommandButton: {
        const game::CommandButtonKind kind =
            command.activation.commandKind;
        const bool intentionalContact =
            kind == game::CommandButtonKind::HijackVehicle ||
            kind == game::CommandButtonKind::ConvertToCarBomb ||
            kind == game::CommandButtonKind::SabotageBuilding;
        if (!intentionalContact) return true;
        return !actors.empty() && !command.commandName.empty() &&
            command.activation.present() &&
            command.activation.buttonStableId != 0 &&
            !command.productionId && command.targetObject &&
            hasNoPositionPayload(command.targetPosition) &&
            command.placementYawRadians.raw() == 0 &&
            hasNoPositionPayload(command.placementEndPosition);
    }
    case GameCommandType::None:
    case GameCommandType::UIAction:
    case GameCommandType::Move:
    case GameCommandType::AttackMove:
    case GameCommandType::Attack:
    case GameCommandType::Build:
    case GameCommandType::Pause:
    case GameCommandType::Surrender:
    case GameCommandType::Stop:
        return true;
    }
    return false;
}

} // namespace

container::Vector<uint8_t> CommandCodec::encode(const GameCommand& command) {
    if (!command.player.isMapPlayer() || !validPlacementPayload(command) ||
        !validForceAttackPayload(command) ||
        command.commandName.size() > MaximumCommandNameBytes ||
        static_cast<uint8_t>(command.source) > static_cast<uint8_t>(CommandSource::Replay) ||
        static_cast<uint8_t>(command.type) >
            static_cast<uint8_t>(GameCommandType::CancelOrderWaypoint) ||
        !validActivation(command)) {
        return {};
    }
    container::Vector<ObjectId> actors = command.actors;
    if (!canonicalActors(actors)) return {};
    if (!validSpecializedPayload(command, actors)) return {};

    container::Vector<uint8_t> out;
    out.reserve(80 + actors.size() * sizeof(uint32_t) + command.commandName.size());
    writeU16(out, Version);
    writeU32(out, command.tick);
    writeU32(out, command.sequence);
    writeU8(out, command.player.value);
    writeU8(out, static_cast<uint8_t>(command.source));
    writeU8(out, static_cast<uint8_t>(command.type));
    writeU16(out, static_cast<uint16_t>(actors.size()));
    for (const ObjectId actor : actors) writeU32(out, actor.value);
    writeU32(out, command.targetObject.value);
    writeU32(out, command.productionId);
    writeU64(out, command.activation.requestSequence);
    writeU64(out, command.activation.buttonStableId);
    writeU8(out, static_cast<uint8_t>(command.activation.commandKind));
    writeU8(out, static_cast<uint8_t>(command.activation.postAccept));
    writeU32(out, command.activation.postAcceptActor.value);
    const uint8_t flags = (command.targetPosition.valid ? 0x01u : 0u) |
                          (command.queued ? 0x02u : 0u) |
                          (command.placementEndPosition.valid ? 0x04u : 0u) |
                          (command.forceAttack ? 0x08u : 0u);
    writeU8(out, flags);
    writeFixed(out, command.targetPosition.x);
    writeFixed(out, command.targetPosition.y);
    writeFixed(out, command.targetPosition.z);
    writeFixed(out, command.placementYawRadians);
    writeFixed(out, command.placementEndPosition.x);
    writeFixed(out, command.placementEndPosition.y);
    writeFixed(out, command.placementEndPosition.z);
    writeU16(out, static_cast<uint16_t>(command.commandName.size()));
    out.insert(out.end(), command.commandName.begin(), command.commandName.end());
    return out;
}

CommandCodecResult CommandCodec::decode(const uint8_t* data, size_t size) {
    CommandCodecResult result;
    if (!data || size == 0) {
        result.error = "empty command";
        return result;
    }
    Reader reader(data, size);
    uint16_t version = 0;
    uint8_t player = 0;
    uint8_t source = 0;
    uint8_t type = 0;
    uint16_t actorCount = 0;
    uint8_t flags = 0;
    if (!reader.readU16(version)) {
        result.error = "missing command version";
        return result;
    }
    if (version != Version) {
        result.error = "unsupported command version";
        return result;
    }

    GameCommand command;
    if (!reader.readU32(command.tick) || !reader.readU32(command.sequence) ||
        !reader.readU8(player) || !reader.readU8(source) || !reader.readU8(type) ||
        !reader.readU16(actorCount) || actorCount > MaximumActors) {
        result.error = "truncated or oversized command header";
        return result;
    }
    command.actors.reserve(actorCount);
    ObjectId previous = INVALID_OBJECT_ID;
    for (uint16_t index = 0; index < actorCount; ++index) {
        ObjectId actor;
        if (!reader.readU32(actor.value) || !actor || (index != 0 && !(previous < actor))) {
            result.error = "command actors are not canonical unique ObjectIds";
            return result;
        }
        command.actors.push_back(actor);
        previous = actor;
    }
    uint8_t activationKind = 0;
    uint8_t postAccept = 0;
    if (!reader.readU32(command.targetObject.value) || !reader.readU32(command.productionId) ||
        !reader.readU64(command.activation.requestSequence) ||
        !reader.readU64(command.activation.buttonStableId) ||
        !reader.readU8(activationKind) || !reader.readU8(postAccept) ||
        !reader.readU32(command.activation.postAcceptActor.value) ||
        !reader.readU8(flags) || (flags & ~0x0fu) != 0 ||
        !reader.readFixed(command.targetPosition.x) ||
        !reader.readFixed(command.targetPosition.y) ||
        !reader.readFixed(command.targetPosition.z) ||
        !reader.readFixed(command.placementYawRadians) ||
        !reader.readFixed(command.placementEndPosition.x) ||
        !reader.readFixed(command.placementEndPosition.y) ||
        !reader.readFixed(command.placementEndPosition.z) ||
        !reader.readShortString(command.commandName)) {
        result.error = "truncated or malformed command body";
        return result;
    }
    command.player = PlayerId{player};
    if (!command.player.isMapPlayer() || !validSource(source) ||
        !validType(type) || !reader.atEnd()) {
        result.error = "invalid command enum, player or trailing bytes";
        return result;
    }
    command.source = static_cast<CommandSource>(source);
    command.type = static_cast<GameCommandType>(type);
    command.activation.commandKind =
        static_cast<game::CommandButtonKind>(activationKind);
    command.activation.postAccept =
        static_cast<CommandPostAcceptAction>(postAccept);
    command.targetPosition.valid = (flags & 0x01u) != 0;
    command.queued = (flags & 0x02u) != 0;
    command.placementEndPosition.valid = (flags & 0x04u) != 0;
    command.forceAttack = (flags & 0x08u) != 0;
    if (!validActivation(command)) {
        result.error = "invalid command activation context";
        return result;
    }
    if (!validPlacementPayload(command)) {
        result.error = "invalid build-placement command payload";
        return result;
    }
    if (!validForceAttackPayload(command)) {
        result.error = "invalid force-attack command payload";
        return result;
    }
    if (!validSpecializedPayload(command, command.actors)) {
        result.error = "invalid command-family payload";
        return result;
    }
    result.ok = true;
    result.command = std::move(command);
    result.bytesRead = reader.offset();
    return result;
}

} // namespace engine

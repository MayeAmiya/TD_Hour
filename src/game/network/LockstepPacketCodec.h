#pragma once

#include "core/container/container_types.h"

#include "game/base/GameSettings.h"
#include "game/command/LockstepFrameBuffer.h"

#include <compare>
#include <cstddef>
#include <cstdint>
namespace engine {

enum class LockstepPacketType : uint8_t {
    ClientHello = 1,
    ServerHello = 2,
    CommandBatch = 3,
    ConfirmedFrame = 4,
    SyncSample = 5,
    SyncMismatch = 6,
};

struct LockstepServerHello {
    bool accepted = false;
    uint16_t protocolVersion = 1;
    GameTick frameSendRate = 0;
    container::String error;
};

// Produced after GameSession has frozen all loaded simulation content and
// resolved the roster. The first value covers the aggregate loaded content,
// not merely PlayerTemplate/Multiplayer.ini, so peers cannot join with a
// different Weapon/Thing/Command ruleset.
struct LockstepMatchIdentity final {
    uint64_t simulationContentFingerprint = 0;
    uint64_t resolvedSetupSimulationDigest = 0;

    [[nodiscard]] bool isValid() const noexcept {
        return simulationContentFingerprint != 0 && resolvedSetupSimulationDigest != 0;
    }
    constexpr auto operator<=>(const LockstepMatchIdentity&) const noexcept = default;
};

struct LockstepClientHello {
    container::String sessionId;
    container::String joinToken;
    container::String mapName;
    uint32_t mapCRC = 0;
    uint32_t mapSize = 0;
    // UI/launch metadata only. `matchIdentity` is authoritative for
    // simulation compatibility.
    uint32_t rulesCRC = 0;
    LockstepMatchIdentity matchIdentity;
    int seed = 0;
    uint16_t protocolVersion = 1;
    GameTick frameSendRate = 0;
    uint8_t localPlayerSlot = 0;
};

struct LockstepSyncSample final {
    GameTick tick = 0;
    uint32_t commandChecksum = 0;
    uint32_t combinedChecksum = 0;
    uint64_t aiRuntime = 0;
    uint64_t navigation = 0;
    uint64_t movement = 0;
    uint64_t economy = 0;
    uint64_t players = 0;
    uint64_t worldCombined = 0;

    constexpr bool operator==(const LockstepSyncSample&) const noexcept =
        default;
};

enum LockstepSyncMismatchBit : uint32_t {
    LockstepSyncMismatchCommand = uint32_t{1} << 0,
    LockstepSyncMismatchCombined = uint32_t{1} << 1,
    LockstepSyncMismatchAIRuntime = uint32_t{1} << 2,
    LockstepSyncMismatchNavigation = uint32_t{1} << 3,
    LockstepSyncMismatchMovement = uint32_t{1} << 4,
    LockstepSyncMismatchEconomy = uint32_t{1} << 5,
    LockstepSyncMismatchPlayers = uint32_t{1} << 6,
    LockstepSyncMismatchWorld = uint32_t{1} << 7,
};

struct LockstepSyncMismatch final {
    GameTick tick = 0;
    uint8_t referenceSlot = 0;
    uint8_t divergentSlot = 0;
    uint32_t mismatchMask = 0;
    LockstepSyncSample reference;
    LockstepSyncSample divergent;
};

[[nodiscard]] constexpr uint32_t lockstepSyncMismatchMask(
    const LockstepSyncSample& reference,
    const LockstepSyncSample& divergent) noexcept {
    uint32_t result = 0;
    if (reference.commandChecksum != divergent.commandChecksum)
        result |= LockstepSyncMismatchCommand;
    if (reference.combinedChecksum != divergent.combinedChecksum)
        result |= LockstepSyncMismatchCombined;
    if (reference.aiRuntime != divergent.aiRuntime)
        result |= LockstepSyncMismatchAIRuntime;
    if (reference.navigation != divergent.navigation)
        result |= LockstepSyncMismatchNavigation;
    if (reference.movement != divergent.movement)
        result |= LockstepSyncMismatchMovement;
    if (reference.economy != divergent.economy)
        result |= LockstepSyncMismatchEconomy;
    if (reference.players != divergent.players)
        result |= LockstepSyncMismatchPlayers;
    if (reference.worldCombined != divergent.worldCombined)
        result |= LockstepSyncMismatchWorld;
    return result;
}

struct LockstepPacketResult {
    bool ok = false;
    container::String error;
};

class LockstepPacketCodec {
public:
    // Version 14 carries CommandCodec v10 pointer-free ControlBar activation
    // metadata. Version 15 carries the actorless CommandCodec v15
    // PurchaseScience transaction. Version 16 carries CommandCodec v16
    // canonical Q32.32 command coordinates. Version 13 carried the actor-group Repair
    // payload; Version 11 carried CommandCodec v7 and Version 10 carried Build
    // yaw and the optional authoritative LINEBUILD end anchor.
    // Old protocol peers are rejected during packet-header negotiation rather
    // than failing later while parsing an inner command.
    static constexpr uint16_t Version = 16;

    static container::Vector<uint8_t> encodeClientHello(const GameStartInfo& info,
                                                  LockstepMatchIdentity matchIdentity);
    static container::Vector<uint8_t> encodeServerHello(const LockstepServerHello& hello);
    static container::Vector<uint8_t> encodeCommandBatch(const container::Vector<LocalCommandFrame>& frames);
    static container::Vector<uint8_t> encodeConfirmedFrame(const ConfirmedCommandFrame& frame);
    static container::Vector<uint8_t> encodeSyncSample(const LockstepSyncSample& sample);
    static container::Vector<uint8_t> encodeSyncMismatch(const LockstepSyncMismatch& mismatch);

    static LockstepPacketResult decodeClientHello(const uint8_t* data, size_t size,
                                                   LockstepClientHello& hello);
    static LockstepPacketResult decodeServerHello(const uint8_t* data, size_t size,
                                                   LockstepServerHello& hello);
    static LockstepPacketResult decodeCommandBatch(const uint8_t* data, size_t size,
                                                    container::Vector<LocalCommandFrame>& frames);
    static LockstepPacketResult decodeConfirmedFrame(const uint8_t* data, size_t size,
                                                      ConfirmedCommandFrame& frame);
    static LockstepPacketResult decodeSyncSample(const uint8_t* data, size_t size,
                                                 LockstepSyncSample& sample);
    static LockstepPacketResult decodeSyncMismatch(const uint8_t* data, size_t size,
                                                   LockstepSyncMismatch& mismatch);
};

} // namespace engine

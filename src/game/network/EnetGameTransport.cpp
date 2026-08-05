#include "core/container/container_types.h"
#include "EnetGameTransport.h"

#include "game/command/LockstepFrameBuffer.h"
#include "core/platform/net.h"
#include "debug/debug.h"

#include <enet/enet.h>

#include <iterator>
#include <utility>

namespace engine {

EnetGameTransport::EnetGameTransport() = default;

EnetGameTransport::~EnetGameTransport()
{
    shutdown();
}

bool EnetGameTransport::start(const GameStartInfo& info, LockstepMatchIdentity matchIdentity)
{
    shutdown();
    if (!info.network.enabled || info.network.serverHost.empty() || info.network.serverPort == 0) {
        fail("network session is missing a server address");
        return false;
    }
    if (info.network.frameSendRate == 0) {
        fail("network session frameSendRate must be greater than zero");
        return false;
    }
    if (!matchIdentity.isValid()) {
        fail("network session is missing canonical simulation identity");
        return false;
    }

    m_startInfo = info;
    m_matchIdentity = matchIdentity;
    m_netInit = std::make_unique<platform::net_init>();
    m_host = std::make_unique<platform::host>();
    if (!m_host->create_client(nullptr, 1, 1)) {
        fail("failed to create ENet client host");
        return false;
    }

    platform::address address;
    if (!address.set_host(info.network.serverHost.c_str())) {
        fail("failed to resolve game server host");
        return false;
    }
    address.addr.port = info.network.serverPort;
    m_server = std::make_unique<platform::peer>(m_host->connect(address, 1));
    if (!*m_server) {
        fail("failed to create ENet server connection");
        return false;
    }

    m_state = EnetGameTransportState::Connecting;
    TD_LOG_INFO("[EnetGameTransport] Connecting to {}:{} for session '{}'",
                info.network.serverHost, info.network.serverPort, info.network.sessionId);
    return true;
}

void EnetGameTransport::shutdown()
{
    if (m_server) {
        m_server->disconnect_now();
    }
    m_server.reset();
    m_host.reset();
    m_netInit.reset();
    m_startInfo.reset();
    m_matchIdentity = {};
    m_state = EnetGameTransportState::Disconnected;
    m_confirmedFrameSendRate.reset();
    m_queuedFrames.clear();
    m_error.clear();
}

void EnetGameTransport::update(LockstepFrameBuffer& frameBuffer)
{
    if (!m_host || m_state == EnetGameTransportState::Failed) {
        return;
    }

    ENetEvent event{};
    while (m_host->service(event, 0)) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            m_state = EnetGameTransportState::AwaitingHello;
            sendClientHello();
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            handlePacket(event.packet->data, event.packet->dataLength, frameBuffer);
            enet_packet_destroy(event.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            fail("disconnected from game server");
            break;
        default:
            break;
        }
    }

    if (isReady()) {
        flushQueuedFrames();
    }
}

void EnetGameTransport::queueFrames(container::Vector<LocalCommandFrame> frames)
{
    m_queuedFrames.insert(m_queuedFrames.end(),
                          std::make_move_iterator(frames.begin()),
                          std::make_move_iterator(frames.end()));
    if (isReady()) {
        flushQueuedFrames();
    }
}

void EnetGameTransport::sendSyncSample(const LockstepSyncSample& sample)
{
    if (!isReady() || !m_server) {
        return;
    }
    const auto encoded = LockstepPacketCodec::encodeSyncSample(sample);
    m_server->send(0, platform::packet(encoded));
    m_host->flush();
}

void EnetGameTransport::sendClientHello()
{
    if (!m_server) {
        fail("game server peer is unavailable");
        return;
    }
    const auto encoded = LockstepPacketCodec::encodeClientHello(m_startInfo, m_matchIdentity);
    if (encoded.empty()) {
        fail("failed to encode canonical client hello");
        return;
    }
    m_server->send(0, platform::packet(encoded));
    m_host->flush();
}

void EnetGameTransport::flushQueuedFrames()
{
    if (m_queuedFrames.empty() || !m_server) {
        return;
    }
    const auto encoded = LockstepPacketCodec::encodeCommandBatch(m_queuedFrames);
    if (encoded.empty()) {
        fail("could not encode a canonical local command batch");
        return;
    }
    m_server->send(0, platform::packet(encoded));
    m_host->flush();
    m_queuedFrames.clear();
}

void EnetGameTransport::handlePacket(const uint8_t* data, size_t size, LockstepFrameBuffer& frameBuffer)
{
    if (size == 0) {
        fail("received empty game server packet");
        return;
    }

    const auto type = static_cast<LockstepPacketType>(data[0]);
    if (type == LockstepPacketType::ServerHello) {
        LockstepServerHello hello;
        const auto decoded = LockstepPacketCodec::decodeServerHello(data, size, hello);
        if (!decoded.ok) {
            fail(decoded.error);
        } else if (!hello.accepted) {
            fail(hello.error.empty() ? "game server rejected session join" : hello.error);
        } else if (hello.protocolVersion != m_startInfo.network.protocolVersion) {
            fail("game server confirmed an incompatible network protocol version");
        } else if (hello.frameSendRate == 0) {
            fail("game server confirmed an invalid frameSendRate of 0");
        } else {
            m_confirmedFrameSendRate = hello.frameSendRate;
            m_state = EnetGameTransportState::Ready;
            TD_LOG_INFO("[EnetGameTransport] Session accepted with frameSendRate={}", hello.frameSendRate);
        }
        return;
    }

    if (type == LockstepPacketType::ConfirmedFrame) {
        ConfirmedCommandFrame frame;
        const auto decoded = LockstepPacketCodec::decodeConfirmedFrame(data, size, frame);
        if (!decoded.ok) {
            fail(decoded.error);
            return;
        }
        if (!frameBuffer.receiveConfirmedFrame(std::move(frame))) {
            fail("received malformed or conflicting confirmed command frame");
        }
        return;
    }

    if (type == LockstepPacketType::SyncMismatch) {
        LockstepSyncMismatch mismatch;
        const auto decoded = LockstepPacketCodec::decodeSyncMismatch(
            data, size, mismatch);
        if (!decoded.ok) {
            fail(decoded.error);
            return;
        }
        TD_LOG_ERROR(
            "[EnetGameTransport] Deterministic sync mismatch at tick {}: "
            "referenceSlot={} divergentSlot={} mask=0x{:x}; "
            "command={}/{} combined={}/{} ai={}/{} navigation={}/{} "
            "movement={}/{} economy={}/{} players={}/{} world={}/{}",
            mismatch.tick, mismatch.referenceSlot, mismatch.divergentSlot,
            mismatch.mismatchMask,
            mismatch.reference.commandChecksum,
            mismatch.divergent.commandChecksum,
            mismatch.reference.combinedChecksum,
            mismatch.divergent.combinedChecksum,
            mismatch.reference.aiRuntime, mismatch.divergent.aiRuntime,
            mismatch.reference.navigation, mismatch.divergent.navigation,
            mismatch.reference.movement, mismatch.divergent.movement,
            mismatch.reference.economy, mismatch.divergent.economy,
            mismatch.reference.players, mismatch.divergent.players,
            mismatch.reference.worldCombined,
            mismatch.divergent.worldCombined);
        fail("deterministic simulation sync mismatch");
        return;
    }

    fail("received unsupported game server packet");
}

void EnetGameTransport::fail(container::String error)
{
    m_error = std::move(error);
    m_state = EnetGameTransportState::Failed;
    TD_LOG_ERROR("[EnetGameTransport] {}", m_error);
}

} // namespace engine

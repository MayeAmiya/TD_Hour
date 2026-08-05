#pragma once

#include "core/container/container_types.h"

#include "LockstepPacketCodec.h"
#include "game/base/GameSettings.h"
#include <optional>
namespace platform {
class host;
class net_init;
class peer;
}

namespace engine {

class LockstepFrameBuffer;

enum class EnetGameTransportState : uint8_t {
    Disconnected,
    Connecting,
    AwaitingHello,
    Ready,
    Failed,
};

class EnetGameTransport {
public:
    EnetGameTransport();
    ~EnetGameTransport();

    EnetGameTransport(const EnetGameTransport&) = delete;
    EnetGameTransport& operator=(const EnetGameTransport&) = delete;

    bool start(const GameStartInfo& info, LockstepMatchIdentity matchIdentity);
    void shutdown();
    void update(LockstepFrameBuffer& frameBuffer);
    void queueFrames(container::Vector<LocalCommandFrame> frames);
    void sendSyncSample(const LockstepSyncSample& sample);

    EnetGameTransportState state() const { return m_state; }
    bool isReady() const { return m_state == EnetGameTransportState::Ready; }
    const container::String& error() const { return m_error; }
    std::optional<GameTick> confirmedFrameSendRate() const { return m_confirmedFrameSendRate; }

private:
    void sendClientHello();
    void flushQueuedFrames();
    void handlePacket(const uint8_t* data, size_t size, LockstepFrameBuffer& frameBuffer);
    void fail(container::String error);

    container::UniquePtr<platform::net_init> m_netInit;
    container::UniquePtr<platform::host> m_host;
    container::UniquePtr<platform::peer> m_server;
    GameStartInfo m_startInfo;
    LockstepMatchIdentity m_matchIdentity;
    EnetGameTransportState m_state = EnetGameTransportState::Disconnected;
    std::optional<GameTick> m_confirmedFrameSendRate;
    container::Vector<LocalCommandFrame> m_queuedFrames;
    container::String m_error;
};

} // namespace engine

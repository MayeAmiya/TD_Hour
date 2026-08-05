#include "engine/renderer/world/overlay/ClientOptionsPresentation.h"

namespace engine::render
{

const ClientOptionsRenderState& ClientOptionsPresentationConsumer::consume(const ClientOptionsRenderState& incoming,
                                                                           uint64_t simulationFrame) noexcept
{
    // Diagnostic/ad-hoc frames intentionally have no session authority. They
    // must use their own defaults instead of briefly inheriting a completed
    // map's durable policy, but they also must not erase that map's accepted
    // state before a legitimate session frame arrives.
    if (incoming.presentationEpoch == 0)
    {
        m_unscoped = incoming;
        return m_unscoped;
    }

    if (!m_initialized || incoming.presentationEpoch > m_accepted.presentationEpoch)
    {
        m_accepted = incoming;
        m_acceptedSimulationFrame = simulationFrame;
        m_initialized = true;
    }
    else if (incoming.presentationEpoch == m_accepted.presentationEpoch &&
             (incoming.presentationSequence > m_accepted.presentationSequence ||
              (incoming.presentationSequence == m_accepted.presentationSequence &&
               simulationFrame >= m_acceptedSimulationFrame)))
    {
        m_accepted = incoming;
        m_acceptedSimulationFrame = simulationFrame;
    }
    return m_accepted;
}

void ClientOptionsPresentationConsumer::reset() noexcept
{
    m_accepted = {};
    m_unscoped = {};
    m_acceptedSimulationFrame = 0;
    m_initialized = false;
}

} // namespace engine::render

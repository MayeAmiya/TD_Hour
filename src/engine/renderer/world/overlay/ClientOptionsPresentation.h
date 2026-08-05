#pragma once

#include <cstdint>

#include "presentation/render/RenderSceneSnapshot.h"

namespace engine::render
{

// Renderer-side owner for the durable OPTIONS_SET_* values carried by a
// sealed WorldRenderSnapshot. Logic can legitimately publish an older frame
// after a newer prepared frame was already consumed, so this class gives
// client-only option consumers the same epoch/sequence protection as the
// other durable presentation states.
class ClientOptionsPresentationConsumer final
{
public:
    [[nodiscard]] const ClientOptionsRenderState& consume(const ClientOptionsRenderState& incoming,
                                                          uint64_t simulationFrame) noexcept;

    [[nodiscard]] bool initialized() const noexcept
    {
        return m_initialized;
    }
    [[nodiscard]] const ClientOptionsRenderState& accepted() const noexcept
    {
        return m_accepted;
    }

    void reset() noexcept;

private:
    ClientOptionsRenderState m_accepted{};
    ClientOptionsRenderState m_unscoped{};
    uint64_t m_acceptedSimulationFrame = 0;
    bool m_initialized = false;
};

} // namespace engine::render

#pragma once

#include "core/container/container_types.h"
#include "presentation/audio/AudioContentLayer.h"

namespace engine::fx {
class FxListCatalog;
class ParticleSystemCatalog;
}

namespace engine {

struct GamePresentationContentSnapshot final {
    container::SharedPtr<
        const container::Vector<audio::AudioContentLayer>> audioLayers;
    container::SharedPtr<const fx::ParticleSystemCatalog> particleSystems;
    container::SharedPtr<const fx::FxListCatalog> fxLists;

    [[nodiscard]] container::Span<const audio::AudioContentLayer>
    audioContentLayers() const noexcept {
        return audioLayers
            ? container::Span<const audio::AudioContentLayer>{*audioLayers}
            : container::Span<const audio::AudioContentLayer>{};
    }
};

} // namespace engine

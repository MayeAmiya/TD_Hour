#include "core/container/container_types.h"
#include "Renderer.h"

namespace engine {

container::UniquePtr<Renderer> Renderer::s_instance = nullptr;

Renderer& Renderer::instance() {
    if (!s_instance) {
        s_instance = std::make_unique<StubRenderer>();
    }
    return *s_instance;
}

void Renderer::setInstance(container::UniquePtr<Renderer> renderer) {
    s_instance = std::move(renderer);
}

} // namespace engine

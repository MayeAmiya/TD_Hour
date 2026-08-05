#pragma once

#include "core/container/hash_containers.h"

#include <functional>
namespace engine {
class Renderer;
class TextureManager;
}

namespace gui {

class Widget;

using WidgetDrawCallback = std::function<bool(Widget&, engine::Renderer&, engine::TextureManager&)>;

class DrawCallbackRegistry {
public:
    static DrawCallbackRegistry& instance();

    void registerCallback(const container::String& name, WidgetDrawCallback callback);
    WidgetDrawCallback find(const container::String& name) const;
    void clear();

private:
    DrawCallbackRegistry() = default;

    container::HashMap<container::String, WidgetDrawCallback> m_callbacks;
};

} // namespace gui

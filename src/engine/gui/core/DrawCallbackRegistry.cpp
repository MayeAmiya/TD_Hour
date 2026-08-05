#include "core/container/container_types.h"
#include "DrawCallbackRegistry.h"

namespace gui {

DrawCallbackRegistry& DrawCallbackRegistry::instance() {
    static DrawCallbackRegistry s_instance;
    return s_instance;
}

void DrawCallbackRegistry::registerCallback(const container::String& name, WidgetDrawCallback callback) {
    if (!name.empty() && callback) {
        m_callbacks[name] = std::move(callback);
    }
}

WidgetDrawCallback DrawCallbackRegistry::find(const container::String& name) const {
    auto it = m_callbacks.find(name);
    return it != m_callbacks.end() ? it->second : nullptr;
}

void DrawCallbackRegistry::clear() {
    m_callbacks.clear();
}

} // namespace gui

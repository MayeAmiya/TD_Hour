#include "core/container/container_types.h"
#include "LayoutCallbackRegistry.h"

namespace gui {

LayoutCallbackRegistry& LayoutCallbackRegistry::instance() {
    static LayoutCallbackRegistry s_instance;
    return s_instance;
}

void LayoutCallbackRegistry::registerInit(const container::String& name, LayoutInitFunc func) {
    m_initFuncs[name] = std::move(func);
}

void LayoutCallbackRegistry::registerUpdate(const container::String& name, LayoutUpdateFunc func) {
    m_updateFuncs[name] = std::move(func);
}

void LayoutCallbackRegistry::registerShutdown(const container::String& name, LayoutShutdownFunc func) {
    m_shutdownFuncs[name] = std::move(func);
}

LayoutInitFunc LayoutCallbackRegistry::findInit(const container::String& name) const {
    auto it = m_initFuncs.find(name);
    return (it != m_initFuncs.end()) ? it->second : nullptr;
}

LayoutUpdateFunc LayoutCallbackRegistry::findUpdate(const container::String& name) const {
    auto it = m_updateFuncs.find(name);
    return (it != m_updateFuncs.end()) ? it->second : nullptr;
}

LayoutShutdownFunc LayoutCallbackRegistry::findShutdown(const container::String& name) const {
    auto it = m_shutdownFuncs.find(name);
    return (it != m_shutdownFuncs.end()) ? it->second : nullptr;
}

void LayoutCallbackRegistry::clear() {
    m_initFuncs.clear();
    m_updateFuncs.clear();
    m_shutdownFuncs.clear();
}

} // namespace gui

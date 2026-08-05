#pragma once

#include "core/container/hash_containers.h"

#include <functional>
namespace gui {

class ScreenGroup;

// Callback types matching original engine's WindowLayoutInitFunc/UpdateFunc/ShutdownFunc
using LayoutInitFunc = std::function<void(ScreenGroup*)>;
using LayoutUpdateFunc = std::function<void(ScreenGroup*)>;
using LayoutShutdownFunc = std::function<void(ScreenGroup*, bool immediate)>;

// LayoutCallbackRegistry: maps WND callback names (e.g. "MainMenuInit") to C++ functions.
// Replaces original engine's FunctionLexicon lookup tables.
class LayoutCallbackRegistry {
public:
    static LayoutCallbackRegistry& instance();

    // Register callbacks by name
    void registerInit(const container::String& name, LayoutInitFunc func);
    void registerUpdate(const container::String& name, LayoutUpdateFunc func);
    void registerShutdown(const container::String& name, LayoutShutdownFunc func);

    // Look up callbacks by name (returns nullptr if not found)
    LayoutInitFunc findInit(const container::String& name) const;
    LayoutUpdateFunc findUpdate(const container::String& name) const;
    LayoutShutdownFunc findShutdown(const container::String& name) const;

    // Clear all registrations
    void clear();

private:
    LayoutCallbackRegistry() = default;

    container::HashMap<container::String, LayoutInitFunc> m_initFuncs;
    container::HashMap<container::String, LayoutUpdateFunc> m_updateFuncs;
    container::HashMap<container::String, LayoutShutdownFunc> m_shutdownFuncs;
};

// Convenience macro for registering lifecycle callbacks
#define REGISTER_LAYOUT_INIT(name, func) \
    static bool _reg_init_##name = (LayoutCallbackRegistry::instance().registerInit(#name, func), true)

#define REGISTER_LAYOUT_UPDATE(name, func) \
    static bool _reg_update_##name = (LayoutCallbackRegistry::instance().registerUpdate(#name, func), true)

#define REGISTER_LAYOUT_SHUTDOWN(name, func) \
    static bool _reg_shutdown_##name = (LayoutCallbackRegistry::instance().registerShutdown(#name, func), true)

} // namespace gui

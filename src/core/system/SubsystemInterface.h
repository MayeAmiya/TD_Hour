#pragma once

#include "container/container_types.h"
// SubsystemInterface: base class for all game engine subsystems.
// Modeled after original C&C Generals SubsystemInterface.
// Every subsystem has a name, lifecycle (init/reset/update/shutdown),
// and is managed by SubsystemList.
class SubsystemInterface {
public:
    SubsystemInterface() = default;
    virtual ~SubsystemInterface() = default;

    // Non-copyable
    SubsystemInterface(const SubsystemInterface&) = delete;
    SubsystemInterface& operator=(const SubsystemInterface&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────

    // Allocate resources, register with other subsystems
    virtual void init() = 0;

    // Reset to empty state (ready for new game/data). Do NOT free+realloc resources.
    virtual void reset() {}

    // Per-frame update
    virtual void update() {}

    // Free all resources
    virtual void shutdown() {}

    // Called after all subsystems are inited (resolve cross-subsystem dependencies)
    virtual void postInit() {}

    // ── Name ──────────────────────────────────────────────────────────
    const container::String& getName() const { return m_name; }
    void setName(const container::String& name) { m_name = name; }

protected:
    container::String m_name;
};

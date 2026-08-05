#pragma once

#include "container/container_types.h"

#include "SubsystemInterface.h"
// SubsystemList: manages lifecycle of all subsystems.
// Call initSubsystem() in registration order, then postInitAll() after all are registered.
// Call updateAll() each frame, shutdownAll() on exit.
class SubsystemList {
public:
    SubsystemList();
    ~SubsystemList();

    // Non-copyable
    SubsystemList(const SubsystemList&) = delete;
    SubsystemList& operator=(const SubsystemList&) = delete;

    // ── Registration ──────────────────────────────────────────────────
    // Takes ownership of subsystem pointer. Calls init() immediately.
    void initSubsystem(SubsystemInterface* subsystem);

    // ── Lifecycle ─────────────────────────────────────────────────────
    void postInitAll();
    void updateAll();
    void resetAll();
    void shutdownAll();

    // ── Query ─────────────────────────────────────────────────────────
    int getCount() const { return static_cast<int>(m_subsystems.size()); }
    SubsystemInterface* getSubsystem(int index) const;
    SubsystemInterface* findByName(const container::String& name) const;

private:
    container::Vector<SubsystemInterface*> m_subsystems;
};

// Global subsystem list (created by GameEngine)
extern SubsystemList* TheSubsystemList;

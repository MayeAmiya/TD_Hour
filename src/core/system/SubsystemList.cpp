#include "container/container_types.h"
#include "SubsystemList.h"
#include "debug/debug.h"
#include <algorithm>

SubsystemList* TheSubsystemList = nullptr;

SubsystemList::SubsystemList() = default;

SubsystemList::~SubsystemList() {
    // Do NOT call shutdownAll() here — caller manages lifecycle explicitly.
    // Caller calls shutdownAll() then deletes subsystem objects individually.
    // If we called shutdownAll() here, it would double-shutdown already-deleted objects.
    m_subsystems.clear();
}

void SubsystemList::initSubsystem(SubsystemInterface* subsystem) {
    if (!subsystem) return;
    m_subsystems.push_back(subsystem);
    subsystem->init();
    TD_LOG_INFO("[Subsystem] {} initialized", subsystem->getName());
}

void SubsystemList::postInitAll() {
    for (auto* sys : m_subsystems) {
        sys->postInit();
    }
}

void SubsystemList::updateAll() {
    for (auto* sys : m_subsystems) {
        sys->update();
    }
}

void SubsystemList::resetAll() {
    // Reset in reverse order (last registered first)
    for (auto it = m_subsystems.rbegin(); it != m_subsystems.rend(); ++it) {
        (*it)->reset();
    }
}

void SubsystemList::shutdownAll() {
    // Shutdown in reverse order
    for (auto it = m_subsystems.rbegin(); it != m_subsystems.rend(); ++it) {
        (*it)->shutdown();
        TD_LOG_INFO("[Subsystem] {} shutdown", (*it)->getName());
    }
}

SubsystemInterface* SubsystemList::getSubsystem(int index) const {
    if (index < 0 || index >= static_cast<int>(m_subsystems.size())) return nullptr;
    return m_subsystems[index];
}

SubsystemInterface* SubsystemList::findByName(const container::String& name) const {
    for (auto* sys : m_subsystems) {
        if (sys->getName() == name) return sys;
    }
    return nullptr;
}

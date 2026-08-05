#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"

#include <algorithm>

namespace engine::script {

// Same-step local selection projection shared by script conditions and local
// presentation effects. It is detached from authoritative ECS selection.
class GameSessionScriptLocalPresentationState final {
public:
    explicit GameSessionScriptLocalPresentationState(
        container::Span<const ObjectId> selection = {})
        : m_selection(selection.begin(), selection.end()) {
        std::sort(m_selection.begin(), m_selection.end());
        m_selection.erase(
            std::unique(m_selection.begin(), m_selection.end()),
            m_selection.end());
    }

    [[nodiscard]] bool contains(ObjectId object) const noexcept {
        return std::binary_search(
            m_selection.begin(), m_selection.end(), object);
    }

    void replace(ObjectId object) { m_selection.assign(1, object); }

private:
    container::Vector<ObjectId> m_selection;
};

} // namespace engine::script

#include "core/container/container_types.h"
#include "ScriptObjectIndex.h"

#include <algorithm>

namespace engine::script {
namespace {

[[nodiscard]] bool nameLess(const ScriptNamedObjectRecord& entry, container::StringView name) noexcept {
    return entry.name < name;
}

} // namespace

bool ScriptObjectIndex::bindName(container::StringView name, ObjectId object) {
    if (name.empty() || !object) return false;

    const auto position = findName(name);
    if (position == m_namedObjects.end() || position->name != name) {
        container::Vector<ScriptNamedObjectRecord> candidate = m_namedObjects;
        const auto candidatePosition = std::lower_bound(candidate.begin(), candidate.end(), name, nameLess);
        candidate.insert(candidatePosition, ScriptNamedObjectRecord{
            .name = container::String{name}, .object = object, .state = ScriptNamedObjectState::Alive});
        commitNamedObjects(std::move(candidate));
        return true;
    }
    if (position->state == ScriptNamedObjectState::Alive && position->object != object) {
        return false;
    }

    if (position->state == ScriptNamedObjectState::Alive) return true;

    const size_t recordIndex = static_cast<size_t>(position - m_namedObjects.begin());
    container::Vector<ScriptNamedObjectRecord> candidate = m_namedObjects;
    candidate[recordIndex].object = object;
    candidate[recordIndex].state = ScriptNamedObjectState::Alive;
    commitNamedObjects(std::move(candidate));
    return true;
}

bool ScriptObjectIndex::transferName(container::StringView name, ObjectId replacement) {
    if (name.empty() || !replacement) return false;

    const auto position = findName(name);
    if (position == m_namedObjects.end() || position->name != name) {
        return bindName(name, replacement);
    }

    if (position->state == ScriptNamedObjectState::Alive && position->object == replacement) return true;

    const size_t recordIndex = static_cast<size_t>(position - m_namedObjects.begin());
    container::Vector<ScriptNamedObjectRecord> candidate = m_namedObjects;
    candidate[recordIndex].object = replacement;
    candidate[recordIndex].state = ScriptNamedObjectState::Alive;
    commitNamedObjects(std::move(candidate));
    return true;
}

size_t ScriptObjectIndex::transferObjectNames(ObjectId source,
                                              ObjectId replacement) {
    if (!source || !replacement || source == replacement) return 0;
    const auto found = m_objectToNames.find(source);
    if (found == m_objectToNames.end() || found->second.empty()) return 0;
    container::Vector<ScriptNamedObjectRecord> next = m_namedObjects;
    size_t transferred = 0;
    for (const size_t index : found->second) {
        if (index >= next.size()) continue;
        ScriptNamedObjectRecord& record = next[index];
        if (record.state != ScriptNamedObjectState::Alive ||
            record.object != source) continue;
        record.object = replacement;
        ++transferred;
    }
    if (transferred != 0) commitNamedObjects(std::move(next));
    return transferred;
}

void ScriptObjectIndex::notifyObjectDestroyed(ObjectId object) noexcept {
    if (!object) return;

    const auto found = m_objectToNames.find(object);
    if (found == m_objectToNames.end()) return;

    for (const size_t recordIndex : found->second) {
        // Both tables are committed together at every mutation boundary, but
        // retain this bound check as a defensive no-throw lifecycle guard.
        if (recordIndex >= m_namedObjects.size()) continue;
        ScriptNamedObjectRecord& record = m_namedObjects[recordIndex];
        if (record.state == ScriptNamedObjectState::Alive && record.object == object) {
            record.object = INVALID_OBJECT_ID;
            record.state = ScriptNamedObjectState::Destroyed;
        }
    }
    m_objectToNames.erase(found);
}

std::optional<ObjectId> ScriptObjectIndex::liveNamedObject(container::StringView name) const {
    const auto position = findName(name);
    if (position == m_namedObjects.end() || position->name != name ||
        position->state != ScriptNamedObjectState::Alive) {
        return std::nullopt;
    }
    return position->object;
}

ScriptNamedObjectState ScriptObjectIndex::namedObjectState(container::StringView name) const noexcept {
    const auto position = findName(name);
    return position == m_namedObjects.end() || position->name != name
        ? ScriptNamedObjectState::Unknown
        : position->state;
}

bool ScriptObjectIndex::didNamedObjectExist(container::StringView name) const noexcept {
    return namedObjectState(name) == ScriptNamedObjectState::Destroyed;
}

void ScriptObjectIndex::clear() noexcept {
    m_namedObjects.clear();
    m_objectToNames.clear();
}

container::Vector<ScriptNamedObjectRecord>::iterator ScriptObjectIndex::findName(container::StringView name) {
    return std::lower_bound(m_namedObjects.begin(), m_namedObjects.end(), name, nameLess);
}

container::Vector<ScriptNamedObjectRecord>::const_iterator ScriptObjectIndex::findName(
    container::StringView name) const {
    return std::lower_bound(m_namedObjects.begin(), m_namedObjects.end(), name, nameLess);
}

ScriptObjectIndex::ObjectToNames ScriptObjectIndex::buildObjectToNames(
    const container::Vector<ScriptNamedObjectRecord>& namedObjects) {
    ObjectToNames objectToNames;
    objectToNames.reserve(namedObjects.size());
    for (size_t index = 0; index < namedObjects.size(); ++index) {
        const ScriptNamedObjectRecord& record = namedObjects[index];
        if (record.state == ScriptNamedObjectState::Alive && record.object) {
            objectToNames[record.object].push_back(index);
        }
    }
    return objectToNames;
}

void ScriptObjectIndex::commitNamedObjects(container::Vector<ScriptNamedObjectRecord> namedObjects) {
    ObjectToNames objectToNames = buildObjectToNames(namedObjects);
    m_namedObjects.swap(namedObjects);
    m_objectToNames.swap(objectToNames);
}

} // namespace engine::script

#pragma once

#include "core/container/hash_containers.h"

#include "core/ecs/ObjectId.h"

#include <cstddef>
#include <cstdint>
#include <optional>
namespace engine::script {

// Script names are gameplay identifiers, not ECS entity handles.  In
// particular, a destroyed named object must stay observable as "previously
// existed" because original maps distinguish that state from a name that has
// never been assigned.  The index owns only value IDs and is therefore safe
// to query from the confirmed script tick.
enum class ScriptNamedObjectState : uint8_t {
    Unknown,
    Alive,
    Destroyed,
};

struct ScriptNamedObjectRecord final {
    container::String name;
    ObjectId object = INVALID_OBJECT_ID;
    ScriptNamedObjectState state = ScriptNamedObjectState::Unknown;
};

// Session-owned named-object bridge for ScriptRuntime. Team membership lives
// in ObjectTeamRegistry instead: a Team is an object's unique primary
// ownership grouping, not a second independent script-only collection.
class ScriptObjectIndex final {
public:
    // Binds a currently live object to a script name. Reusing a name after
    // the prior object has died is valid; assigning the same live name to a
    // different object is rejected so the caller cannot silently change a
    // map-script target.
    [[nodiscard]] bool bindName(container::StringView name, ObjectId object);
    // Explicit transfer is the supported replacement path (e.g. hijack /
    // morph). It preserves the logical name while replacing its live target.
    [[nodiscard]] bool transferName(container::StringView name, ObjectId replacement);
    // Rebuild/morph transactions transfer every live alias attached to one
    // stable object in one commit. This preserves campaigns that assign more
    // than one script name without exposing the reverse acceleration table.
    [[nodiscard]] size_t transferObjectNames(ObjectId source,
                                             ObjectId replacement);

    // Called by the authoritative lifecycle path when an object logically
    // leaves the live world. It retains name history as Destroyed rather than
    // erasing it; repeated physical-destruction notification is harmless.
    void notifyObjectDestroyed(ObjectId object) noexcept;

    [[nodiscard]] std::optional<ObjectId> liveNamedObject(container::StringView name) const;
    [[nodiscard]] ScriptNamedObjectState namedObjectState(container::StringView name) const noexcept;
    [[nodiscard]] bool didNamedObjectExist(container::StringView name) const noexcept;
    [[nodiscard]] container::Span<const ScriptNamedObjectRecord> namedObjects() const noexcept {
        return m_namedObjects;
    }

    void clear() noexcept;

private:
    [[nodiscard]] container::Vector<ScriptNamedObjectRecord>::iterator findName(container::StringView name);
    [[nodiscard]] container::Vector<ScriptNamedObjectRecord>::const_iterator findName(
        container::StringView name) const;
    // `m_namedObjects` remains a sorted vector so name lookup and its public
    // deterministic snapshot stay compact.  This reverse table is solely a
    // lifecycle acceleration structure: destruction starts from an ObjectId
    // and visits only its live aliases.
    using ObjectToNames = container::HashMap<ObjectId, container::Vector<size_t>>;

    [[nodiscard]] static ObjectToNames buildObjectToNames(
        const container::Vector<ScriptNamedObjectRecord>& namedObjects);
    // Binding and transfer are infrequent control-plane mutations.  Build a
    // complete reverse table before replacing either live table so a caught
    // allocation failure cannot leave a live name invisible to destruction.
    void commitNamedObjects(container::Vector<ScriptNamedObjectRecord> namedObjects);

    container::Vector<ScriptNamedObjectRecord> m_namedObjects;
    ObjectToNames m_objectToNames;
};

} // namespace engine::script

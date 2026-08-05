#pragma once

#include "core/container/container_types.h"
#include "game/base/MapContentIdentity.h"

namespace game {

// Immutable bytes selected from one VFS winner for a single map-start
// transaction. Terrain, legacy scripts and startup identity validation must
// consume this same handle rather than reopening the map independently.
class MapSourceBlob final {
public:
    [[nodiscard]] const MapContentIdentity& identity() const noexcept {
        return m_identity;
    }
    [[nodiscard]] container::Span<const uint8_t> bytes() const noexcept {
        return m_bytes;
    }

private:
    friend bool loadMapSourceBlob(container::StringView requestedPath,
                                  container::SharedPtr<const MapSourceBlob>& output,
                                  container::String* error);

    MapContentIdentity m_identity;
    container::Vector<uint8_t> m_bytes;
};

using MapSourceHandle = container::SharedPtr<const MapSourceBlob>;

// Resolves an exact official/user VFS path (or an unambiguous legacy
// basename), reads the selected candidate exactly once and computes its
// portable path/size/CRC identity exactly once. A readable candidate is final:
// format validation belongs to TerrainLogic and never falls through to a
// different map merely because the selected bytes are malformed.
[[nodiscard]] bool loadMapSourceBlob(container::StringView requestedPath,
                                     MapSourceHandle& output,
                                     container::String* error = nullptr);

} // namespace game

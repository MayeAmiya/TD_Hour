#include "ClientTerrainObjectPersistentStateCodec.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

#include "core/container/hash_containers.h"

namespace engine
{
namespace
{

void writeU8(container::Vector<uint8_t>& out, uint8_t value)
{
    out.push_back(value);
}

void writeU16(container::Vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8u));
}

void writeU32(container::Vector<uint8_t>& out, uint32_t value)
{
    for (uint32_t shift = 0; shift < 32; shift += 8)
    {
        out.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void writeU64(container::Vector<uint8_t>& out, uint64_t value)
{
    for (uint32_t shift = 0; shift < 64; shift += 8)
    {
        out.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void writeFloat(container::Vector<uint8_t>& out, float value)
{
    writeU32(out, std::bit_cast<uint32_t>(value));
}

class Reader final
{
public:
    Reader(const uint8_t* data, size_t size)
        : m_data(data)
        , m_size(size)
    {
    }

    bool readU8(uint8_t& value)
    {
        if (remaining() < 1)
            return false;
        value = m_data[m_offset++];
        return true;
    }
    bool readU16(uint16_t& value)
    {
        if (remaining() < 2)
            return false;
        value = static_cast<uint16_t>(m_data[m_offset]) | static_cast<uint16_t>(m_data[m_offset + 1]) << 8u;
        m_offset += 2;
        return true;
    }
    bool readU32(uint32_t& value)
    {
        if (remaining() < 4)
            return false;
        value = 0;
        for (uint32_t shift = 0; shift < 32; shift += 8)
        {
            value |= static_cast<uint32_t>(m_data[m_offset++]) << shift;
        }
        return true;
    }
    bool readU64(uint64_t& value)
    {
        if (remaining() < 8)
            return false;
        value = 0;
        for (uint32_t shift = 0; shift < 64; shift += 8)
        {
            value |= static_cast<uint64_t>(m_data[m_offset++]) << shift;
        }
        return true;
    }
    bool readFloat(float& value)
    {
        uint32_t bits = 0;
        if (!readU32(bits))
            return false;
        value = std::bit_cast<float>(bits);
        return true;
    }
    [[nodiscard]] size_t remaining() const noexcept
    {
        return m_size - m_offset;
    }

private:
    const uint8_t* m_data = nullptr;
    size_t m_size = 0;
    size_t m_offset = 0;
};

bool finite(const math::vec3& value) noexcept
{
    return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
}

bool validMutation(const ClientTerrainObjectMutation& mutation) noexcept
{
    const auto rawState = static_cast<uint8_t>(mutation.treeState);
    if (rawState > static_cast<uint8_t>(ClientTerrainTreeState::Removed) || !finite(mutation.toppleDirection) ||
        !std::isfinite(mutation.toppleRadians) || mutation.toppleRadians < 0.0f ||
        mutation.toppleRadians > math::PI * 0.5f || !std::isfinite(mutation.angularVelocity) ||
        !std::isfinite(mutation.angularAcceleration) || !std::isfinite(mutation.sinkOffset) ||
        mutation.sinkOffset < 0.0f || !std::isfinite(mutation.sinkElapsedFrames) || mutation.sinkElapsedFrames < 0.0f ||
        !finite(mutation.pushAsideDirection) || !std::isfinite(mutation.pushAsideAmount) ||
        mutation.pushAsideAmount < 0.0f || mutation.pushAsideAmount > 1.0f ||
        !std::isfinite(mutation.pushAsideDeltaPerFrame))
    {
        return false;
    }
    const bool pushed = mutation.pushAsideAmount != 0.0f || mutation.pushAsideDeltaPerFrame != 0.0f;
    const bool toppled =
        mutation.treeState == ClientTerrainTreeState::Falling || mutation.treeState == ClientTerrainTreeState::Down;
    const float toppleLength2 = mutation.toppleDirection.x() * mutation.toppleDirection.x() +
                                mutation.toppleDirection.y() * mutation.toppleDirection.y();
    const float pushLength2 = mutation.pushAsideDirection.x() * mutation.pushAsideDirection.x() +
                              mutation.pushAsideDirection.y() * mutation.pushAsideDirection.y();
    return !(mutation.treeState == ClientTerrainTreeState::Upright && !pushed) &&
           (!toppled || (std::isfinite(toppleLength2) && toppleLength2 > std::numeric_limits<float>::epsilon())) &&
           (!pushed || (mutation.treeState == ClientTerrainTreeState::Upright && std::isfinite(pushLength2) &&
                        pushLength2 > std::numeric_limits<float>::epsilon())) &&
           (!mutation.toppleFrozenByFog || mutation.treeState == ClientTerrainTreeState::Falling);
}

void writeVec3(container::Vector<uint8_t>& out, const math::vec3& value)
{
    writeFloat(out, value.x());
    writeFloat(out, value.y());
    writeFloat(out, value.z());
}

bool readVec3(Reader& reader, math::vec3& value)
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!reader.readFloat(x) || !reader.readFloat(y) || !reader.readFloat(z))
        return false;
    value = {x, y, z};
    return true;
}

} // namespace

container::Vector<uint8_t> ClientTerrainObjectPersistentStateCodec::encode(
    const ClientTerrainObjectPersistentState& state)
{
    if (state.version != ClientTerrainObjectPersistentState::kVersion || state.contentIdentity == 0 ||
        state.mutations.size() > kMaximumMutations)
    {
        return {};
    }
    container::HashSet<uint64_t> sources;
    sources.reserve(state.mutations.size());
    container::Vector<const ClientTerrainObjectMutation*> canonicalMutations;
    canonicalMutations.reserve(state.mutations.size());
    for (const ClientTerrainObjectMutation& mutation : state.mutations)
    {
        if (!validMutation(mutation) || !sources.insert(mutation.sourceRecordIndex).second)
        {
            return {};
        }
        canonicalMutations.push_back(&mutation);
    }
    std::sort(canonicalMutations.begin(),
              canonicalMutations.end(),
              [](const ClientTerrainObjectMutation* left, const ClientTerrainObjectMutation* right)
              { return left->sourceRecordIndex < right->sourceRecordIndex; });

    constexpr size_t kHeaderBytes = 24;
    constexpr size_t kMutationBytes = 64;
    container::Vector<uint8_t> output;
    output.reserve(kHeaderBytes + state.mutations.size() * kMutationBytes);
    writeU32(output, kMagic);
    writeU16(output, kCodecVersion);
    writeU16(output, 0);
    writeU32(output, state.version);
    writeU64(output, state.contentIdentity);
    writeU32(output, static_cast<uint32_t>(state.mutations.size()));
    for (const ClientTerrainObjectMutation* mutationPointer : canonicalMutations)
    {
        const ClientTerrainObjectMutation& mutation = *mutationPointer;
        writeU64(output, mutation.sourceRecordIndex);
        writeU8(output, static_cast<uint8_t>(mutation.treeState));
        writeU8(output, mutation.toppleFrozenByFog ? 1u : 0u);
        writeU16(output, 0);
        writeVec3(output, mutation.toppleDirection);
        writeFloat(output, mutation.toppleRadians);
        writeFloat(output, mutation.angularVelocity);
        writeFloat(output, mutation.angularAcceleration);
        writeFloat(output, mutation.sinkOffset);
        writeFloat(output, mutation.sinkElapsedFrames);
        writeVec3(output, mutation.pushAsideDirection);
        writeFloat(output, mutation.pushAsideAmount);
        writeFloat(output, mutation.pushAsideDeltaPerFrame);
    }
    return output;
}

ClientTerrainObjectPersistentStateDecodeResult ClientTerrainObjectPersistentStateCodec::decode(const uint8_t* data,
                                                                                               size_t size)
{
    ClientTerrainObjectPersistentStateDecodeResult result;
    if (!data || size == 0)
    {
        result.error = "empty client terrain checkpoint";
        return result;
    }
    Reader reader(data, size);
    ClientTerrainObjectPersistentState decodedState;
    uint32_t magic = 0;
    uint16_t codecVersion = 0;
    uint16_t reserved = 0;
    uint32_t mutationCount = 0;
    if (!reader.readU32(magic) || !reader.readU16(codecVersion) || !reader.readU16(reserved) ||
        !reader.readU32(decodedState.version) || !reader.readU64(decodedState.contentIdentity) ||
        !reader.readU32(mutationCount))
    {
        result.error = "truncated client terrain checkpoint header";
        return result;
    }
    if (magic != kMagic || codecVersion != kCodecVersion || reserved != 0 ||
        decodedState.version != ClientTerrainObjectPersistentState::kVersion)
    {
        result.error = "unsupported client terrain checkpoint format";
        return result;
    }
    constexpr size_t kMutationBytes = 64;
    if (decodedState.contentIdentity == 0 || mutationCount > kMaximumMutations ||
        mutationCount > reader.remaining() / kMutationBytes ||
        reader.remaining() != static_cast<size_t>(mutationCount) * kMutationBytes)
    {
        result.error = "invalid client terrain checkpoint length";
        return result;
    }

    container::HashSet<uint64_t> sources;
    sources.reserve(mutationCount);
    decodedState.mutations.reserve(mutationCount);
    for (uint32_t index = 0; index < mutationCount; ++index)
    {
        ClientTerrainObjectMutation mutation;
        uint8_t rawState = 0;
        uint8_t flags = 0;
        uint16_t mutationReserved = 0;
        if (!reader.readU64(mutation.sourceRecordIndex) || !reader.readU8(rawState) || !reader.readU8(flags) ||
            !reader.readU16(mutationReserved) || !readVec3(reader, mutation.toppleDirection) ||
            !reader.readFloat(mutation.toppleRadians) || !reader.readFloat(mutation.angularVelocity) ||
            !reader.readFloat(mutation.angularAcceleration) || !reader.readFloat(mutation.sinkOffset) ||
            !reader.readFloat(mutation.sinkElapsedFrames) || !readVec3(reader, mutation.pushAsideDirection) ||
            !reader.readFloat(mutation.pushAsideAmount) || !reader.readFloat(mutation.pushAsideDeltaPerFrame))
        {
            result.error = "truncated client terrain checkpoint mutation";
            return result;
        }
        mutation.treeState = static_cast<ClientTerrainTreeState>(rawState);
        mutation.toppleFrozenByFog = (flags & 1u) != 0;
        if ((flags & ~1u) != 0 || mutationReserved != 0 || !validMutation(mutation) ||
            !sources.insert(mutation.sourceRecordIndex).second)
        {
            result.error = "invalid client terrain checkpoint mutation";
            return result;
        }
        decodedState.mutations.push_back(mutation);
    }
    result.state = std::move(decodedState);
    result.ok = true;
    return result;
}

} // namespace engine

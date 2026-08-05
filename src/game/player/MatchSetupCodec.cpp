#include "core/container/container_types.h"
#include "MatchSetupCodec.h"

#include <algorithm>
#include <limits>
namespace engine {
namespace {

constexpr container::Array<uint8_t, 4> kMagic{{'G', 'T', 'M', 'S'}};
constexpr uint32_t kMaxEncodedStringBytes = 1024u * 1024u;

class Writer final {
public:
    void byte(uint8_t value) { m_output.push_back(value); }
    void u32(uint32_t value) {
        for (uint32_t shift = 0; shift < 32; shift += 8) byte(static_cast<uint8_t>((value >> shift) & 0xffu));
    }
    void u64(uint64_t value) {
        for (uint32_t shift = 0; shift < 64; shift += 8) byte(static_cast<uint8_t>((value >> shift) & 0xffu));
    }
    void i32(int32_t value) { u32(static_cast<uint32_t>(value)); }
    bool string(container::StringView value, container::String* error) {
        if (value.size() > kMaxEncodedStringBytes) {
            if (error) *error = "match setup string exceeds codec limit";
            return false;
        }
        u32(static_cast<uint32_t>(value.size()));
        m_output.insert(m_output.end(), value.begin(), value.end());
        return true;
    }
    [[nodiscard]] container::Vector<uint8_t> take() { return std::move(m_output); }

private:
    container::Vector<uint8_t> m_output;
};

class Reader final {
public:
    explicit Reader(container::Span<const uint8_t> input) : m_input(input) {}

    bool byte(uint8_t& value) {
        if (m_cursor == m_input.size()) return false;
        value = m_input[m_cursor++];
        return true;
    }
    bool u32(uint32_t& value) {
        if (m_input.size() - m_cursor < 4) return false;
        value = 0;
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            value |= static_cast<uint32_t>(m_input[m_cursor++]) << shift;
        }
        return true;
    }
    bool u64(uint64_t& value) {
        if (m_input.size() - m_cursor < 8) return false;
        value = 0;
        for (uint32_t shift = 0; shift < 64; shift += 8) {
            value |= static_cast<uint64_t>(m_input[m_cursor++]) << shift;
        }
        return true;
    }
    bool i32(int32_t& value) {
        uint32_t raw = 0;
        if (!u32(raw)) return false;
        value = static_cast<int32_t>(raw);
        return true;
    }
    bool string(container::String& value) {
        uint32_t length = 0;
        if (!u32(length) || length > kMaxEncodedStringBytes || length > m_input.size() - m_cursor) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(m_input.data() + m_cursor), length);
        m_cursor += length;
        return true;
    }
    [[nodiscard]] bool done() const noexcept { return m_cursor == m_input.size(); }

private:
    container::Span<const uint8_t> m_input;
    size_t m_cursor = 0;
};

void setError(container::String* error, container::String message) {
    if (error) *error = std::move(message);
}

} // namespace

bool MatchSetupCodec::encode(const ResolvedMatchSetup& setup, container::Vector<uint8_t>& output,
                             container::String* error) {
    if (error) error->clear();
    output.clear();
    const ResolvedMatchSetupValidation validation = validateResolvedMatchSetup(setup);
    if (!validation.ok()) {
        setError(error, validation.issues.front().message);
        return false;
    }

    container::Vector<const ResolvedPlayerSetup*> players;
    players.reserve(setup.players.size());
    for (const ResolvedPlayerSetup& player : setup.players) {
        players.push_back(&player);
    }
    std::sort(players.begin(), players.end(), [](const ResolvedPlayerSetup* lhs,
                                                 const ResolvedPlayerSetup* rhs) {
        return lhs->player < rhs->player;
    });
    Writer writer;
    for (const uint8_t value : kMagic) writer.byte(value);
    writer.u32(setup.schemaVersion);
    writer.byte(static_cast<uint8_t>(setup.mode));
    if (!writer.string(setup.mapName, error)) return false;
    writer.u32(setup.mapCrc);
    writer.u32(setup.mapSize);
    writer.i32(setup.difficulty);
    writer.i32(setup.rankPoints);
    writer.i32(setup.gameSpeedFps);
    writer.u32(setup.seed);
    writer.byte(setup.superweaponRestricted ? 1u : 0u);
    writer.byte(setup.oldFactionsOnly ? 1u : 0u);
    writer.u64(setup.playerRulesetFingerprint);
    writer.u64(setup.simulationContentFingerprint);
    writer.u32(static_cast<uint32_t>(players.size()));
    for (const ResolvedPlayerSetup* player : players) {
        writer.byte(player->player.value);
        writer.byte(player->slot.value);
        writer.byte(static_cast<uint8_t>(player->controller));
        writer.byte(static_cast<uint8_t>(player->aiDifficulty));
        writer.byte(static_cast<uint8_t>(player->participation));
        writer.u32(player->faction.value);
        writer.u32(player->color.value);
        writer.byte(player->alliance.value);
        if (!writer.string(player->displayName, error)) return false;
        writer.i32(player->startPosition);
        writer.i32(player->startingMoney);
    }
    output = writer.take();
    return true;
}

std::optional<ResolvedMatchSetup> MatchSetupCodec::decode(container::Span<const uint8_t> input,
                                                           container::String* error) {
    if (error) error->clear();
    Reader reader(input);
    for (const uint8_t expected : kMagic) {
        uint8_t actual = 0;
        if (!reader.byte(actual) || actual != expected) {
            setError(error, "invalid resolved match setup codec magic");
            return std::nullopt;
        }
    }

    ResolvedMatchSetup setup;
    uint8_t mode = 0;
    uint8_t superweaponRestricted = 0;
    uint8_t oldFactionsOnly = 0;
    uint32_t playerCount = 0;
    if (!reader.u32(setup.schemaVersion) ||
        setup.schemaVersion != ResolvedMatchSetup::kSchemaVersion ||
        !reader.byte(mode) || mode >= static_cast<uint8_t>(GameMode::Invalid) ||
        !reader.string(setup.mapName) || !reader.u32(setup.mapCrc) || !reader.u32(setup.mapSize) ||
        !reader.i32(setup.difficulty) || !reader.i32(setup.rankPoints) || !reader.i32(setup.gameSpeedFps) ||
        !reader.u32(setup.seed) || !reader.byte(superweaponRestricted) || superweaponRestricted > 1 ||
        !reader.byte(oldFactionsOnly) || oldFactionsOnly > 1 ||
        !reader.u64(setup.playerRulesetFingerprint) ||
        !reader.u64(setup.simulationContentFingerprint) ||
        !reader.u32(playerCount) || playerCount > PLAYER_SLOT_COUNT) {
        setError(error, "truncated or invalid resolved match setup payload");
        return std::nullopt;
    }
    setup.mode = static_cast<GameMode>(mode);
    setup.superweaponRestricted = superweaponRestricted != 0;
    setup.oldFactionsOnly = oldFactionsOnly != 0;

    setup.players.reserve(playerCount);
    for (uint32_t index = 0; index < playerCount; ++index) {
        ResolvedPlayerSetup player;
        uint8_t controller = 0;
        uint8_t aiDifficulty = 0;
        uint8_t participation = 0;
        if (!reader.byte(player.player.value) || !reader.byte(player.slot.value) ||
            !reader.byte(controller) || controller > static_cast<uint8_t>(PlayerControllerKind::Neutral) ||
            !reader.byte(aiDifficulty) || aiDifficulty > static_cast<uint8_t>(AiDifficulty::Hard) ||
            !reader.byte(participation) || participation > static_cast<uint8_t>(PlayerParticipationKind::Observer) ||
            !reader.u32(player.faction.value) || !reader.u32(player.color.value) ||
            !reader.byte(player.alliance.value) || !reader.string(player.displayName) ||
            !reader.i32(player.startPosition) || !reader.i32(player.startingMoney)) {
            setError(error, "truncated resolved match player record");
            return std::nullopt;
        }
        player.controller = static_cast<PlayerControllerKind>(controller);
        player.aiDifficulty = static_cast<AiDifficulty>(aiDifficulty);
        player.participation = static_cast<PlayerParticipationKind>(participation);
        setup.players.push_back(std::move(player));
    }
    if (!reader.done()) {
        setError(error, "resolved match setup contains trailing bytes");
        return std::nullopt;
    }
    const ResolvedMatchSetupValidation validation = validateResolvedMatchSetup(setup);
    if (!validation.ok()) {
        setError(error, validation.issues.front().message);
        return std::nullopt;
    }
    std::sort(setup.players.begin(), setup.players.end(), [](const ResolvedPlayerSetup& lhs,
                                                              const ResolvedPlayerSetup& rhs) {
        return lhs.player < rhs.player;
    });
    return setup;
}

} // namespace engine

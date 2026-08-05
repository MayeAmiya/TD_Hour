#include "CommandMapRuntime.h"

#include "core/container/string_utils.h"
#include "core/data/ini/LegacyIniDirectory.h"
#include "core/io/LocaleResourceLocator.h"
#include "core/io/VFS.h"
#include "debug/debug.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>

namespace app::input {
namespace {

[[nodiscard]] container::String trimCopy(container::StringView value) {
    return container::String(container::trimAsciiView(value));
}

[[nodiscard]] container::String upper(container::StringView value) {
    container::String result(value);
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
    return result;
}

[[nodiscard]] container::String withoutComment(container::String line) {
    const size_t comment = line.find(';');
    if (comment != container::String::npos) line.resize(comment);
    return trimCopy(line);
}

[[nodiscard]] uint8_t trailingOrdinal(container::StringView action) noexcept {
    if (action.empty() || action.back() < '0' || action.back() > '9') return 0;
    return static_cast<uint8_t>(action.back() - '0');
}

[[nodiscard]] CommandMapAction parseAction(
    container::StringView authored, uint8_t& ordinal) noexcept {
    const container::String action = upper(authored);
    ordinal = trailingOrdinal(action);
    if (action.starts_with("SAVE_VIEW")) return CommandMapAction::SaveView;
    if (action.starts_with("VIEW_VIEW")) return CommandMapAction::ViewSaved;
    if (action.starts_with("CREATE_TEAM")) return CommandMapAction::CreateTeam;
    if (action.starts_with("SELECT_TEAM")) return CommandMapAction::SelectTeam;
    if (action.starts_with("ADD_TEAM")) return CommandMapAction::AddTeam;
    if (action.starts_with("VIEW_TEAM")) return CommandMapAction::ViewTeam;
    struct Entry final { container::StringView name; CommandMapAction action; };
    static constexpr Entry entries[] = {
        {"SELECT_MATCHING_UNITS", CommandMapAction::SelectMatchingUnits},
        {"SELECT_NEXT_UNIT", CommandMapAction::SelectNextUnit},
        {"SELECT_PREV_UNIT", CommandMapAction::SelectPreviousUnit},
        {"SELECT_NEXT_WORKER", CommandMapAction::SelectNextWorker},
        {"SELECT_PREV_WORKER", CommandMapAction::SelectPreviousWorker},
        {"SELECT_HERO", CommandMapAction::SelectHero},
        {"VIEW_COMMAND_CENTER", CommandMapAction::ViewCommandCenter},
        {"VIEW_LAST_RADAR_EVENT", CommandMapAction::ViewLastRadarEvent},
        {"SELECT_ALL", CommandMapAction::SelectAll},
        {"SCATTER", CommandMapAction::Scatter},
        {"STOP", CommandMapAction::Stop},
        {"CREATE_FORMATION", CommandMapAction::CreateFormation},
        {"CHAT_ALLIES", CommandMapAction::ChatAllies},
        {"CHAT_EVERYONE", CommandMapAction::ChatEveryone},
        {"DIPLOMACY", CommandMapAction::Diplomacy},
        {"PLACE_BEACON", CommandMapAction::PlaceBeacon},
        {"DELETE_BEACON", CommandMapAction::DeleteBeacon},
        {"OPTIONS", CommandMapAction::Options},
        {"TOGGLE_CONTROL_BAR", CommandMapAction::ToggleControlBar},
        {"BEGIN_FORCEATTACK", CommandMapAction::BeginForceAttack},
        {"END_FORCEATTACK", CommandMapAction::EndForceAttack},
        {"BEGIN_FORCEMOVE", CommandMapAction::BeginForceMove},
        {"END_FORCEMOVE", CommandMapAction::EndForceMove},
        {"BEGIN_WAYPOINTS", CommandMapAction::BeginWaypoints},
        {"END_WAYPOINTS", CommandMapAction::EndWaypoints},
        {"BEGIN_PREFER_SELECTION", CommandMapAction::BeginPreferSelection},
        {"END_PREFER_SELECTION", CommandMapAction::EndPreferSelection},
        {"TAKE_SCREENSHOT", CommandMapAction::TakeScreenshot},
        {"BEGIN_CAMERA_ROTATE_LEFT", CommandMapAction::BeginCameraRotateLeft},
        {"END_CAMERA_ROTATE_LEFT", CommandMapAction::EndCameraRotateLeft},
        {"BEGIN_CAMERA_ROTATE_RIGHT", CommandMapAction::BeginCameraRotateRight},
        {"END_CAMERA_ROTATE_RIGHT", CommandMapAction::EndCameraRotateRight},
        {"BEGIN_CAMERA_ZOOM_IN", CommandMapAction::BeginCameraZoomIn},
        {"END_CAMERA_ZOOM_IN", CommandMapAction::EndCameraZoomIn},
        {"BEGIN_CAMERA_ZOOM_OUT", CommandMapAction::BeginCameraZoomOut},
        {"END_CAMERA_ZOOM_OUT", CommandMapAction::EndCameraZoomOut},
        {"CAMERA_RESET", CommandMapAction::CameraReset},
        {"TOGGLE_CAMERA_TRACKING_DRAWABLE", CommandMapAction::ToggleCameraTrackingDrawable},
    };
    for (const Entry& entry : entries) {
        if (action == entry.name) return entry.action;
    }
    return CommandMapAction::Unknown;
}

[[nodiscard]] SDL_Scancode parseScancode(container::StringView authored) {
    container::String key = upper(authored);
    if (key.starts_with("KEY_")) key.erase(0, 4);
    if (key.empty() || key == "NONE") return SDL_SCANCODE_UNKNOWN;
    if (key == "ESC") return SDL_SCANCODE_ESCAPE;
    if (key == "DEL") return SDL_SCANCODE_DELETE;
    if (key == "BACKSPACE") return SDL_SCANCODE_BACKSPACE;
    if (key == "ENTER") return SDL_SCANCODE_RETURN;
    if (key == "SPACE") return SDL_SCANCODE_SPACE;
    if (key == "TAB") return SDL_SCANCODE_TAB;
    if (key == "LEFT") return SDL_SCANCODE_LEFT;
    if (key == "RIGHT") return SDL_SCANCODE_RIGHT;
    if (key == "UP") return SDL_SCANCODE_UP;
    if (key == "DOWN") return SDL_SCANCODE_DOWN;
    if (key.size() >= 2u && key[0] == 'F') {
        uint32_t number = 0;
        const auto parsed = std::from_chars(
            key.data() + 1, key.data() + key.size(), number);
        if (parsed.ec == std::errc{} && number >= 1u && number <= 24u) {
            return static_cast<SDL_Scancode>(
                static_cast<int>(SDL_SCANCODE_F1) +
                static_cast<int>(number - 1u));
        }
    }
    if (key.size() == 3u && key.starts_with("KP") &&
        std::isdigit(static_cast<unsigned char>(key[2]))) {
        static constexpr SDL_Scancode keypad[] = {
            SDL_SCANCODE_KP_0, SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_2,
            SDL_SCANCODE_KP_3, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_5,
            SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_7, SDL_SCANCODE_KP_8,
            SDL_SCANCODE_KP_9};
        return keypad[key[2] - '0'];
    }
    if (key.size() == 1u) {
        if (key[0] >= 'A' && key[0] <= 'Z') {
            return static_cast<SDL_Scancode>(
                static_cast<int>(SDL_SCANCODE_A) + key[0] - 'A');
        }
        if (key[0] >= '1' && key[0] <= '9') {
            return static_cast<SDL_Scancode>(
                static_cast<int>(SDL_SCANCODE_1) + key[0] - '1');
        }
        if (key[0] == '0') return SDL_SCANCODE_0;
    }
    return SDL_SCANCODE_UNKNOWN;
}

[[nodiscard]] uint8_t parseModifiers(container::StringView authored) {
    const container::String value = upper(authored);
    uint8_t result = CommandMapModifierNone;
    if (value.find("SHIFT") != container::String::npos)
        result |= CommandMapModifierShift;
    if (value.find("CTRL") != container::String::npos ||
        value.find("CONTROL") != container::String::npos)
        result |= CommandMapModifierControl;
    if (value.find("ALT") != container::String::npos)
        result |= CommandMapModifierAlt;
    return result;
}

[[nodiscard]] uint8_t parseContexts(container::StringView authored) {
    const container::String value = upper(authored);
    uint8_t result = CommandMapContextNone;
    if (value.find("GAME") != container::String::npos)
        result |= CommandMapContextGame;
    return result;
}

[[nodiscard]] uint8_t eventModifiers(SDL_Keymod modifiers) noexcept {
    uint8_t result = CommandMapModifierNone;
    if ((modifiers & SDL_KMOD_SHIFT) != 0) result |= CommandMapModifierShift;
    if ((modifiers & SDL_KMOD_CTRL) != 0) result |= CommandMapModifierControl;
    if ((modifiers & SDL_KMOD_ALT) != 0) result |= CommandMapModifierAlt;
    return result;
}

[[nodiscard]] bool modifierScancodeMatches(
    SDL_Scancode scancode, uint8_t required) noexcept {
    if ((required & CommandMapModifierShift) != 0 &&
        (scancode == SDL_SCANCODE_LSHIFT ||
         scancode == SDL_SCANCODE_RSHIFT)) return true;
    if ((required & CommandMapModifierControl) != 0 &&
        (scancode == SDL_SCANCODE_LCTRL ||
         scancode == SDL_SCANCODE_RCTRL)) return true;
    if ((required & CommandMapModifierAlt) != 0 &&
        (scancode == SDL_SCANCODE_LALT ||
         scancode == SDL_SCANCODE_RALT)) return true;
    return false;
}

[[nodiscard]] CommandMapAction pairedEndAction(
    CommandMapAction action) noexcept {
    switch (action) {
    case CommandMapAction::BeginForceAttack:
        return CommandMapAction::EndForceAttack;
    case CommandMapAction::BeginForceMove:
        return CommandMapAction::EndForceMove;
    case CommandMapAction::BeginWaypoints:
        return CommandMapAction::EndWaypoints;
    case CommandMapAction::BeginPreferSelection:
        return CommandMapAction::EndPreferSelection;
    case CommandMapAction::BeginCameraRotateLeft:
        return CommandMapAction::EndCameraRotateLeft;
    case CommandMapAction::BeginCameraRotateRight:
        return CommandMapAction::EndCameraRotateRight;
    case CommandMapAction::BeginCameraZoomIn:
        return CommandMapAction::EndCameraZoomIn;
    case CommandMapAction::BeginCameraZoomOut:
        return CommandMapAction::EndCameraZoomOut;
    default:
        return CommandMapAction::Unknown;
    }
}

[[nodiscard]] std::optional<size_t> scancodeIndex(
    SDL_Scancode scancode) noexcept {
    const int value = static_cast<int>(scancode);
    if (value < 0 || value >= SDL_SCANCODE_COUNT) return std::nullopt;
    return static_cast<size_t>(value);
}

} // namespace

void CommandMapRuntime::reloadIfNeeded() {
    const uint64_t revision = io::VFS::instance().contentRevision();
    if (m_contentRevision == revision) return;
    resetActiveBindings();
    m_contentRevision = revision;
    m_bindings.clear();
    if (!load()) {
        TD_LOG_WARN("[CommandMap] No usable ZH CommandMap definition was found");
    }
}

void CommandMapRuntime::resetActiveBindings() noexcept {
    for (auto& binding : m_activeBindings) binding.reset();
}

bool CommandMapRuntime::load() {
    io::VFS& vfs = io::VFS::instance();
    container::Vector<container::String> paths;
    if (const auto locator = io::acquireLocaleResourceLocator()) {
        const container::String localizedRoot =
            "data/" + locator->localeDirectory() + "/commandmap";
        const container::Array<container::StringView, 2> roots{{
            localizedRoot,
            "data/ini/commandmap",
        }};
        paths = game::ini::enumerateLegacyIniDirectories(roots);
    } else {
        constexpr container::Array<container::StringView, 1> roots{{
            "data/ini/commandmap",
        }};
        paths = game::ini::enumerateLegacyIniDirectories(roots);
    }

    bool parsedDefinition = false;
    size_t filesRead = 0;
    for (const container::String& path : paths) {
        const container::String content = vfs.readAll(path);
        if (content.empty()) continue;
        ++filesRead;
        parsedDefinition = parse(content) || parsedDefinition;
    }

    // MetaMap records with MK_NONE can intentionally bind a modifier key, but
    // records with neither a physical key nor a modifier are inactive.  Keep
    // partial records while all roots are being merged, then expose only the
    // bindings that can actually match an event.
    std::erase_if(m_bindings, [](const CommandMapBinding& binding) {
        return binding.contexts == CommandMapContextNone ||
            (binding.key == SDL_SCANCODE_UNKNOWN &&
             binding.modifiers == CommandMapModifierNone);
    });

    if (!parsedDefinition || m_bindings.empty()) return false;
    TD_LOG_INFO(
        "[CommandMap] Loaded {} in-game bindings from {} INI files at VFS revision {}",
        m_bindings.size(), filesRead, m_contentRevision);
    return true;
}

bool CommandMapRuntime::parse(container::StringView content) {
    std::istringstream stream{container::String(content)};
    container::String line;
    CommandMapBinding current;
    bool inBinding = false;
    bool parsedDefinition = false;
    while (std::getline(stream, line)) {
        line = withoutComment(std::move(line));
        if (line.empty()) continue;
        const container::String upperLine = upper(line);
        if (upperLine.starts_with("COMMANDMAP ")) {
            current = {};
            const container::StringView actionName = container::trimAsciiView(
                container::StringView(line).substr(11u));
            current.action = parseAction(actionName, current.ordinal);
            if (current.action != CommandMapAction::Unknown) {
                const auto existing = std::find_if(
                    m_bindings.begin(), m_bindings.end(),
                    [&current](const CommandMapBinding& binding) {
                        return binding.action == current.action &&
                            binding.ordinal == current.ordinal;
                    });
                if (existing != m_bindings.end()) current = *existing;
            }
            inBinding = true;
            continue;
        }
        if (!inBinding) continue;
        if (upperLine == "END") {
            if (current.action != CommandMapAction::Unknown) {
                const auto existing = std::find_if(
                    m_bindings.begin(), m_bindings.end(),
                    [&current](const CommandMapBinding& binding) {
                        return binding.action == current.action &&
                            binding.ordinal == current.ordinal;
                    });
                if (existing == m_bindings.end()) {
                    m_bindings.push_back(current);
                } else {
                    *existing = current;
                }
                parsedDefinition = true;
            }
            inBinding = false;
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == container::String::npos) continue;
        const container::String field = upper(container::trimAsciiView(
            container::StringView(line).substr(0, equals)));
        const container::StringView value = container::trimAsciiView(
            container::StringView(line).substr(equals + 1u));
        if (field == "KEY") {
            current.key = parseScancode(value);
        } else if (field == "TRANSITION") {
            current.transition = upper(value) == "UP"
                ? CommandMapTransition::Up : CommandMapTransition::Down;
        } else if (field == "MODIFIERS") {
            current.modifiers = parseModifiers(value);
        } else if (field == "USEABLEIN") {
            current.contexts = parseContexts(value);
        }
    }
    return parsedDefinition;
}

std::optional<CommandMapBinding> CommandMapRuntime::match(
    SDL_Scancode scancode, bool keyDown, SDL_Keymod modifiers,
    bool inGame, bool repeat) noexcept {
    const std::optional<size_t> physicalKey = scancodeIndex(scancode);
    if (!keyDown && physicalKey) {
        std::optional<CommandMapBinding>& active =
            m_activeBindings[*physicalKey];
        if (active) {
            CommandMapBinding released = *active;
            active.reset();
            released.action = pairedEndAction(released.action);
            released.transition = CommandMapTransition::Up;
            return released;
        }
    }

    const uint8_t context = static_cast<uint8_t>(
        inGame ? CommandMapContextGame : CommandMapContextNone);
    const CommandMapTransition transition = keyDown
        ? CommandMapTransition::Down : CommandMapTransition::Up;
    uint8_t actualModifiers = eventModifiers(modifiers);
    for (const CommandMapBinding& binding : m_bindings) {
        if ((binding.contexts & context) == 0u ||
            binding.transition != transition ||
            (repeat && transition == CommandMapTransition::Down)) {
            continue;
        }
        if (binding.key == SDL_SCANCODE_UNKNOWN) {
            if (!modifierScancodeMatches(scancode, binding.modifiers)) continue;
            // SDL may clear a modifier before emitting its KEY_UP. Treat the
            // released physical key as still present for exact matching.
            if (!keyDown) actualModifiers |= binding.modifiers;
        } else if (binding.key != scancode) {
            continue;
        }
        const bool waypointModifiedOrdinaryKey =
            binding.key != SDL_SCANCODE_UNKNOWN &&
            binding.modifiers == CommandMapModifierNone &&
            actualModifiers == CommandMapModifierShift;
        if (actualModifiers != binding.modifiers &&
            !waypointModifiedOrdinaryKey) {
            continue;
        }

        if (keyDown && physicalKey) {
            const CommandMapAction endAction = pairedEndAction(binding.action);
            if (endAction != CommandMapAction::Unknown) {
                m_activeBindings[*physicalKey] = binding;
            }
        }
        return binding;
    }
    return std::nullopt;
}

} // namespace app::input

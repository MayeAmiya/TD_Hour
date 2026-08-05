#include "core/container/hash_containers.h"
#include "core/container/string_utils.h"
#include "ControlBarSchemeRuntime.h"

#include "VFS.h"
#include "core/data/ini/LegacyIniDirectory.h"
#include "debug/debug.h"
#include "presentation/render/PresentationDefaults.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>
#include <sstream>
namespace gui::ingame {
namespace {

constexpr auto trim = container::trimAsciiCopy;

container::String withoutComment(container::String line) {
    size_t comment = line.find(';');
    if (comment != container::String::npos) {
        line.resize(comment);
    }
    return trim(std::move(line));
}

container::String lower(container::String value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::optional<std::pair<int, int>> parseVector2(const container::String& line) {
    static const std::regex re(R"(X\s*:\s*(-?\d+)\s+Y\s*:\s*(-?\d+))", std::regex::icase);
    std::smatch match;
    if (!std::regex_search(line, match, re)) {
        return std::nullopt;
    }
    return std::make_pair(std::stoi(match[1].str()), std::stoi(match[2].str()));
}

std::optional<int> parseTrailingInt(const container::String& line) {
    static const std::regex re(R"((-?\d+)\s*$)");
    std::smatch match;
    if (!std::regex_search(line, match, re)) {
        return std::nullopt;
    }
    return std::stoi(match[1].str());
}

std::optional<uint32_t> parseNamedColor(const container::String& line) {
    static const std::regex re(R"(R\s*:\s*(\d+)\s+G\s*:\s*(\d+)\s+B\s*:\s*(\d+)\s+A\s*:\s*(\d+))", std::regex::icase);
    std::smatch match;
    if (!std::regex_search(line, match, re)) {
        return std::nullopt;
    }

    const auto clampByte = [](int value) {
        return std::clamp(value, 0, 255);
    };
    const uint32_t r = static_cast<uint32_t>(clampByte(std::stoi(match[1].str())));
    const uint32_t g = static_cast<uint32_t>(clampByte(std::stoi(match[2].str())));
    const uint32_t b = static_cast<uint32_t>(clampByte(std::stoi(match[3].str())));
    const uint32_t a = static_cast<uint32_t>(clampByte(std::stoi(match[4].str())));
    return (a << 24) | (r << 16) | (g << 8) | b;
}

container::String parseValueAfterKeyword(const container::String& line) {
    size_t space = line.find_first_of(" \t");
    if (space == container::String::npos) {
        return {};
    }
    return trim(line.substr(space + 1));
}

container::String parseValue(const container::String& line) {
    size_t eq = line.find('=');
    if (eq != container::String::npos) {
        return trim(line.substr(eq + 1));
    }
    return parseValueAfterKeyword(line);
}

container::String keywordLower(const container::String& line) {
    size_t end = line.find_first_of(" \t=");
    if (end == container::String::npos) {
        return lower(trim(line));
    }
    return lower(trim(line.substr(0, end)));
}

container::String normalizedSide(container::StringView side) {
    container::String value{side};
    value = lower(trim(std::move(value)));

    if (value.find("gla") != container::String::npos) return "GLA";
    if (value.find("china") != container::String::npos || value.find("boss") != container::String::npos) return "China";
    if (value.find("america") != container::String::npos || value == "usa" || value == "us") return "America";
    return {};
}

struct ParsedScheme {
    container::String name;
    container::String side;
    int screenW = engine::presentation_defaults::VIRTUAL_WIDTH;
    int screenH = engine::presentation_defaults::VIRTUAL_HEIGHT;
    container::String rightHudImage;
    container::String queueButtonImage;
    container::String commandMarkerImage;
    container::String expBarForegroundImage;
    container::String powerPurchaseImage;
    uint32_t commandBarBorderColor = COLOR_WHITE;
    uint32_t buttonBorderBuildColor = 0x00ffffffu;
    uint32_t buttonBorderActionColor = 0x00ffffffu;
    uint32_t buttonBorderUpgradeColor = 0x00ffffffu;
    uint32_t buttonBorderSystemColor = 0x00ffffffu;
    container::HashMap<container::String, container::String> namedImages;
    container::HashMap<container::String, std::pair<int, int>> namedPoints;
    container::Vector<ControlBarSchemeImagePart> imageParts;
};

container::Vector<ControlBarSchemeImagePart> scaledImageParts(const ParsedScheme& scheme) {
    container::Vector<ControlBarSchemeImagePart> parts = scheme.imageParts;
    const float scaleX = scheme.screenW > 0
        ? static_cast<float>(engine::presentation_defaults::VIRTUAL_WIDTH) / static_cast<float>(scheme.screenW)
        : 1.0f;
    const float scaleY = scheme.screenH > 0
        ? static_cast<float>(engine::presentation_defaults::VIRTUAL_HEIGHT) / static_cast<float>(scheme.screenH)
        : 1.0f;

    if (scaleX == 1.0f && scaleY == 1.0f) {
        return parts;
    }

    for (auto& part : parts) {
        part.x = static_cast<int>(part.x * scaleX);
        part.y = static_cast<int>(part.y * scaleY);
        part.w = static_cast<int>(part.w * scaleX);
        part.h = static_cast<int>(part.h * scaleY);
    }
    return parts;
}

} // namespace

ControlBarSchemeRuntime& ControlBarSchemeRuntime::instance() {
    static ControlBarSchemeRuntime s_instance;
    return s_instance;
}

const container::Vector<ControlBarSchemeImagePart>& ControlBarSchemeRuntime::imageParts() {
    ensureLoaded();
    return m_imageParts;
}

const container::String& ControlBarSchemeRuntime::rightHudImage() {
    ensureLoaded();
    return m_rightHudImage;
}

const container::String& ControlBarSchemeRuntime::queueButtonImage() {
    ensureLoaded();
    return m_queueButtonImage;
}

const container::String& ControlBarSchemeRuntime::commandMarkerImage() {
    ensureLoaded();
    return m_commandMarkerImage;
}

const container::String& ControlBarSchemeRuntime::expBarForegroundImage() {
    ensureLoaded();
    return m_expBarForegroundImage;
}

const container::String& ControlBarSchemeRuntime::powerPurchaseImage() {
    ensureLoaded();
    return m_powerPurchaseImage;
}

uint32_t ControlBarSchemeRuntime::commandBarBorderColor() {
    ensureLoaded();
    return m_commandBarBorderColor;
}

uint32_t ControlBarSchemeRuntime::buttonBorderBuildColor() {
    ensureLoaded();
    return m_buttonBorderBuildColor;
}

uint32_t ControlBarSchemeRuntime::buttonBorderActionColor() {
    ensureLoaded();
    return m_buttonBorderActionColor;
}

uint32_t ControlBarSchemeRuntime::buttonBorderUpgradeColor() {
    ensureLoaded();
    return m_buttonBorderUpgradeColor;
}

uint32_t ControlBarSchemeRuntime::buttonBorderSystemColor() {
    ensureLoaded();
    return m_buttonBorderSystemColor;
}

container::String ControlBarSchemeRuntime::namedImage(container::StringView key) {
    ensureLoaded();
    auto it = m_namedImages.find(lower(container::String{key}));
    return it != m_namedImages.end() ? it->second : container::String{};
}

std::optional<std::pair<int, int>> ControlBarSchemeRuntime::namedPoint(container::StringView key) {
    ensureLoaded();
    auto it = m_namedPoints.find(lower(container::String{key}));
    if (it == m_namedPoints.end()) return std::nullopt;
    return it->second;
}

void ControlBarSchemeRuntime::setPlayerSide(container::StringView side) {
    const container::String normalized = normalizedSide(side);
    if (m_playerSide == normalized) return;

    m_playerSide = normalized;
    if (m_loaded) {
        m_loaded = false;
        m_rightHudImage.clear();
        m_queueButtonImage.clear();
        m_commandMarkerImage.clear();
        m_expBarForegroundImage.clear();
        m_powerPurchaseImage.clear();
        m_commandBarBorderColor = COLOR_WHITE;
        m_buttonBorderBuildColor = 0x00ffffffu;
        m_buttonBorderActionColor = 0x00ffffffu;
        m_buttonBorderUpgradeColor = 0x00ffffffu;
        m_buttonBorderSystemColor = 0x00ffffffu;
        m_namedImages.clear();
        m_namedPoints.clear();
        m_imageParts.clear();
    }
}

void ControlBarSchemeRuntime::reset() {
    m_loaded = false;
    m_rightHudImage.clear();
    m_queueButtonImage.clear();
    m_commandMarkerImage.clear();
    m_expBarForegroundImage.clear();
    m_powerPurchaseImage.clear();
    m_commandBarBorderColor = COLOR_WHITE;
    m_buttonBorderBuildColor = 0x00ffffffu;
    m_buttonBorderActionColor = 0x00ffffffu;
    m_buttonBorderUpgradeColor = 0x00ffffffu;
    m_buttonBorderSystemColor = 0x00ffffffu;
    m_drawOffsetY = 0;
    m_powerMeter = {};
    m_namedImages.clear();
    m_namedPoints.clear();
    m_imageParts.clear();
}

void ControlBarSchemeRuntime::ensureLoaded() {
    if (m_loaded) return;
    m_loaded = true;

    if (!loadFromVfs()) {
        TD_LOG_WARN("[ControlBarScheme] No control bar scheme available");
    }
}

bool ControlBarSchemeRuntime::loadFromVfs() {
    // RefCode ControlBarSchemeManager::init() loads both directories with
    // INI_LOAD_OVERWRITE. INI::loadFileDirectory() reads root.ini first,
    // then sorted direct fragments, then sorted recursive descendants.
    constexpr container::Array<container::StringView, 2> roots{{
        "data/ini/default/ControlBarScheme",
        "data/ini/ControlBarScheme",
    }};
    const container::Vector<container::String> sources =
        game::ini::enumerateLegacyIniDirectories(roots);
    if (sources.empty()) {
        TD_LOG_WARN("[ControlBarScheme] No ControlBarScheme INI source found in VFS");
        return false;
    }

    auto& vfs = io::VFS::instance();
    container::String content;
    size_t sourcesRead = 0;
    for (const container::String& source : sources) {
        container::String layer = vfs.readAll(source);
        if (layer.empty()) continue;
        content.append(layer);
        content.push_back('\n');
        ++sourcesRead;
    }
    if (content.empty()) {
        TD_LOG_WARN("[ControlBarScheme] ControlBarScheme INI sources were empty");
        return false;
    }
    if (!parse(content)) {
        TD_LOG_WARN("[ControlBarScheme] No usable image scheme parsed from {} INI sources",
                    sourcesRead);
        return false;
    }

    TD_LOG_INFO("[ControlBarScheme] Loaded {} image parts from {} INI sources",
                m_imageParts.size(), sourcesRead);
    return true;
}

bool ControlBarSchemeRuntime::parse(const container::String& content) {
    std::istringstream stream(content);
    container::String line;
    container::Vector<ParsedScheme> schemes;
    ParsedScheme* currentScheme = nullptr;
    bool inImagePart = false;
    ControlBarSchemeImagePart current;

    while (std::getline(stream, line)) {
        line = withoutComment(std::move(line));
        if (line.empty()) continue;

        const container::String key = lower(line);
        if (key.rfind("controlbarscheme", 0) == 0) {
            ParsedScheme scheme;
            scheme.name = parseValueAfterKeyword(line);
            const container::String canonicalName = lower(scheme.name);
            const auto existing = std::find_if(
                schemes.begin(), schemes.end(), [&canonicalName](const ParsedScheme& candidate) {
                    return lower(candidate.name) == canonicalName;
                });
            if (existing != schemes.end()) {
                // RefCode newControlBarScheme() resets an existing same-name
                // object before parsing the later block. This is whole-block
                // overwrite, not field merge or ImagePart accumulation. The
                // object remains in its original manager-list position.
                *existing = std::move(scheme);
                currentScheme = &*existing;
            } else {
                schemes.push_back(std::move(scheme));
                currentScheme = &schemes.back();
            }
            inImagePart = false;
            continue;
        }

        if (key == "end") {
            if (inImagePart) {
                if (currentScheme && !current.imageName.empty() && current.w > 0 && current.h > 0) {
                    currentScheme->imageParts.push_back(current);
                }
                current = {};
                inImagePart = false;
            } else {
                currentScheme = nullptr;
            }
            continue;
        }

        if (key == "imagepart") {
            if (!currentScheme) continue;
            current = {};
            inImagePart = true;
            continue;
        }

        if (currentScheme && !inImagePart && key.rfind("side", 0) == 0) {
            currentScheme->side = parseValue(line);
            continue;
        }
        if (currentScheme && !inImagePart && key.rfind("screencreationres", 0) == 0) {
            if (auto parsed = parseVector2(line)) {
                currentScheme->screenW = parsed->first;
                currentScheme->screenH = parsed->second;
            }
            continue;
        }
        if (currentScheme && !inImagePart && key.rfind("righthudimage", 0) == 0) {
            currentScheme->rightHudImage = parseValue(line);
            currentScheme->namedImages[keywordLower(line)] = currentScheme->rightHudImage;
            continue;
        }
        if (currentScheme && !inImagePart && key.rfind("queuebuttonimage", 0) == 0) {
            currentScheme->queueButtonImage = parseValue(line);
            currentScheme->namedImages[keywordLower(line)] = currentScheme->queueButtonImage;
            continue;
        }
        if (currentScheme && !inImagePart && key.rfind("commandmarkerimage", 0) == 0) {
            currentScheme->commandMarkerImage = parseValue(line);
            currentScheme->namedImages[keywordLower(line)] = currentScheme->commandMarkerImage;
            continue;
        }
        if (currentScheme && !inImagePart && key.rfind("expbarforegroundimage", 0) == 0) {
            currentScheme->expBarForegroundImage = parseValue(line);
            currentScheme->namedImages[keywordLower(line)] = currentScheme->expBarForegroundImage;
            continue;
        }
        if (currentScheme && !inImagePart && key.rfind("powerpurchaseimage", 0) == 0) {
            currentScheme->powerPurchaseImage = parseValue(line);
            currentScheme->namedImages[keywordLower(line)] = currentScheme->powerPurchaseImage;
            continue;
        }
        if (currentScheme && !inImagePart && key.rfind("commandbarbordercolor", 0) == 0) {
            if (auto parsed = parseNamedColor(line)) {
                currentScheme->commandBarBorderColor = *parsed;
            }
            continue;
        }
        if (currentScheme && !inImagePart && key.rfind("buttonborderbuildcolor", 0) == 0) {
            if (auto parsed = parseNamedColor(line)) {
                currentScheme->buttonBorderBuildColor = *parsed;
            }
            continue;
        }
        if (currentScheme && !inImagePart && key.rfind("buttonborderactioncolor", 0) == 0) {
            if (auto parsed = parseNamedColor(line)) {
                currentScheme->buttonBorderActionColor = *parsed;
            }
            continue;
        }
        if (currentScheme && !inImagePart && key.rfind("buttonborderupgradecolor", 0) == 0) {
            if (auto parsed = parseNamedColor(line)) {
                currentScheme->buttonBorderUpgradeColor = *parsed;
            }
            continue;
        }
        if (currentScheme && !inImagePart && key.rfind("buttonbordersystemcolor", 0) == 0) {
            if (auto parsed = parseNamedColor(line)) {
                currentScheme->buttonBorderSystemColor = *parsed;
            }
            continue;
        }

        if (currentScheme && !inImagePart) {
            const container::String field = keywordLower(line);
            if (auto parsedPoint = parseVector2(line)) {
                currentScheme->namedPoints[field] = *parsedPoint;
                continue;
            }
            const container::String value = parseValue(line);
            if (!value.empty()) {
                currentScheme->namedImages[field] = value;
            }
            continue;
        }

        if (!inImagePart) continue;

        if (key.rfind("position", 0) == 0) {
            if (auto parsed = parseVector2(line)) {
                current.x = parsed->first;
                current.y = parsed->second;
            }
        } else if (key.rfind("size", 0) == 0) {
            if (auto parsed = parseVector2(line)) {
                current.w = parsed->first;
                current.h = parsed->second;
            }
        } else if (key.rfind("imagename", 0) == 0) {
            current.imageName = parseValue(line);
        } else if (key.rfind("layer", 0) == 0) {
            if (auto parsed = parseTrailingInt(line)) {
                current.layer = *parsed;
            }
        }
    }

    const container::String requestedSide = normalizedSide(m_playerSide);
    auto selectScheme = [&](const ParsedScheme& scheme) {
        m_imageParts = scaledImageParts(scheme);
        m_rightHudImage = scheme.rightHudImage;
        m_queueButtonImage = scheme.queueButtonImage;
        m_commandMarkerImage = scheme.commandMarkerImage;
        m_expBarForegroundImage = scheme.expBarForegroundImage;
        m_powerPurchaseImage = scheme.powerPurchaseImage;
        m_commandBarBorderColor = scheme.commandBarBorderColor;
        m_buttonBorderBuildColor = scheme.buttonBorderBuildColor;
        m_buttonBorderActionColor = scheme.buttonBorderActionColor;
        m_buttonBorderUpgradeColor = scheme.buttonBorderUpgradeColor;
        m_buttonBorderSystemColor = scheme.buttonBorderSystemColor;
        m_namedImages = scheme.namedImages;
        m_namedPoints.clear();
        const float scaleX = scheme.screenW > 0
            ? static_cast<float>(engine::presentation_defaults::VIRTUAL_WIDTH) / static_cast<float>(scheme.screenW)
            : 1.0f;
        const float scaleY = scheme.screenH > 0
            ? static_cast<float>(engine::presentation_defaults::VIRTUAL_HEIGHT) / static_cast<float>(scheme.screenH)
            : 1.0f;
        for (const auto& [name, point] : scheme.namedPoints) {
            m_namedPoints[name] = {
                static_cast<int>(point.first * scaleX),
                static_cast<int>(point.second * scaleY),
            };
        }
    };

    if (!requestedSide.empty()) {
        for (const auto& scheme : schemes) {
            if (normalizedSide(scheme.side) == requestedSide && !scheme.imageParts.empty()) {
                selectScheme(scheme);
                TD_LOG_INFO("[ControlBarScheme] Selected scheme '{}' for side '{}' res={}x{}",
                            scheme.name, requestedSide, scheme.screenW, scheme.screenH);
                return true;
            }
        }
    }

    for (const auto& scheme : schemes) {
        if (lower(scheme.name) == "default" && !scheme.imageParts.empty()) {
            selectScheme(scheme);
            TD_LOG_INFO("[ControlBarScheme] Selected scheme '{}' for side '{}'",
                        scheme.name,
                        requestedSide.empty() ? "Default" : requestedSide);
            return true;
        }
    }

    for (const auto& scheme : schemes) {
        if (!scheme.imageParts.empty()) {
            selectScheme(scheme);
            TD_LOG_INFO("[ControlBarScheme] Selected first image scheme '{}' res={}x{}",
                        scheme.name, scheme.screenW, scheme.screenH);
            return true;
        }
    }

    return !m_imageParts.empty();
}

} // namespace gui::ingame

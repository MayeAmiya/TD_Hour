#include "core/container/hash_containers.h"
#include "core/container/string_utils.h"
#include "GeneralsIniParser.h"
#include "VFS.h"
#include "debug/debug.h"
#include <fstream>
#include <algorithm>
#ifdef _WIN32
#include <Windows.h>
#endif
namespace game {

static const container::HashSet<container::String> KNOWN_BLOCK_TYPES = {
    "Draw", "Behavior", "Update", "AI", "Upgrade", "ActiveBody", "Body",
    "StructureBody", "ImmortalBody", "HighlanderBody", "InactiveBody", "UndeadBody",
    "HiveStructureBody",
    "ClientUpdate", "Module", "DrawModule",
    "ModuleTag", "Subsystem", "Voice", "Audio", "EvaEvent",
    "ConditionState", "DefaultConditionState", "TransitionState", "AnimationState",
    "StaticGameLOD", "DynamicGameLOD",
};

// A small set of shipped Object INI fields also uses the legacy whitespace
// assignment spelling (`Key Value`) instead of `Key = Value`.  These lines do
// not own an End marker. Treating them as anonymous blocks shifts the parser
// stack, so a later ConditionState/Draw End can close the containing Object
// and silently discard its Body and Behavior suffix.
static const container::HashSet<container::String>
    KNOWN_WHITESPACE_VALUE_KEYS = {
        "Model",
        "AnimationSpeedFactorRange",
        "ExtraPublicBone",
        "WaitForStateToFinishIfPossible",
    };

// Legacy INI content uses `;` for end-of-line comments, including on
// Locomotor/Body values where retaining the suffix silently corrupts enum and
// module-name parsing. Semicolons inside quoted user text remain data.
static container::StringView stripInlineComment(container::StringView line) {
    bool inQuotes = false;
    for (size_t index = 0; index < line.size(); ++index) {
        if (line[index] == '"') {
            inQuotes = !inQuotes;
        } else if (line[index] == ';' && !inQuotes) {
            return line.substr(0, index);
        }
    }
    return line;
}

// INI keywords are case-insensitive in the original parser.  In particular,
// the shipped Armor.ini contains an `END` spelling among the usual `End`
// markers; treating it as a new block leaves the parse stack unbalanced and
// hides every following top-level declaration from typed stores.
constexpr auto equalsAsciiIgnoreCase = container::asciiEqualIgnoreCase;

container::StringView GeneralsIniParser::trim(container::StringView s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

container::Vector<container::String> GeneralsIniParser::tokenize(container::StringView line) {
    container::Vector<container::String> tokens;
    container::String current;
    bool inQuotes = false;
    for (char c : line) {
        if (c == '"') { inQuotes = !inQuotes; current += c; }
        else if (c == ' ' && !inQuotes) {
            if (!current.empty()) { tokens.push_back(std::move(current)); current.clear(); }
        } else {
            current += c;
        }
    }
    if (!current.empty()) tokens.push_back(std::move(current));
    return tokens;
}

bool GeneralsIniParser::parse(container::StringView content,
                              container::StringView sourcePath) {
    m_blocks.clear();
    container::Vector<IniBlock*> stack;
    container::String owned(content);
    const auto sharedSourcePath = sourcePath.empty()
        ? container::SharedPtr<const container::String>{}
        : std::make_shared<const container::String>(sourcePath);
    uint32_t currentLine = 1;

    const auto appendChild = [&](IniBlock child, bool opensScope) {
        child.source = {
            .path = sharedSourcePath,
            .line = currentLine,
        };
        if (!stack.empty()) {
            IniBlock& parent = *stack.back();
            const size_t index = parent.children.size();
            parent.children.push_back(std::move(child));
            parent.entries.push_back({.kind = IniEntryKind::Child, .index = index});
            if (opensScope) stack.push_back(&parent.children.back());
            return;
        }
        m_blocks.push_back(std::move(child));
        if (opensScope) stack.push_back(&m_blocks.back());
    };

    const auto appendValue = [&](container::String key, container::String value) {
        if (stack.empty()) return;
        IniBlock& parent = *stack.back();
        const size_t index = parent.values.size();
        parent.values.emplace_back(std::move(key), std::move(value));
        parent.valueSourceLines.push_back(currentLine);
        parent.entries.push_back({.kind = IniEntryKind::Value, .index = index});
    };

    size_t pos = 0;
    while (pos < owned.size()) {
        size_t eol = owned.find('\n', pos);
        if (eol == container::String::npos) eol = owned.size();
        size_t len = eol - pos;
        if (len > 0 && owned[eol - 1] == '\r') --len;
        container::StringView line(owned.data() + pos, len);
        pos = eol + 1;

        auto sv = trim(stripInlineComment(line));
        if (sv.empty() || sv[0] == ';') {
            ++currentLine;
            continue;
        }

        auto eqPos = sv.find('=');

        if (eqPos != container::StringView::npos) {
            auto key = trim(sv.substr(0, eqPos));
            auto val = trim(sv.substr(eqPos + 1));

            // AliasConditionState is an inline declaration that applies to
            // the preceding condition state.  It has no matching End, so it
            // must be retained as an ordered child without becoming a stack
            // frame.  Treating it as a normal block corrupts the parse tree
            // for large object INIs and eventually overflows the stack.
            if (key == "AliasConditionState") {
                IniBlock alias;
                alias.type = container::String(key);
                alias.name = container::String(val);
                appendChild(std::move(alias), false);
                ++currentLine;
                continue;
            }

            // `Upgrade` is a nested object-module declaration in an Object
            // recipe, but it is also the ordinary scalar target field of a
            // CommandButton.  Treating `Upgrade = Foo` on a button as a
            // child scope leaves the button stack unclosed, causing every
            // following command button to be swallowed into the first one.
            // The original parser dispatches by the current FieldParse table;
            // preserve that context-sensitive distinction here.
            const bool commandButtonUpgradeField = !stack.empty() &&
                equalsAsciiIgnoreCase(stack.back()->type, "CommandButton") &&
                equalsAsciiIgnoreCase(key, "Upgrade");
            if (KNOWN_BLOCK_TYPES.count(container::String(key)) && !commandButtonUpgradeField) {
                IniBlock block;
                block.type = container::String(key);
                block.name = container::String(val);
                appendChild(std::move(block), true);
            } else {
                appendValue(container::String(key), container::String(val));
            }
        } else {
            auto tokens = tokenize(sv);
            if (tokens.empty()) {
                ++currentLine;
                continue;
            }

            if (equalsAsciiIgnoreCase(tokens[0], "End")) {
                if (!stack.empty()) stack.pop_back();
                ++currentLine;
                continue;
            }

            // Content files use both `AliasConditionState = FOO` and the
            // older whitespace form `AliasConditionState FOO`.  Neither has
            // an End marker; retain the declaration in draw-child order but
            // never push it onto the parser stack.
            if (tokens[0] == "AliasConditionState") {
                IniBlock alias;
                alias.type = tokens[0];
                for (size_t index = 1; index < tokens.size(); ++index) {
                    if (!alias.name.empty()) alias.name.push_back(' ');
                    alias.name += tokens[index];
                }
                appendChild(std::move(alias), false);
                ++currentLine;
                continue;
            }

            container::String blockType = tokens[0];
            container::String blockName;
            // TransitionState has two positional names (source and target).
            // Preserve the entire tail instead of silently losing the target;
            // ordinary one-name blocks retain their previous representation.
            for (size_t index = 1; index < tokens.size(); ++index) {
                if (!blockName.empty()) blockName.push_back(' ');
                blockName += tokens[index];
            }

            if (KNOWN_WHITESPACE_VALUE_KEYS.count(blockType)) {
                appendValue(std::move(blockType), std::move(blockName));
                ++currentLine;
                continue;
            }

            // Object/ObjectReskin are FieldParse top-level declarations in
            // the retail loader, never legal children of a module. A few
            // shipped legacy recipes contain an unterminated/unknown inline
            // construct before the next object; the generic stack parser
            // otherwise swallows that object (and its WorkerAIUpdate) into
            // the previous recipe. Re-synchronize at this typed boundary.
            if (equalsAsciiIgnoreCase(blockType, "Object") ||
                equalsAsciiIgnoreCase(blockType, "ObjectReskin")) {
                stack.clear();
            }

            IniBlock block;
            block.type = std::move(blockType);
            block.name = std::move(blockName);
            appendChild(std::move(block), true);
        }
        ++currentLine;
    }

    return true;
}

bool GeneralsIniParser::parseFile(const container::String& path) {
    auto& vfs = io::VFS::instance();
    if (vfs.exists(path)) {
        // INI::load/loadFileDirectory opens file instance 0: the VFS winner.
        // Physical base/ZH/mod copies of one logical path are alternatives,
        // not fragments to concatenate. Explicit Map/CreateOverrides sources
        // are loaded as separate logical files by their owning loader.
        return parse(vfs.readAll(path), path);
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        TD_LOG_WARN("[GeneralsIni] Cannot open: {}", path);
        return false;
    }
    container::String content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    return parse(content, path);
}

void GeneralsIniParser::setBlockHandler(const container::String& type, BlockHandler handler) {
    m_handlers[type] = std::move(handler);
}

void GeneralsIniParser::loadAllFromDirectory(const container::String& dirPath) {
#ifdef _WIN32
    WIN32_FIND_DATAA findData;
    container::String searchPath = dirPath + "\\*.ini";
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        TD_LOG_WARN("[GeneralsIni] Cannot find INI files in: {}; directory skipped",
                    dirPath);
        return;
    }

    do {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        container::String filePath = dirPath + "\\" + findData.cFileName;
        parseFile(filePath);
    } while (FindNextFileA(hFind, &findData));
    FindClose(hFind);
#endif

    for (auto& block : m_blocks) {
        auto it = m_handlers.find(block.type);
        if (it != m_handlers.end()) {
            it->second(block);
        }
    }
}

} // namespace game

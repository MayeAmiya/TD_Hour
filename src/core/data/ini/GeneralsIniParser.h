#pragma once

#include "core/container/hash_containers.h"
#include "core/container/container_types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
namespace game {

struct IniSourceLocation final {
    container::SharedPtr<const container::String> path;
    uint32_t line = 0;

    [[nodiscard]] container::StringView pathView() const noexcept {
        return path ? container::StringView{*path} : container::StringView{};
    }
    [[nodiscard]] bool known() const noexcept {
        return path && !path->empty() && line != 0;
    }
};

// `values` and `children` retain convenient typed storage for existing
// loaders. `entries` is their source-order index: Object recipe compilation
// must distinguish `Field → Module → Field` from a bulk field pass followed
// by a bulk module pass, just like the original INI FieldParse loop.
enum class IniEntryKind : uint8_t {
    Value,
    Child,
};

struct IniEntry final {
    IniEntryKind kind = IniEntryKind::Value;
    size_t index = 0;
};

struct IniBlock {
    container::String type;
    container::String name;
    container::Vector<std::pair<container::String, container::String>> values;
    container::Vector<IniBlock> children;
    container::Vector<IniEntry> entries;
    IniSourceLocation source;
    // Kept parallel to `values` so existing loaders retain their pair-based
    // iteration API while typed diagnostics can still identify the exact
    // authored line.
    container::Vector<uint32_t> valueSourceLines;

    [[nodiscard]] IniSourceLocation valueSource(size_t index) const noexcept {
        return {
            .path = source.path,
            .line = index < valueSourceLines.size()
                ? valueSourceLines[index] : source.line,
        };
    }
};

class GeneralsIniParser {
public:
    bool parse(container::StringView content,
               container::StringView sourcePath = {});
    bool parseFile(const container::String& path);

    const container::Vector<IniBlock>& blocks() const { return m_blocks; }
    [[nodiscard]] container::Vector<IniBlock> takeBlocks() noexcept {
        return std::move(m_blocks);
    }

    using BlockHandler = std::function<void(const IniBlock&)>;
    void setBlockHandler(const container::String& type, BlockHandler handler);

    void loadAllFromDirectory(const container::String& dirPath);

private:
    container::StringView trim(container::StringView s);
    container::Vector<container::String> tokenize(container::StringView line);

    container::Vector<IniBlock> m_blocks;
    container::HashMap<container::String, BlockHandler> m_handlers;
};

} // namespace game

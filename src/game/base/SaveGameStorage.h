#pragma once

#include "core/container/container_types.h"

#include <cstdint>
namespace game {

struct SaveGameEntry {
    container::String fileName;
    container::String displayName;
};

class SaveGameStorage {
public:
    static SaveGameStorage& instance();

    container::Vector<SaveGameEntry> listSaves() const;
    container::Vector<uint8_t> readSave(const container::String& fileName) const;
    bool writeSave(const container::String& fileName, const container::Vector<uint8_t>& content) const;
    bool deleteSave(const container::String& fileName) const;

private:
    SaveGameStorage() = default;
};

} // namespace game

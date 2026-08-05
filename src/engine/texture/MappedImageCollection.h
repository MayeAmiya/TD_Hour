#pragma once

#include "core/container/hash_containers.h"
#include "presentation/ui/MappedImageContentLayer.h"

#include <cstdint>
namespace engine {

struct MappedImage {
    container::String name;
    container::String textureFile;    // TGA filename (e.g. "MainMenuBackdropuserinterface.tga")
    int32_t textureWidth = 0;
    int32_t textureHeight = 0;
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;
    int32_t status = 0;

    int32_t imageWidth() const { return right - left; }
    int32_t imageHeight() const { return bottom - top; }
};

class MappedImageCollection {
public:
    MappedImageCollection() = default;
    ~MappedImageCollection() = default;

    // Load all MappedImages INI files from VFS
    void load();

    // Find image by name (case-insensitive)
    const MappedImage* findByName(const container::String& name) const;

    // Main-thread session boundary. A later map.ini/solo.ini layer sparsely
    // patches the preceding layer, starting from the immutable base image when
    // the name first appears. The base catalog is never mutated, so Next/Retry
    // cannot leak one map's UI assets into another session.
    void activateSession(
        uint64_t presentationEpoch,
        container::Span<const ui::MappedImageContentLayer> layers);
    void clearSession() noexcept;
    [[nodiscard]] uint64_t activeSessionEpoch() const noexcept {
        return m_sessionEpoch;
    }

    size_t getImageCount() const { return m_images.size(); }

    static MappedImageCollection& instance() {
        static MappedImageCollection s_instance;
        return s_instance;
    }

private:
    using ImageMap = container::HashMap<
        container::String, container::UniquePtr<MappedImage>>;
    void parseINIData(
        const container::String& data, ImageMap& destination,
        const ImageMap* fallback);
    static container::String toLowerStr(const container::String& s);

    // findByName returns pointers that remain valid as additional INI files load.
    ImageMap m_images;
    ImageMap m_sessionImages;
    uint64_t m_sessionEpoch = 0;
};

} // namespace engine

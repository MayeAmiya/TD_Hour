#include "core/container/container_types.h"
#include "W3dAssetIdentity.h"

#include <algorithm>

namespace data::w3d {
namespace {

[[nodiscard]] char lowerAscii(char value) noexcept {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value + ('a' - 'A')) : value;
}

[[nodiscard]] container::String lowerAscii(container::String value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](char value) noexcept { return lowerAscii(value); });
    return value;
}

[[nodiscard]] bool endsWithInsensitive(
    container::StringView value, container::StringView suffix) noexcept {
    if (value.size() < suffix.size()) return false;
    const size_t offset = value.size() - suffix.size();
    for (size_t index = 0; index < suffix.size(); ++index) {
        if (lowerAscii(value[offset + index]) != lowerAscii(suffix[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<container::String> canonicalRelativePath(
    container::StringView source) {
    if (source.empty() || source.front() == '/' || source.front() == '\\') {
        return std::nullopt;
    }
    container::String normalized;
    normalized.reserve(source.size());
    bool previousSlash = false;
    for (char value : source) {
        const char current = value == '\\' ? '/' : lowerAscii(value);
        if (current == '/') {
            if (normalized.empty() || previousSlash) continue;
            previousSlash = true;
        } else {
            previousSlash = false;
        }
        normalized.push_back(current);
    }
    while (!normalized.empty() && normalized.back() == '/') {
        normalized.pop_back();
    }
    while (normalized.rfind("./", 0) == 0) normalized.erase(0, 2);
    if (normalized.empty() || normalized.front() == '/' ||
        normalized.find(':') != container::String::npos) {
        return std::nullopt;
    }
    size_t start = 0;
    while (start <= normalized.size()) {
        const size_t end = normalized.find('/', start);
        const container::StringView component(
            normalized.data() + start,
            (end == container::String::npos ? normalized.size() : end) - start);
        if (component.empty() || component == "." || component == "..") {
            return std::nullopt;
        }
        if (end == container::String::npos) break;
        start = end + 1;
    }
    return normalized;
}

[[nodiscard]] container::String basename(container::StringView value) {
    const size_t slash = value.find_last_of('/');
    return container::String(
        slash == container::StringView::npos ? value : value.substr(slash + 1));
}

[[nodiscard]] container::String stripExtension(container::String value) {
    if (endsWithInsensitive(value, kW3dExtension)) {
        value.resize(value.size() - kW3dExtension.size());
    }
    return value;
}

void setError(container::String* error, container::String value) {
    if (error) *error = std::move(value);
}

} // namespace

std::optional<W3dModelIdentity> resolveW3dModelIdentity(
    container::StringView source, container::StringView explicitPrototype,
    container::String* error) {
    if (error) error->clear();
    const std::optional<container::String> normalized =
        canonicalRelativePath(source);
    if (!normalized) {
        setError(error,
            "W3D source must be a non-empty relative VFS path or logical prototype");
        return std::nullopt;
    }

    const bool hasPath = normalized->find('/') != container::String::npos;
    const bool isPath = endsWithInsensitive(*normalized, kW3dExtension);
    container::String fileStem;
    container::String defaultPrototype;
    container::String sourcePath;
    if (hasPath) {
        sourcePath = *normalized;
        if (!isPath) sourcePath += kW3dExtension;
        fileStem = stripExtension(basename(sourcePath));
        defaultPrototype = fileStem;
    } else if (isPath) {
        fileStem = stripExtension(*normalized);
        defaultPrototype = fileStem;
        sourcePath = container::String{kDefaultW3dRoot} + fileStem +
            container::String{kW3dExtension};
    } else {
        const size_t separator = normalized->find('.');
        fileStem = separator == container::String::npos
            ? *normalized : normalized->substr(0, separator);
        defaultPrototype = *normalized;
        sourcePath = container::String{kDefaultW3dRoot} + fileStem +
            container::String{kW3dExtension};
    }
    if (fileStem.empty()) {
        setError(error, "W3D source does not contain a filename");
        return std::nullopt;
    }

    container::String prototype;
    if (explicitPrototype.empty()) {
        prototype = std::move(defaultPrototype);
    } else {
        const std::optional<container::String> normalizedPrototype =
            canonicalRelativePath(explicitPrototype);
        if (!normalizedPrototype) {
            setError(error, "W3D prototype must be a non-empty relative name");
            return std::nullopt;
        }
        prototype = stripExtension(basename(*normalizedPrototype));
    }
    if (prototype.empty()) {
        setError(error, "W3D prototype name is empty");
        return std::nullopt;
    }
    return W3dModelIdentity{
        .sourcePath = lowerAscii(std::move(sourcePath)),
        .prototype = lowerAscii(std::move(prototype)),
    };
}

container::String w3dHierarchySourcePath(
    container::StringView hierarchyName) {
    container::String stem = stripExtension(
        basename(lowerAscii(container::String{hierarchyName})));
    return stem.empty() ? container::String{}
        : container::String{kDefaultW3dRoot} + stem +
            container::String{kW3dExtension};
}

container::String w3dAnimationFileStem(
    container::StringView logicalAnimationName) {
    const size_t dot = logicalAnimationName.find('.');
    const container::StringView suffix = dot == container::StringView::npos
        ? logicalAnimationName : logicalAnimationName.substr(dot + 1);
    return lowerAscii(stripExtension(basename(suffix)));
}

container::String w3dAnimationSourcePath(
    container::StringView logicalAnimationName) {
    const container::String stem =
        w3dAnimationFileStem(logicalAnimationName);
    return stem.empty() ? container::String{}
        : container::String{kDefaultW3dRoot} + stem +
            container::String{kW3dExtension};
}

} // namespace data::w3d

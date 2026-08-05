#pragma once

#include "container/container_types.h"

namespace io::virtual_path {

namespace detail {

[[nodiscard]] inline container::String canonicalize(
    container::StringView input, bool allowWildcards) {
    if (input.empty() || input.front() == '/' || input.front() == '\\' ||
        input.find(':') != container::StringView::npos) {
        return {};
    }

    container::String result;
    result.reserve(input.size());
    size_t begin = 0;
    while (begin <= input.size()) {
        const size_t end = input.find_first_of("/\\", begin);
        const size_t segmentEnd = end == container::StringView::npos
            ? input.size() : end;
        const container::StringView segment = input.substr(
            begin, segmentEnd - begin);

        // Repeated/trailing separators and explicit current-directory
        // segments are aliases of the same VFS identity. Parent traversal is
        // never a valid virtual resource path: LocalFileSystem is writable.
        if (!segment.empty() && segment != ".") {
            if (segment == "..") return {};
            if (!result.empty()) result.push_back('/');
            for (const unsigned char character : segment) {
                if (character < 0x20u || character == 0x7fu ||
                    (!allowWildcards &&
                     (character == '*' || character == '?'))) {
                    return {};
                }
                result.push_back(character >= 'A' && character <= 'Z'
                    ? static_cast<char>(character + ('a' - 'A'))
                    : static_cast<char>(character));
            }
        }
        if (end == container::StringView::npos) break;
        begin = end + 1u;
    }
    return result;
}

} // namespace detail

[[nodiscard]] inline container::String canonical(
    container::StringView input) {
    return detail::canonicalize(input, false);
}

[[nodiscard]] inline container::String pattern(
    container::StringView input) {
    return detail::canonicalize(input, true);
}

} // namespace io::virtual_path

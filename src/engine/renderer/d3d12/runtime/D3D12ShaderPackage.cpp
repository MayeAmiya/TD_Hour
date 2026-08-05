#include "D3D12ShaderPackage.h"

#include "core/container/hash_containers.h"
#include "debug/debug.h"

#include <Windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>

namespace engine::d3d12 {
namespace {

constexpr container::StringView kPackageFormat =
    "GeneralsTDShaderPackage";
constexpr uintmax_t kMaximumShaderBlobBytes = 16u * 1024u * 1024u;

std::filesystem::path executableShaderDirectory() {
    container::Array<wchar_t, 32768> executablePath{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executablePath.data(),
        static_cast<DWORD>(executablePath.size()));
    if (length == 0 || length >= executablePath.size()) {
        TD_LOG_ERROR(
            "[ShaderPackage] executable path unavailable: error={}",
            static_cast<uint32_t>(GetLastError()));
        return {};
    }
    return std::filesystem::path(executablePath.data()).parent_path() /
        L"shaders";
}

bool readManifest(
    const std::filesystem::path& path,
    container::HashMap<container::String, container::String>& values) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return false;
    container::String line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        const size_t separator = line.find('=');
        if (separator == container::String::npos) continue;
        values.insert_or_assign(
            line.substr(0, separator), line.substr(separator + 1u));
    }
    return input.eof() || !input.fail();
}

bool matches(
    const container::HashMap<container::String, container::String>& values,
    container::StringView key, container::StringView expected) {
    const auto found = values.find(container::String(key));
    return found != values.end() && found->second == expected;
}

bool readBlob(const std::filesystem::path& path,
              container::Vector<uint8_t>& output) {
    std::error_code error;
    const uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size < 4u || size > kMaximumShaderBlobBytes) return false;
    output.resize(static_cast<size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open() ||
        !input.read(reinterpret_cast<char*>(output.data()),
                    static_cast<std::streamsize>(output.size())) ||
        std::memcmp(output.data(), "DXBC", 4u) != 0) {
        output.clear();
        return false;
    }
    return true;
}

} // namespace

bool loadShaderPackage(
    container::StringView shaderName,
    container::StringView expectedVersion,
    container::StringView expectedSourceSha256,
    container::Span<const ShaderPackageEntrySpec> entries,
    container::Vector<container::Vector<uint8_t>>& bytecode) {
    bytecode.clear();
    if (shaderName.empty() || expectedVersion.empty() ||
        expectedSourceSha256.empty() || entries.empty()) {
        return false;
    }
    const std::filesystem::path directory = executableShaderDirectory();
    if (directory.empty()) return false;
    const std::filesystem::path manifestPath = directory /
        (container::String(shaderName) + ".manifest");
    container::HashMap<container::String, container::String> manifest;
    if (!readManifest(manifestPath, manifest)) {
        TD_LOG_ERROR(
            "[ShaderPackage] '{}' manifest missing/unreadable: {}",
            shaderName, manifestPath.string());
        return false;
    }
    if (!matches(manifest, "format", kPackageFormat) ||
        !matches(manifest, "shader", shaderName)) {
        TD_LOG_ERROR(
            "[ShaderPackage] '{}' manifest format/identity mismatch: {}",
            shaderName, manifestPath.string());
        return false;
    }
    if (!matches(manifest, "version", expectedVersion)) {
        TD_LOG_ERROR(
            "[ShaderPackage] '{}' package version mismatch (expected {}): {}",
            shaderName, expectedVersion, manifestPath.string());
        return false;
    }
    if (!matches(manifest, "source_sha256", expectedSourceSha256)) {
        TD_LOG_ERROR(
            "[ShaderPackage] '{}' source SHA-256 mismatch (expected {}): {}",
            shaderName, expectedSourceSha256, manifestPath.string());
        return false;
    }

    bytecode.reserve(entries.size());
    for (const ShaderPackageEntrySpec& entry : entries) {
        if (!matches(manifest, entry.fileManifestKey,
                     entry.expectedFile) ||
            !matches(manifest, entry.profileManifestKey,
                     entry.expectedProfile)) {
            TD_LOG_ERROR(
                "[ShaderPackage] '{}' entry metadata mismatch "
                "(expected file '{}', profile '{}') in {}",
                shaderName, entry.expectedFile, entry.expectedProfile,
                manifestPath.string());
            bytecode.clear();
            return false;
        }
        container::Vector<uint8_t> blob;
        if (!readBlob(directory / container::String(entry.expectedFile),
                      blob)) {
            TD_LOG_ERROR(
                "[ShaderPackage] '{}' DXBC missing/corrupt: {}",
                shaderName, entry.expectedFile);
            bytecode.clear();
            return false;
        }
        bytecode.push_back(std::move(blob));
    }
    TD_LOG_INFO(
        "[ShaderPackage] '{}' v{} loaded (source SHA-256 {})",
        shaderName, expectedVersion, expectedSourceSha256);
    return true;
}

} // namespace engine::d3d12

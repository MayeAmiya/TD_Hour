#include "core/container/hash_containers.h"
#include "core/container/string_utils.h"
#include "engine/renderer/world/model/W3dAssetCache.h"

#include "VFS.h"
#include "LocaleResourceLocator.h"
#include "data/w3d/W3dAssetIdentity.h"
#include "data/w3d/W3dLoader.h"
#include "data/w3d/W3dTypes.h"
#include "engine/resource/ResourceSchedulerRuntime.h"
#include "debug/debug.h"

#include <algorithm>
#include <utility>

namespace engine::render {
namespace {

char lowerAscii(char value) noexcept {
    if (value >= 'A' && value <= 'Z') return static_cast<char>(value + ('a' - 'A'));
    return value;
}

container::String lowerAscii(container::String value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](char current) noexcept { return lowerAscii(current); });
    return value;
}

uint64_t cpuLoadPriorityKey(W3dModelHandle handle) noexcept {
    return (static_cast<uint64_t>(handle.generation) << 32u) |
        handle.index;
}

container::String boundedName(const char* data, size_t capacity) {
    size_t length = 0;
    while (length < capacity && data[length] != '\0') ++length;
    return container::String(data, length);
}

bool containsHierarchy(const data::w3d::ParsedW3D& parsed,
                       container::StringView hierarchyName) {
    const container::String wanted = lowerAscii(container::String(hierarchyName));
    return std::any_of(parsed.hierarchies.begin(), parsed.hierarchies.end(),
        [&wanted](const data::w3d::ParsedHierarchy& hierarchy) {
            return lowerAscii(boundedName(hierarchy.name, data::w3d::NAME_LEN)) == wanted;
        });
}

constexpr auto equalAsciiInsensitive = container::asciiEqualIgnoreCase;

container::String canonicalTextureIdentity(container::StringView textureName) {
    if (textureName.empty()) return {};
    if (const auto locator = io::acquireLocaleResourceLocator()) {
        if (const auto resolved = locator->resolve(
                io::LocaleResourceKind::Texture, textureName)) {
            return container::String{"texture:"} + *resolved;
        }
    }

    // A missing dependency still needs an identity for diagnostics and later
    // readiness publication. Preserve its complete authored path: reducing it
    // to basename/stem aliases unrelated textures from different directories.
    container::String canonical{textureName};
    std::replace(canonical.begin(), canonical.end(), '\\', '/');
    canonical = lowerAscii(std::move(canonical));
    return canonical.empty()
        ? container::String{}
        : container::String{"texture:"} + canonical;
}

uint32_t appendDependencyNode(W3dAssetDependencies& dependencies,
                              W3dDependencyKind kind,
                              W3dDependencyState state,
                              container::String logicalName,
                              container::String canonicalIdentity,
                              uint64_t revision = 0) {
    dependencies.nodes.push_back({
        .kind = kind,
        .state = state,
        .logicalName = std::move(logicalName),
        .canonicalIdentity = std::move(canonicalIdentity),
        .revision = revision,
    });
    return static_cast<uint32_t>(dependencies.nodes.size() - 1u);
}

RenderAssetDependencyResolution dependencyResolution(
    W3dDependencyState state) noexcept {
    switch (state) {
    case W3dDependencyState::Ready:
        return RenderAssetDependencyResolution::Ready;
    case W3dDependencyState::Missing:
        return RenderAssetDependencyResolution::Missing;
    case W3dDependencyState::Fallback:
        return RenderAssetDependencyResolution::Fallback;
    case W3dDependencyState::Referenced:
        return RenderAssetDependencyResolution::Referenced;
    }
    return RenderAssetDependencyResolution::Referenced;
}

void appendDependencyEdge(
    W3dAssetDependencies& dependencies,
    uint32_t sourceNode,
    uint32_t targetNode,
    RenderAssetDependencyPolicy policy) {
    if (sourceNode >= dependencies.nodes.size() ||
        targetNode >= dependencies.nodes.size()) {
        return;
    }
    W3dDependencyNode& target = dependencies.nodes[targetNode];
    if (target.incomingReferences != std::numeric_limits<uint32_t>::max()) {
        ++target.incomingReferences;
    }
    dependencies.edges.push_back({
        .sourceNode = sourceNode,
        .targetNode = targetNode,
        .policy = policy,
        .resolution = dependencyResolution(target.state),
    });
}

void updateDependencyTargetResolution(
    W3dAssetDependencies& dependencies, uint32_t targetNode) noexcept {
    if (targetNode >= dependencies.nodes.size()) return;
    const RenderAssetDependencyResolution resolution =
        dependencyResolution(dependencies.nodes[targetNode].state);
    for (W3dDependencyEdge& edge : dependencies.edges) {
        if (edge.targetNode == targetNode) edge.resolution = resolution;
    }
}

void hashCombine(size_t& seed, size_t value) noexcept {
    seed ^= value + static_cast<size_t>(0x9E3779B9u) + (seed << 6) + (seed >> 2);
}

uint64_t saturatedAdd(uint64_t lhs, uint64_t rhs) noexcept {
    return rhs > std::numeric_limits<uint64_t>::max() - lhs
        ? std::numeric_limits<uint64_t>::max()
        : lhs + rhs;
}

uint64_t saturatedMultiply(size_t count, size_t stride) noexcept {
    if (count == 0 || stride == 0) return 0;
    if (count > std::numeric_limits<uint64_t>::max() / stride)
        return std::numeric_limits<uint64_t>::max();
    return static_cast<uint64_t>(count) * static_cast<uint64_t>(stride);
}

W3dGpuUploadPriority sanitizedPriority(W3dGpuUploadPriority priority) noexcept {
    return priority < W3dGpuUploadPriority::Count
        ? priority : W3dGpuUploadPriority::Normal;
}

} // namespace

uint64_t estimateW3dGpuUploadBytes(const CpuStaticModel& model) noexcept {
    uint64_t bytes = saturatedMultiply(
        model.materials.size(), sizeof(StaticMaterialDesc));
    bytes = saturatedAdd(bytes, saturatedMultiply(
        model.additionalModels.size(),
        sizeof(CpuStaticModel::HierarchyModelReference)));
    bytes = saturatedAdd(bytes, saturatedMultiply(
        model.proxies.size(),
        sizeof(CpuStaticModel::HierarchyProxyReference)));
    for (const StaticMaterialDesc& material : model.materials) {
        bytes = saturatedAdd(bytes, material.name.size());
        bytes = saturatedAdd(bytes, material.textureName.size());
        bytes = saturatedAdd(bytes, material.detailTextureName.size());
    }
    for (const CpuStaticMesh& mesh : model.meshes) {
        bytes = saturatedAdd(bytes, sizeof(CpuStaticMesh));
        bytes = saturatedAdd(bytes, mesh.name.size());
        bytes = saturatedAdd(bytes, saturatedMultiply(
            mesh.vertices.size(), sizeof(StaticMeshVertex)));
        bytes = saturatedAdd(bytes, saturatedMultiply(
            mesh.indices.size(), sizeof(uint32_t)));
        bytes = saturatedAdd(bytes, saturatedMultiply(
            mesh.primitives.size(), sizeof(StaticPrimitive)));
    }
    return bytes;
}

size_t W3dAssetCache::ModelKeyHash::operator()(const ModelKey& key) const noexcept {
    size_t result = std::hash<container::String>{}(key.sourcePath);
    hashCombine(result, std::hash<container::String>{}(key.prototype));
    hashCombine(result, std::hash<bool>{}(key.includeHiddenMeshes));
    hashCombine(result, std::hash<bool>{}(key.includeCollisionMeshes));
    return result;
}

W3dAssetCache::W3dAssetCache()
    : W3dAssetCache(io::VFS::instance()) {}

W3dAssetCache::W3dAssetCache(io::VFS& vfs) noexcept
    : m_vfs(&vfs) {}

W3dAssetCache::~W3dAssetCache() {
    try {
        stopCpuLoads();
        clear();
    } catch (...) {
        // Destruction must not propagate a backend retirement callback error.
    }
}

std::optional<W3dAssetCache::ModelKey> W3dAssetCache::resolveKey(
    const W3dAssetRequest& request, container::String& error) {
    const std::optional<data::w3d::W3dModelIdentity> identity =
        data::w3d::resolveW3dModelIdentity(
            request.source, request.prototype, &error);
    if (!identity) return std::nullopt;
    ModelKey key;
    key.sourcePath = identity->sourcePath;
    key.prototype = identity->prototype;
    key.includeHiddenMeshes = request.includeHiddenMeshes;
    key.includeCollisionMeshes = request.includeCollisionMeshes;
    return key;
}

W3dModelHandle W3dAssetCache::request(const W3dAssetRequest& requestDescription) {
    ++m_requests;
    container::String resolutionError;
    auto key = resolveKey(requestDescription, resolutionError);
    if (!key) {
        ++m_cacheMisses;
        return {};
    }

    if (const auto found = m_modelLookup.find(*key); found != m_modelLookup.end()) {
        ++m_cacheHits;
        Slot& slot = m_slots[found->second];
        slot.lastReachableFrame = m_residencyFrame;
        const W3dModelHandle handle{found->second, slot.generation};
        if (requestDescription.queueGpuUpload)
            queueGpuUpload(handle, requestDescription.gpuUploadPriority);
        return handle;
    }

    ++m_cacheMisses;
    const W3dModelHandle handle = allocateSlot(std::move(*key));
    m_slots[handle.index].lastReachableFrame = m_residencyFrame;
    loadCpuAsset(handle.index);
    if (requestDescription.queueGpuUpload)
        queueGpuUpload(handle, requestDescription.gpuUploadPriority);
    return handle;
}

W3dModelHandle W3dAssetCache::request(container::StringView logicalPrototype,
                                      bool queueUpload) {
    W3dAssetRequest requestDescription;
    requestDescription.source = logicalPrototype;
    requestDescription.queueGpuUpload = queueUpload;
    return request(requestDescription);
}

W3dModelHandle W3dAssetCache::requestAsync(
    const W3dAssetRequest& requestDescription) {
    ++m_requests;
    container::String resolutionError;
    auto key = resolveKey(requestDescription, resolutionError);
    if (!key) {
        ++m_cacheMisses;
        return {};
    }

    if (const auto found = m_modelLookup.find(*key); found != m_modelLookup.end()) {
        ++m_cacheHits;
        Slot& slot = m_slots[found->second];
        slot.lastReachableFrame = m_residencyFrame;
        const W3dModelHandle handle{found->second, slot.generation};
        if (slot.state == W3dAssetState::CpuLoadQueued ||
            slot.state == W3dAssetState::CpuLoading) {
            const RenderAssetPriority requestedCpuPriority =
                sanitizedPriority(requestDescription.gpuUploadPriority);
            for (CpuLoadJob& job : m_cpuLoadJobs) {
                if (job.handle == handle && job.revision == slot.revision &&
                    job.session == m_cpuLoadSession) {
                    job.priority = std::max(
                        job.priority, requestedCpuPriority);
                }
            }
            auto& priority = m_cpuLoadPriorities[cpuLoadPriorityKey(handle)];
            priority = std::max(priority, requestedCpuPriority);
            if (requestDescription.queueGpuUpload) {
                const W3dGpuUploadPriority requestedPriority =
                    sanitizedPriority(requestDescription.gpuUploadPriority);
                slot.queueGpuAfterCpuLoadPriority = slot.queueGpuAfterCpuLoad
                    ? std::max(slot.queueGpuAfterCpuLoadPriority,
                               requestedPriority)
                    : requestedPriority;
                slot.queueGpuAfterCpuLoad = true;
            }
        } else if (requestDescription.queueGpuUpload) {
            queueGpuUpload(handle, requestDescription.gpuUploadPriority);
        }
        return handle;
    }

    ++m_cacheMisses;
    const W3dModelHandle handle = allocateSlot(std::move(*key));
    Slot& slot = m_slots[handle.index];
    slot.lastReachableFrame = m_residencyFrame;
    slot.state = W3dAssetState::CpuLoadQueued;
    slot.queueGpuAfterCpuLoad = requestDescription.queueGpuUpload;
    slot.queueGpuAfterCpuLoadPriority =
        sanitizedPriority(requestDescription.gpuUploadPriority);
    m_cpuLoadJobs.push_back({
        .handle = handle,
        .revision = slot.revision,
        .session = m_cpuLoadSession,
        .key = slot.key,
        .enqueueSequence = m_nextCpuLoadSequence++,
        .priority = sanitizedPriority(
            requestDescription.gpuUploadPriority),
    });
    m_cpuLoadPriorities[cpuLoadPriorityKey(handle)] = sanitizedPriority(
        requestDescription.gpuUploadPriority);
    collectFinishedCpuLoads();
    dispatchCpuLoads();
    return handle;
}

W3dModelHandle W3dAssetCache::requestAsync(
    container::StringView logicalPrototype, bool queueUpload,
    RenderAssetPriority priority) {
    W3dAssetRequest requestDescription;
    requestDescription.source = logicalPrototype;
    requestDescription.queueGpuUpload = queueUpload;
    requestDescription.gpuUploadPriority = priority;
    return requestAsync(requestDescription);
}

bool W3dAssetCache::contains(W3dModelHandle handle) const noexcept {
    return findSlot(handle) != nullptr;
}

std::optional<W3dAssetState> W3dAssetCache::state(W3dModelHandle handle) const noexcept {
    const Slot* slot = findSlot(handle);
    return slot ? std::optional<W3dAssetState>(slot->state) : std::nullopt;
}

uint64_t W3dAssetCache::revision(W3dModelHandle handle) const noexcept {
    const Slot* slot = findSlot(handle);
    return slot ? slot->revision : 0;
}

std::optional<W3dAssetVersion> W3dAssetCache::version(
    W3dModelHandle handle) const noexcept {
    const Slot* slot = findSlot(handle);
    if (!slot || slot->revision == 0) return std::nullopt;
    return W3dAssetVersion{.handle = handle, .revision = slot->revision};
}

bool W3dAssetCache::matches(W3dAssetVersion expected) const noexcept {
    if (!expected) return false;
    const Slot* slot = findSlot(expected.handle);
    return slot && slot->revision == expected.revision;
}

container::SharedPtr<const CpuStaticModel> W3dAssetCache::cpuModel(W3dModelHandle handle) const {
    const Slot* slot = findSlot(handle);
    return slot ? slot->cpuModel : nullptr;
}

container::SharedPtr<const W3dGpuModel> W3dAssetCache::gpuModel(W3dModelHandle handle) {
    Slot* slot = findSlot(handle);
    if (slot) slot->lastReachableFrame = m_residencyFrame;
    return slot ? slot->gpuModel : nullptr;
}

container::String W3dAssetCache::error(W3dModelHandle handle) const {
    const Slot* slot = findSlot(handle);
    return slot ? slot->error : "stale or invalid W3D model handle";
}

container::String W3dAssetCache::sourcePath(W3dModelHandle handle) const {
    const Slot* slot = findSlot(handle);
    return slot ? slot->key.sourcePath : container::String{};
}

container::String W3dAssetCache::prototype(W3dModelHandle handle) const {
    const Slot* slot = findSlot(handle);
    return slot ? slot->key.prototype : container::String{};
}

std::optional<W3dAssetDependencies> W3dAssetCache::dependencies(
    W3dModelHandle handle) const {
    const Slot* slot = findSlot(handle);
    return slot ? std::optional<W3dAssetDependencies>{slot->dependencies}
                : std::nullopt;
}

void W3dAssetCache::recordAnimationDependency(
    W3dModelHandle handle,
    container::StringView logicalName,
    container::StringView hierarchyName,
    container::StringView sourcePath,
    uint64_t revision,
    bool ready,
    container::StringView diagnostic) {
    Slot* slot = findSlot(handle);
    if (!slot || logicalName.empty() || sourcePath.empty()) return;

    W3dAssetDependencies& dependencies = slot->dependencies;
    if (dependencies.nodes.empty()) return;
    const container::String identity = lowerAscii(
        container::String(sourcePath) + "#" + container::String(logicalName));
    auto existing = std::find_if(
        dependencies.nodes.begin(), dependencies.nodes.end(),
        [&identity](const W3dDependencyNode& node) {
            return node.kind == W3dDependencyKind::Animation &&
                   node.canonicalIdentity == identity;
        });
    if (existing == dependencies.nodes.end()) {
        const uint32_t animationNode = appendDependencyNode(
            dependencies, W3dDependencyKind::Animation,
            ready ? W3dDependencyState::Ready
                  : diagnostic.empty() ? W3dDependencyState::Referenced
                                       : W3dDependencyState::Missing,
            container::String(logicalName), identity, revision);
        uint32_t sourceNode = 0;
        for (uint32_t index = 0; index < dependencies.nodes.size(); ++index) {
            if (dependencies.nodes[index].kind == W3dDependencyKind::Hierarchy) {
                sourceNode = index;
                break;
            }
        }
        appendDependencyEdge(
            dependencies, sourceNode, animationNode,
            RenderAssetDependencyPolicy::Optional);
        uint32_t& references = m_dependencyTargetReferences[identity];
        if (references != std::numeric_limits<uint32_t>::max()) {
            ++references;
        }
        existing = dependencies.nodes.begin() + animationNode;
    } else {
        existing->state = ready ? W3dDependencyState::Ready
                                : diagnostic.empty()
                                    ? W3dDependencyState::Referenced
                                    : W3dDependencyState::Missing;
        existing->revision = revision;
        existing->logicalName = container::String(logicalName);
    }
    if (!hierarchyName.empty() &&
        !equalAsciiInsensitive(hierarchyName, slot->dependencies.hierarchyName)) {
        existing->state = W3dDependencyState::Fallback;
    }
    updateDependencyTargetResolution(
        dependencies,
        static_cast<uint32_t>(existing - dependencies.nodes.begin()));
    if (!diagnostic.empty() &&
        std::find(existing->diagnostics.begin(), existing->diagnostics.end(),
                  diagnostic) == existing->diagnostics.end()) {
        existing->diagnostics.emplace_back(diagnostic);
        if (std::find(dependencies.diagnostics.begin(),
                      dependencies.diagnostics.end(), diagnostic) ==
            dependencies.diagnostics.end()) {
            dependencies.diagnostics.emplace_back(diagnostic);
        }
    }
    if (std::find(dependencies.animations.begin(), dependencies.animations.end(),
                  logicalName) == dependencies.animations.end()) {
        dependencies.animations.emplace_back(logicalName);
    }
}

void W3dAssetCache::recordTextureDependency(
    W3dModelHandle handle,
    container::StringView textureName,
    bool resident) {
    Slot* slot = findSlot(handle);
    if (!slot || textureName.empty()) return;
    const container::String identity =
        canonicalTextureIdentity(textureName);
    const auto node = std::find_if(
        slot->dependencies.nodes.begin(),
        slot->dependencies.nodes.end(),
        [&identity](const W3dDependencyNode& candidate) {
            return candidate.kind == W3dDependencyKind::Texture &&
                candidate.canonicalIdentity == identity;
        });
    if (node == slot->dependencies.nodes.end()) return;
    node->state = resident
        ? W3dDependencyState::Ready
        : W3dDependencyState::Fallback;
    updateDependencyTargetResolution(
        slot->dependencies,
        static_cast<uint32_t>(
            node - slot->dependencies.nodes.begin()));
}

bool W3dAssetCache::reload(W3dModelHandle handle, bool queueUpload) {
    Slot* slot = findSlot(handle);
    if (!slot) return false;
    ++m_reloads;

    container::HashSet<container::String> invalidatedFiles{slot->key.sourcePath};
    if (!slot->dependencies.hierarchySourcePath.empty()) {
        invalidatedFiles.insert(slot->dependencies.hierarchySourcePath);
    }
    container::HashSet<uint32_t> affectedSlots{handle.index};
    for (const container::String& path : invalidatedFiles) {
        if (const auto found = m_fileDependents.find(path);
            found != m_fileDependents.end()) {
            affectedSlots.insert(found->second.begin(), found->second.end());
        }
        container::String fileCacheKey = path;
        if (const auto locator = io::acquireLocaleResourceLocator()) {
            if (const std::optional<container::String> resolved =
                    locator->resolve(io::LocaleResourceKind::W3d, path)) {
                fileCacheKey = *resolved;
            }
        }
        if (auto file = m_fileCache.find(fileCacheKey);
            file != m_fileCache.end()) {
            file->second.parsed.reset();
            file->second.error.clear();
            file->second.errorKind = RenderAssetErrorKind::None;
            ++file->second.revision;
            if (file->second.revision == 0) ++file->second.revision;
        }
    }

    struct ReloadSlot {
        uint32_t index = 0;
        bool queueUpload = false;
    };
    container::Vector<ReloadSlot> reloadSlots;
    reloadSlots.reserve(affectedSlots.size());
    for (uint32_t index : affectedSlots) {
        if (index >= m_slots.size() || !m_slots[index].occupied) continue;
        Slot& affected = m_slots[index];
        const W3dModelHandle affectedHandle{index, affected.generation};
        const bool restoreUpload = index == handle.index
            ? queueUpload
            : affected.state == W3dAssetState::UploadQueued ||
              affected.state == W3dAssetState::Uploading ||
              affected.state == W3dAssetState::GpuReady ||
              affected.state == W3dAssetState::GpuUploadFailed;
        reloadSlots.push_back({index, restoreUpload});
        discardPendingCpuLoads(affectedHandle);
        discardPendingUploads(affectedHandle);
        unregisterFileDependencies(index);
        unregisterDependencyGraph(affected.dependencies);
        retireGpuModel(std::move(affected.gpuModel));
        affected.residentSinceFrame = 0;
        affected.cpuModel.reset();
        affected.dependencies = {};
        affected.error.clear();
        affected.errorKind = RenderAssetErrorKind::None;
        affected.queueGpuAfterCpuLoad = false;
        affected.queueGpuAfterCpuLoadPriority =
            W3dGpuUploadPriority::Normal;
        ++affected.revision;
        if (affected.revision == 0) ++affected.revision;
    }

    bool targetLoaded = false;
    for (const ReloadSlot& reloadSlot : reloadSlots) {
        const bool loaded = loadCpuAsset(reloadSlot.index);
        if (reloadSlot.index == handle.index) targetLoaded = loaded;
        if (loaded && reloadSlot.queueUpload) {
            Slot& affected = m_slots[reloadSlot.index];
            queueGpuUpload({reloadSlot.index, affected.generation});
        }
    }
    return targetLoaded;
}

bool W3dAssetCache::evict(W3dModelHandle handle) {
    Slot* slot = findSlot(handle);
    if (!slot) return false;
    ++m_evictions;

    const uint32_t index = handle.index;
    discardPendingCpuLoads(handle);
    discardPendingUploads(handle);
    unregisterFileDependencies(index);
    unregisterDependencyGraph(slot->dependencies);
    m_modelLookup.erase(slot->key);
    retireGpuModel(std::move(slot->gpuModel));
    slot->cpuModel.reset();
    slot->dependencies = {};
    slot->error.clear();
    slot->errorKind = RenderAssetErrorKind::None;
    slot->queueGpuAfterCpuLoad = false;
    slot->queueGpuAfterCpuLoadPriority = W3dGpuUploadPriority::Normal;
    slot->residencyPinMask = 0;
    slot->residentSinceFrame = 0;
    slot->lastReachableFrame = 0;
    slot->key = {};
    slot->revision = 0;
    slot->occupied = false;
    slot->state = W3dAssetState::Failed;
    slot->generation = nextGeneration(slot->generation);
    m_freeSlots.push_back(index);
    return true;
}

void W3dAssetCache::clear() {
    ++m_resets;
    ++m_cpuLoadSession;
    if (m_cpuLoadSession == 0u) ++m_cpuLoadSession;
    if (engine::resource::ResourceSchedulerRuntime* scheduler =
            engine::resource::activeResourceSchedulerRuntime()) {
        for (const CpuLoadTask& task : m_cpuLoadTasks) {
            static_cast<void>(scheduler->cancel(task.ticket));
        }
    }
    m_modelLookup.clear();
    m_fileCache.clear();
    m_fileDependents.clear();
    m_dependencyTargetReferences.clear();
    m_uploadQueue.clear();
    m_cancelledQueuedCpuLoadJobs.fetch_add(
        m_cpuLoadJobs.size(), std::memory_order_relaxed);
    m_cancelledPendingCpuLoadCompletions.fetch_add(
        m_cpuLoadCompletions.size(), std::memory_order_relaxed);
    m_cpuLoadJobs.clear();
    m_cpuLoadCompletions.clear();
    m_cpuLoadPriorities.clear();
    m_freeSlots.clear();
    m_freeSlots.reserve(m_slots.size());

    for (uint32_t index = 0; index < m_slots.size(); ++index) {
        Slot& slot = m_slots[index];
        retireGpuModel(std::move(slot.gpuModel));
        slot.cpuModel.reset();
        slot.dependencies = {};
        slot.error.clear();
        slot.errorKind = RenderAssetErrorKind::None;
        slot.queueGpuAfterCpuLoad = false;
        slot.queueGpuAfterCpuLoadPriority = W3dGpuUploadPriority::Normal;
        slot.residencyPinMask = 0;
        slot.residentSinceFrame = 0;
        slot.lastReachableFrame = 0;
        slot.key = {};
        slot.revision = 0;
        slot.occupied = false;
        slot.state = W3dAssetState::Failed;
        slot.generation = nextGeneration(slot.generation);
        m_freeSlots.push_back(index);
    }
    m_uploadWindowOpen = false;
    m_nextUploadSequence = 1;
    m_lastUploadBatch = {};
    m_lifetimeUploadTotals = {};
    m_residencyFrame = 0;
}

W3dAssetCache::Slot* W3dAssetCache::findSlot(W3dModelHandle handle) noexcept {
    if (!handle.valid() || handle.index >= m_slots.size()) return nullptr;
    Slot& slot = m_slots[handle.index];
    return slot.occupied && slot.generation == handle.generation ? &slot : nullptr;
}

const W3dAssetCache::Slot* W3dAssetCache::findSlot(W3dModelHandle handle) const noexcept {
    if (!handle.valid() || handle.index >= m_slots.size()) return nullptr;
    const Slot& slot = m_slots[handle.index];
    return slot.occupied && slot.generation == handle.generation ? &slot : nullptr;
}

W3dModelHandle W3dAssetCache::allocateSlot(ModelKey key) {
    uint32_t index = 0;
    if (m_freeSlots.empty()) {
        index = static_cast<uint32_t>(m_slots.size());
        m_slots.emplace_back();
    } else {
        index = m_freeSlots.back();
        m_freeSlots.pop_back();
    }

    Slot& slot = m_slots[index];
    slot.key = std::move(key);
    slot.revision = 1;
    slot.occupied = true;
    slot.state = W3dAssetState::Failed;
    slot.cpuModel.reset();
    slot.gpuModel.reset();
    slot.dependencies = {};
    slot.error.clear();
    slot.errorKind = RenderAssetErrorKind::None;
    slot.queueGpuAfterCpuLoad = false;
    slot.queueGpuAfterCpuLoadPriority = W3dGpuUploadPriority::Normal;
    m_modelLookup.emplace(slot.key, index);
    return {index, slot.generation};
}

container::SharedPtr<const data::w3d::ParsedW3D> W3dAssetCache::loadParsedFile(
    container::StringView sourcePath, container::String& error,
    uint64_t* revision, RenderAssetErrorKind* errorKind) {
    error.clear();
    if (errorKind) *errorKind = RenderAssetErrorKind::None;
    container::String key(sourcePath);
    bool sourceAvailable = m_vfs != nullptr;
    if (const auto locator = io::acquireLocaleResourceLocator()) {
        const std::optional<container::String> resolved = locator->resolve(
            io::LocaleResourceKind::W3d, sourcePath);
        sourceAvailable = resolved.has_value();
        if (resolved) key = *resolved;
    }
    auto [found, inserted] = m_fileCache.try_emplace(key);
    ParsedFileNode& node = found->second;
    if (revision) *revision = node.revision;
    if (!inserted) {
        if (node.parsed) return node.parsed;
        if (!node.error.empty()) {
            error = node.error;
            if (errorKind) *errorKind = node.errorKind;
            return nullptr;
        }
    }

    container::Vector<uint8_t> bytes;
    if (!sourceAvailable || !m_vfs || !m_vfs->readToBuffer(key, bytes)) {
        node.error = "W3D VFS read failed: " + key;
        node.errorKind = RenderAssetErrorKind::Io;
        error = node.error;
        if (errorKind) *errorKind = node.errorKind;
        return nullptr;
    }
    if (bytes.empty()) {
        node.error = "W3D VFS file is empty: " + key;
        node.errorKind = RenderAssetErrorKind::Io;
        error = node.error;
        if (errorKind) *errorKind = node.errorKind;
        return nullptr;
    }

    data::w3d::W3dLoader loader;
    if (!loader.loadFromMemory(bytes.data(), bytes.size())) {
        node.error = "W3D parse failed for " + key + ": " + loader.error();
        node.errorKind = RenderAssetErrorKind::Parse;
        error = node.error;
        if (errorKind) *errorKind = node.errorKind;
        return nullptr;
    }
    node.parsed = std::make_shared<const data::w3d::ParsedW3D>(loader.takeResult());
    node.error.clear();
    node.errorKind = RenderAssetErrorKind::None;
    return node.parsed;
}

bool W3dAssetCache::loadCpuAsset(uint32_t slotIndex) {
    if (slotIndex >= m_slots.size() || !m_slots[slotIndex].occupied) return false;
    Slot& slot = m_slots[slotIndex];
    unregisterFileDependencies(slotIndex);
    unregisterDependencyGraph(slot.dependencies);
    slot.dependencies = {};
    slot.dependencies.modelSourcePath = slot.key.sourcePath;
    slot.errorKind = RenderAssetErrorKind::None;
    container::String parseError;
    RenderAssetErrorKind parseErrorKind = RenderAssetErrorKind::None;
    uint64_t modelFileRevision = 0;
    const container::SharedPtr<const data::w3d::ParsedW3D> parsed =
        loadParsedFile(
            slot.key.sourcePath, parseError, &modelFileRevision,
            &parseErrorKind);
    registerFileDependency(slotIndex, slot.key.sourcePath);
    const uint32_t modelNode = appendDependencyNode(
        slot.dependencies, W3dDependencyKind::Model,
        parsed ? W3dDependencyState::Ready : W3dDependencyState::Missing,
        slot.key.prototype, slot.key.sourcePath, modelFileRevision);
    if (!parsed) {
        slot.dependencies.nodes[modelNode].errorKind = parseErrorKind;
        slot.dependencies.nodes[modelNode].diagnostics.push_back(parseError);
        slot.dependencies.diagnostics.push_back(parseError);
        slot.state = W3dAssetState::Failed;
        slot.error = std::move(parseError);
        slot.errorKind = parseErrorKind;
        ++m_failedSynchronousCpuLoads;
        registerDependencyGraph(slot.dependencies);
        return false;
    }

    W3dStaticModelBuildOptions options;
    options.requestedPrototype = slot.key.prototype;
    options.includeHiddenMeshes = slot.key.includeHiddenMeshes;
    options.includeCollisionMeshes = slot.key.includeCollisionMeshes;
    slot.dependencies.hierarchyName =
        W3dStaticModelBuilder::requiredHierarchyName(*parsed, options);
    container::SharedPtr<const data::w3d::ParsedW3D> externalHierarchy;
    uint64_t hierarchyFileRevision = modelFileRevision;
    uint32_t hierarchyNode = UINT32_MAX;
    if (!slot.dependencies.hierarchyName.empty() &&
        !containsHierarchy(*parsed, slot.dependencies.hierarchyName)) {
        slot.dependencies.externalHierarchyRequired = true;
        slot.dependencies.hierarchySourcePath =
            data::w3d::w3dHierarchySourcePath(
                slot.dependencies.hierarchyName);
        registerFileDependency(slotIndex, slot.dependencies.hierarchySourcePath);
        container::String hierarchyError;
        RenderAssetErrorKind hierarchyErrorKind =
            RenderAssetErrorKind::None;
        externalHierarchy = loadParsedFile(
            slot.dependencies.hierarchySourcePath, hierarchyError,
            &hierarchyFileRevision, &hierarchyErrorKind);
        hierarchyNode = appendDependencyNode(
            slot.dependencies, W3dDependencyKind::Hierarchy,
            externalHierarchy ? W3dDependencyState::Ready
                              : W3dDependencyState::Missing,
            slot.dependencies.hierarchyName,
            slot.dependencies.hierarchySourcePath,
            hierarchyFileRevision);
        appendDependencyEdge(
            slot.dependencies, modelNode, hierarchyNode,
            RenderAssetDependencyPolicy::FallbackAllowed);
        if (externalHierarchy) {
            options.externalHierarchySource = externalHierarchy.get();
        } else {
            slot.dependencies.nodes[hierarchyNode].errorKind =
                hierarchyErrorKind;
            slot.dependencies.diagnostics.push_back(std::move(hierarchyError));
            slot.dependencies.nodes[hierarchyNode].diagnostics.push_back(
                slot.dependencies.diagnostics.back());
        }
    } else if (!slot.dependencies.hierarchyName.empty()) {
        slot.dependencies.hierarchySourcePath = slot.key.sourcePath;
        hierarchyNode = appendDependencyNode(
            slot.dependencies, W3dDependencyKind::Hierarchy,
            W3dDependencyState::Ready,
            slot.dependencies.hierarchyName, slot.key.sourcePath,
            modelFileRevision);
        appendDependencyEdge(
            slot.dependencies, modelNode, hierarchyNode,
            RenderAssetDependencyPolicy::FallbackAllowed);
    }
    container::String buildError;
    auto model = W3dStaticModelBuilder::build(*parsed, options, &buildError);
    if (!model) {
        slot.state = W3dAssetState::Failed;
        slot.error = "W3D static model build failed for " + slot.key.prototype + " in " +
            slot.key.sourcePath + ": " + buildError;
        slot.errorKind = RenderAssetErrorKind::Build;
        ++m_failedSynchronousCpuLoads;
        registerDependencyGraph(slot.dependencies);
        return false;
    }

    slot.dependencies.externalHierarchyResolved =
        model->externalHierarchyResolved;
    slot.dependencies.skeletonFallback = model->skeletonFallback;
    if (hierarchyNode != UINT32_MAX && model->skeletonFallback &&
        slot.dependencies.nodes[hierarchyNode].state ==
            W3dDependencyState::Missing) {
        slot.dependencies.nodes[hierarchyNode].state =
            W3dDependencyState::Fallback;
        updateDependencyTargetResolution(
            slot.dependencies, hierarchyNode);
    }
    for (const CpuStaticModel::Animation& animation : model->animations) {
        if (!animation.name.empty()) {
            slot.dependencies.animations.push_back(animation.name);
            const uint32_t animationNode = appendDependencyNode(
                slot.dependencies, W3dDependencyKind::Animation,
                W3dDependencyState::Ready, animation.name,
                container::String{"animation:"} + lowerAscii(
                    slot.dependencies.hierarchyName + "." + animation.name),
                hierarchyFileRevision);
            appendDependencyEdge(
                slot.dependencies,
                hierarchyNode != UINT32_MAX ? hierarchyNode : modelNode,
                animationNode,
                RenderAssetDependencyPolicy::Optional);
        }
    }
    for (const StaticMaterialDesc& material : model->materials) {
        const auto addTexture = [
            &slot, modelNode](
            const container::String& texture,
            RenderAssetDependencyPolicy policy) {
            if (texture.empty()) return;
            const container::String identity =
                canonicalTextureIdentity(texture);
            const auto existingNode = std::find_if(
                slot.dependencies.nodes.begin(),
                slot.dependencies.nodes.end(),
                [&identity](const W3dDependencyNode& node) {
                    return node.kind == W3dDependencyKind::Texture &&
                        node.canonicalIdentity == identity;
                });
            if (existingNode != slot.dependencies.nodes.end()) {
                const uint32_t targetNode = static_cast<uint32_t>(
                    existingNode - slot.dependencies.nodes.begin());
                if (policy ==
                    RenderAssetDependencyPolicy::FallbackAllowed) {
                    for (W3dDependencyEdge& edge :
                         slot.dependencies.edges) {
                        if (edge.sourceNode == modelNode &&
                            edge.targetNode == targetNode) {
                            edge.policy = policy;
                        }
                    }
                }
                return;
            }
            slot.dependencies.textures.push_back(texture);
            const uint32_t textureNode = appendDependencyNode(
                slot.dependencies, W3dDependencyKind::Texture,
                W3dDependencyState::Referenced, texture,
                identity);
            appendDependencyEdge(
                slot.dependencies, modelNode, textureNode, policy);
        };
        addTexture(
            material.textureName,
            RenderAssetDependencyPolicy::FallbackAllowed);
        addTexture(
            material.detailTextureName,
            RenderAssetDependencyPolicy::Optional);
    }
    slot.dependencies.diagnostics.insert(
        slot.dependencies.diagnostics.end(),
        model->diagnostics.begin(), model->diagnostics.end());
    for (const container::String& diagnostic : slot.dependencies.diagnostics) {
        TD_LOG_WARN("[W3dAssetCache] '{}' dependency: {}",
                    slot.key.prototype, diagnostic);
    }

    slot.cpuModel = std::make_shared<const CpuStaticModel>(std::move(*model));
    slot.state = W3dAssetState::CpuReady;
    slot.error.clear();
    slot.errorKind = RenderAssetErrorKind::None;
    registerDependencyGraph(slot.dependencies);
    return true;
}

void W3dAssetCache::unregisterFileDependencies(uint32_t slotIndex) {
    for (auto dependency = m_fileDependents.begin();
         dependency != m_fileDependents.end();) {
        dependency->second.erase(slotIndex);
        if (dependency->second.empty()) {
            dependency = m_fileDependents.erase(dependency);
        } else {
            ++dependency;
        }
    }
}

void W3dAssetCache::registerFileDependency(
    uint32_t slotIndex, container::StringView canonicalPath) {
    if (canonicalPath.empty()) return;
    m_fileDependents[container::String(canonicalPath)].insert(slotIndex);
}

void W3dAssetCache::registerDependencyGraph(
    const W3dAssetDependencies& dependencies) {
    container::HashSet<container::String> registered;
    registered.reserve(dependencies.nodes.size());
    for (const W3dDependencyNode& node : dependencies.nodes) {
        if (node.canonicalIdentity.empty() ||
            !registered.insert(node.canonicalIdentity).second) {
            continue;
        }
        uint32_t& references =
            m_dependencyTargetReferences[node.canonicalIdentity];
        if (references != std::numeric_limits<uint32_t>::max()) {
            ++references;
        }
    }
}

void W3dAssetCache::unregisterDependencyGraph(
    const W3dAssetDependencies& dependencies) {
    container::HashSet<container::String> unregistered;
    unregistered.reserve(dependencies.nodes.size());
    for (const W3dDependencyNode& node : dependencies.nodes) {
        if (node.canonicalIdentity.empty() ||
            !unregistered.insert(node.canonicalIdentity).second) {
            continue;
        }
        const auto found = m_dependencyTargetReferences.find(
            node.canonicalIdentity);
        if (found == m_dependencyTargetReferences.end()) continue;
        if (found->second > 1u) --found->second;
        else m_dependencyTargetReferences.erase(found);
    }
}

void W3dAssetCache::discardPendingUploads(W3dModelHandle handle) {
    std::erase_if(m_uploadQueue, [handle](const PendingUpload& pending) {
        return pending.handle == handle;
    });
}

void W3dAssetCache::retireGpuModel(container::SharedPtr<const W3dGpuModel> model) {
    if (!model) return;
    if (m_retire) m_retire(std::move(model));
}

uint32_t W3dAssetCache::nextGeneration(uint32_t generation) noexcept {
    ++generation;
    return generation == 0 ? 1 : generation;
}

} // namespace engine::render

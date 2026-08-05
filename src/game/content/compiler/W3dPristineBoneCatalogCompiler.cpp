#include "core/container/hash_containers.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingFactory.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "core/io/VFS.h"
#include "core/io/LocaleResourceLocator.h"
#include "core/platform/runtime_threads.h"
#include "data/w3d/W3dAssetIdentity.h"
#include "data/w3d/W3dLoader.h"
#include "game/object/plan/combat/ObjectBoneFxPlanTypes.h"
#include "game/object/plan/structure/ObjectAirfieldPlanTypes.h"
#include "game/object/plan/economy/ObjectProductionPlanTypes.h"
#include "game/object/plan/combat/ObjectTransitionDamageFxPlanTypes.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace game {
namespace {

[[nodiscard]] container::String lowerAscii(container::StringView value) {
    container::String result{value};
    for (char& character : result) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character + ('a' - 'A'));
        }
    }
    return result;
}

[[nodiscard]] container::String boundedName(const char* data,
                                             size_t capacity) {
    size_t length = 0;
    while (length < capacity && data[length] != '\0') ++length;
    return container::String{data, length};
}

[[nodiscard]] bool hasAuthoritativeBoneRequest(
    const ObjectArchetype& archetype) {
    if (archetype.airfieldPlan &&
        (!archetype.airfieldPlan->parkingPlaces.empty() ||
         !archetype.airfieldPlan->flightDecks.empty())) {
        // JetAIUpdate/ParkingPlace/FlightDeck use logical runway, taxi,
        // creation and parking bones for authoritative movement.  Loading
        // the pristine pose is therefore required even for an unarmed
        // airfield whose Draw recipe has no weapon-bone consumer.
        return true;
    }
    if (archetype.productionExitPlan &&
        archetype.productionExitPlan->kind ==
            ObjectProductionExitKind::SpawnPoint &&
        !archetype.productionExitPlan->spawnPointBoneName.empty()) {
        return true;
    }
    for (const ModelConditionVisualRule& visual :
         archetype.templateData.modelConditionVisuals) {
        for (const ModelWeaponBoneDefinition& weapon : visual.weaponBones) {
            if (!weapon.fireFxBone.empty() || !weapon.recoilBone.empty() ||
                !weapon.muzzleFlash.empty() || !weapon.launchBone.empty()) {
                return true;
            }
        }
    }
    for (const ModelConditionTransitionRule& visual :
         archetype.templateData.modelConditionTransitions) {
        for (const ModelWeaponBoneDefinition& weapon : visual.weaponBones) {
            if (!weapon.fireFxBone.empty() || !weapon.recoilBone.empty() ||
                !weapon.muzzleFlash.empty() || !weapon.launchBone.empty()) {
                return true;
            }
        }
    }
    for (const ModelDrawVisualChannel& channel :
         archetype.templateData.drawVisualChannels) {
        if (!channel.attachToBoneInAnotherModule.empty() ||
            !channel.attachToBoneInContainer.empty()) {
            return true;
        }
    }
    if (archetype.boneFxPlan) {
        for (const ObjectBoneFxRule& rule : archetype.boneFxPlan->rules) {
            for (const auto& state : rule.entries) {
                if (std::any_of(state.begin(), state.end(),
                        [](const ObjectBoneFxEntry& entry) {
                            return entry.kind ==
                                    ObjectBoneFxPayloadKind::ObjectCreationList &&
                                !entry.resource.empty();
                        })) {
                    return true;
                }
            }
        }
    }
    if (archetype.transitionDamageFxPlan) {
        for (const ObjectTransitionDamageFxRule& rule :
             archetype.transitionDamageFxPlan->rules) {
            for (const auto& state : rule.entries) {
                if (std::any_of(state.begin(), state.end(),
                        [](const ObjectTransitionDamageFxEntry& entry) {
                            return entry.kind ==
                                    ObjectTransitionDamageFxPayloadKind::
                                        ObjectCreationList &&
                                entry.location.kind ==
                                    ObjectTransitionDamageFxLocationKind::Bone &&
                                !entry.resource.empty();
                        })) {
                    return true;
                }
            }
        }
    }
    return false;
}

[[nodiscard]] const data::w3d::ParsedHLod* findHlod(
    const data::w3d::ParsedW3D& parsed, container::StringView prototype) {
    const container::String wanted = lowerAscii(prototype);
    for (const data::w3d::ParsedHLod& hlod : parsed.hlods) {
        if (lowerAscii(boundedName(hlod.name, data::w3d::NAME_LEN)) == wanted &&
            hlod.highestDetailLod()) {
            return &hlod;
        }
    }
    return nullptr;
}

[[nodiscard]] const data::w3d::ParsedHierarchy* findHierarchy(
    const data::w3d::ParsedW3D& parsed, container::StringView name) {
    const container::String wanted = lowerAscii(name);
    const data::w3d::ParsedHierarchy* result = nullptr;
    for (const data::w3d::ParsedHierarchy& hierarchy : parsed.hierarchies) {
        if (lowerAscii(boundedName(hierarchy.name, data::w3d::NAME_LEN)) !=
            wanted) {
            continue;
        }
        if (result) return nullptr; // ambiguous legacy content: root fallback
        result = &hierarchy;
    }
    return result;
}

[[nodiscard]] const data::w3d::ParsedAnimation* findAnimation(
    const data::w3d::ParsedW3D& parsed, container::StringView logicalName,
    container::StringView hierarchyName) {
    const container::String logical = lowerAscii(logicalName);
    const container::String wantedHierarchy = lowerAscii(hierarchyName);
    const size_t dot = logical.find('.');
    const container::String clip = dot == container::String::npos
        ? logical : logical.substr(dot + 1);
    for (const data::w3d::ParsedAnimation& animation : parsed.animations) {
        const container::String hierarchy = lowerAscii(boundedName(
            animation.hierarchyName, data::w3d::NAME_LEN));
        const container::String name = lowerAscii(boundedName(
            animation.name, data::w3d::NAME_LEN));
        if (hierarchy != wantedHierarchy) continue;
        if (name == clip || hierarchy + "." + name == logical) {
            return &animation;
        }
    }
    return nullptr;
}

struct SourceFile final {
    container::String path;
    container::SharedPtr<const data::w3d::ParsedW3D> parsed;
    uint64_t byteFingerprint = 14695981039346656037ull;
    uint64_t byteSize = 0;
};

void fingerprintByte(uint64_t& hash, uint8_t value) noexcept {
    hash ^= value;
    hash *= 1099511628211ull;
}

void fingerprintU64(uint64_t& hash, uint64_t value) noexcept {
    for (uint32_t shift = 0; shift < 64; shift += 8) {
        fingerprintByte(hash,
            static_cast<uint8_t>((value >> shift) & 0xffu));
    }
}

void fingerprintString(uint64_t& hash, container::StringView value) noexcept {
    fingerprintU64(hash, value.size());
    for (const unsigned char character : value) {
        fingerprintByte(hash, character);
    }
}

[[nodiscard]] bool readAndFingerprint(
    container::StringView path, container::Vector<uint8_t>& bytes,
    uint64_t& fingerprint, uint64_t& byteSize) {
    container::UniquePtr<io::File> file;
    if (!io::VFS::instance().open(path, file) || !file) return false;
    const int64_t signedSize = file->size();
    if (signedSize <= 0 ||
        static_cast<uint64_t>(signedSize) >
            static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return signedSize == 0;
    }

    const size_t size = static_cast<size_t>(signedSize);
    bytes.resize(size);
    fingerprint = 14695981039346656037ull;
    byteSize = size;
    constexpr size_t kReadChunkSize = 256u * 1024u;
    size_t offset = 0;
    while (offset < size) {
        const size_t requested = std::min(kReadChunkSize, size - offset);
        const size_t received = file->read(bytes.data() + offset, requested);
        if (received == 0 || received > requested) return false;
        for (size_t index = 0; index < received; ++index) {
            fingerprintByte(fingerprint, bytes[offset + index]);
        }
        offset += received;
    }
    return true;
}

class SourceCache final {
public:
    [[nodiscard]] const SourceFile* load(container::StringView path) {
        container::String key = lowerAscii(path);
        bool sourceAvailable = true;
        if (const auto locator = io::acquireLocaleResourceLocator()) {
            const std::optional<container::String> resolved = locator->resolve(
                io::LocaleResourceKind::W3d, key);
            sourceAvailable = resolved.has_value();
            if (resolved) key = *resolved;
        }
        if (const auto found = m_files.find(key); found != m_files.end()) {
            return found->second.parsed ? &found->second : nullptr;
        }
        SourceFile file;
        file.path = key;
        container::Vector<uint8_t> bytes;
        if (!sourceAvailable ||
            !readAndFingerprint(key, bytes, file.byteFingerprint,
                                file.byteSize) ||
            bytes.empty()) {
            m_files.emplace(key, std::move(file));
            return nullptr;
        }
        data::w3d::W3dLoader loader;
        if (!loader.loadFromMemory(bytes.data(), bytes.size())) {
            m_files.emplace(key, std::move(file));
            return nullptr;
        }
        file.parsed = std::make_shared<const data::w3d::ParsedW3D>(
            loader.takeResult());
        auto [stored, inserted] = m_files.emplace(key, std::move(file));
        static_cast<void>(inserted);
        return &stored->second;
    }

    [[nodiscard]] const container::TreeMap<container::String, SourceFile>&
    files() const noexcept {
        return m_files;
    }

private:
    container::TreeMap<container::String, SourceFile> m_files;
};

struct ResolvedModelSource final {
    const SourceFile* file = nullptr;
    const data::w3d::ParsedHLod* hlod = nullptr;
};

[[nodiscard]] std::optional<ResolvedModelSource> resolveModelSource(
    SourceCache& sources, const data::w3d::W3dModelIdentity& identity,
    container::HashMap<container::String, ResolvedModelSource>& resolved,
    container::HashSet<container::String>& unresolved) {
    const container::String resolutionKey =
        identity.sourcePath + "\n" + identity.prototype;
    if (const auto found = resolved.find(resolutionKey);
        found != resolved.end()) {
        return found->second;
    }
    if (unresolved.contains(resolutionKey)) return std::nullopt;

    if (const SourceFile* direct = sources.load(identity.sourcePath)) {
        if (const data::w3d::ParsedHLod* hlod =
                findHlod(*direct->parsed, identity.prototype)) {
            const ResolvedModelSource result{.file = direct, .hlod = hlod};
            resolved.emplace(resolutionKey, result);
            return result;
        }
    }

    container::Vector<container::String> candidates;
    container::HashSet<container::String> emitted;
    const auto append = [&](container::String path) {
        path = lowerAscii(path);
        if (!path.empty() && emitted.insert(path).second) {
            candidates.push_back(std::move(path));
        }
    };
    emitted.insert(identity.sourcePath);

    const size_t slash = identity.sourcePath.find_last_of("/\\");
    const container::String root = slash == container::String::npos
        ? container::String{} : identity.sourcePath.substr(0, slash + 1);
    const container::String filename = slash == container::String::npos
        ? identity.sourcePath : identity.sourcePath.substr(slash + 1);

    // Legacy art commonly stores several named HLODs in a shorter package:
    // Foo_D1B may live in Foo_D1.w3d, and Foo_D/Foo_D1 in Foo.w3d.
    container::String stem = filename;
    if (stem.size() >= data::w3d::kW3dExtension.size() &&
        stem.ends_with(data::w3d::kW3dExtension)) {
        stem.resize(stem.size() - data::w3d::kW3dExtension.size());
    }
    while (stem.size() > 1) {
        stem.pop_back();
        const container::String package = root + stem +
            container::String{data::w3d::kW3dExtension};
        append(package);
    }

    for (const container::String& path : candidates) {
        const SourceFile* file = sources.load(path);
        if (!file) continue;
        if (const data::w3d::ParsedHLod* hlod =
                findHlod(*file->parsed, identity.prototype)) {
            const ResolvedModelSource result{.file = file, .hlod = hlod};
            resolved.emplace(resolutionKey, result);
            return result;
        }
    }
    unresolved.insert(resolutionKey);
    return std::nullopt;
}

[[nodiscard]] container::String poseKey(
    const data::w3d::W3dModelIdentity& model,
    container::StringView animation, bool finalFrame) {
    return model.sourcePath + "\n" + model.prototype + "\n" +
        lowerAscii(animation) + (finalFrame ? "\nfinal" : "\nfirst");
}

struct StagedArchetype final {
    container::String foldedName;
    container::Vector<container::String> poseKeys;
    math::q32_32 assetScale{int32_t{1}};
};

struct SourceFingerprintRecord final {
    uint64_t byteFingerprint = 14695981039346656037ull;
    uint64_t byteSize = 0;
    bool parsed = false;
};

struct CompileBatchResult final {
    container::Vector<StagedArchetype> archetypes;
    container::TreeMap<container::String, W3dPristinePose> poses;
    container::TreeMap<container::String, SourceFingerprintRecord> sources;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] CompileBatchResult compileBatch(
    const ThingFactory& things,
    container::Span<const container::String> names) {
    CompileBatchResult output;
    SourceCache sources;
    container::HashMap<container::String, ResolvedModelSource>
        resolvedModelSources;
    container::HashSet<container::String> unresolvedModelSources;

    output.archetypes.reserve(names.size());
    for (const container::String& name : names) {
        const container::SharedPtr<const ObjectArchetype> archetype =
            things.findArchetype(name);
        if (!archetype || !hasAuthoritativeBoneRequest(*archetype)) continue;

        StagedArchetype binding;
        binding.foldedName = lowerAscii(name);
        binding.assetScale = archetype->templateData.assetScale;
        const size_t normalRuleCount =
            archetype->templateData.modelConditionVisuals.size();
        const size_t transitionRuleCount =
            archetype->templateData.modelConditionTransitions.size();
        binding.poseKeys.resize(normalRuleCount + transitionRuleCount);

        for (size_t ruleIndex = 0;
             ruleIndex < normalRuleCount + transitionRuleCount;
             ++ruleIndex) {
            const bool transition = ruleIndex >= normalRuleCount;
            const size_t sourceRuleIndex = transition
                ? ruleIndex - normalRuleCount : ruleIndex;
            const ModelConditionVisualRule* normalRule = transition
                ? nullptr : &archetype->templateData
                    .modelConditionVisuals[sourceRuleIndex];
            const ModelConditionTransitionRule* transitionRule = transition
                ? &archetype->templateData
                    .modelConditionTransitions[sourceRuleIndex]
                : nullptr;
            const container::String& model = transition
                ? transitionRule->model : normalRule->model;
            if (model.empty()) continue;
            const container::Vector<ModelAnimationCandidate>& candidates =
                transition ? transitionRule->animationCandidates
                           : normalRule->animationCandidates;
            const container::String& authoredAnimation = transition
                ? transitionRule->animation : normalRule->animation;
            const bool finalFrame = transition
                ? (transitionRule->animationFlags & modelAnimationFlagBit(
                       ModelAnimationFlag::PristineBonePositionInFinalFrame)) != 0
                : normalRule->pristineBonePositionInFinalFrame;
            container::String identityError;
            const std::optional<data::w3d::W3dModelIdentity> identity =
                data::w3d::resolveW3dModelIdentity(
                    model, {}, &identityError);
            if (!identity) {
                output.diagnostics.push_back(
                    name + ": invalid pristine model '" + model +
                    "': " + identityError);
                continue;
            }
            const container::String animation = !candidates.empty()
                ? candidates.front().resource : authoredAnimation;
            const container::String key = poseKey(
                *identity, animation, finalFrame);
            if (output.poses.contains(key)) {
                binding.poseKeys[ruleIndex] = key;
                continue;
            }

            const std::optional<ResolvedModelSource> modelSource =
                resolveModelSource(sources, *identity, resolvedModelSources,
                                   unresolvedModelSources);
            if (!modelSource) continue;
            const SourceFile* modelFile = modelSource->file;
            const data::w3d::ParsedHLod* hlod = modelSource->hlod;
            const container::String hierarchyName = boundedName(
                hlod->hierarchyName, data::w3d::NAME_LEN);
            const data::w3d::ParsedHierarchy* hierarchy =
                findHierarchy(*modelFile->parsed, hierarchyName);
            const SourceFile* hierarchyFile = modelFile;
            if (!hierarchy) {
                hierarchyFile = sources.load(
                    data::w3d::w3dHierarchySourcePath(hierarchyName));
                hierarchy = hierarchyFile
                    ? findHierarchy(*hierarchyFile->parsed, hierarchyName)
                    : nullptr;
            }
            if (!hierarchy) continue;

            const data::w3d::ParsedAnimation* selectedAnimation = nullptr;
            if (!animation.empty()) {
                const SourceFile* animationFile = sources.load(
                    data::w3d::w3dAnimationSourcePath(animation));
                selectedAnimation = animationFile
                    ? findAnimation(*animationFile->parsed, animation,
                                    hierarchyName)
                    : nullptr;
                if (!selectedAnimation && hierarchyFile) {
                    selectedAnimation = findAnimation(
                        *hierarchyFile->parsed, animation, hierarchyName);
                }
                if (!selectedAnimation && modelFile != hierarchyFile) {
                    selectedAnimation = findAnimation(
                        *modelFile->parsed, animation, hierarchyName);
                }
            }
            const uint32_t frame = selectedAnimation && finalFrame &&
                    selectedAnimation->numFrames != 0
                ? selectedAnimation->numFrames - 1u : 0u;
            data::w3d::FixedPoseDiagnostics poseDiagnostics;
            const container::Vector<data::w3d::FixedRigidTransform> pose =
                data::w3d::evaluateFixedPristinePose(
                    *hierarchy, selectedAnimation, frame,
                    math::q32_32{int32_t{1}}, &poseDiagnostics);

            container::TreeMap<container::String,
                data::w3d::FixedRigidTransform> entries;
            for (size_t index = 1;
                 index < hierarchy->pivots.size() && index < pose.size();
                 ++index) {
                const container::String bone = lowerAscii(boundedName(
                    hierarchy->pivots[index].name, data::w3d::NAME_LEN));
                if (!bone.empty()) entries.try_emplace(bone, pose[index]);
            }
            if (const data::w3d::ParsedHLodLod* lod =
                    hlod->highestDetailLod()) {
                for (const data::w3d::HLodSubObject& subObject :
                     lod->subObjects) {
                    if (subObject.boneIndex >= pose.size()) continue;
                    const container::String alias = lowerAscii(boundedName(
                        subObject.name, sizeof(subObject.name)));
                    if (!alias.empty()) {
                        entries.try_emplace(alias, pose[subObject.boneIndex]);
                        // WW3D's Get_Sub_Object_By_Name accepts the authored
                        // short object name used by ExtraPublicBone (for
                        // example Dum_Turret), while HLOD records commonly
                        // store Container.Dum_Turret. Preserve both lookup
                        // forms; a real hierarchy bone keeps precedence.
                        const size_t separator = alias.find_last_of('.');
                        if (separator != container::String::npos &&
                            separator + 1u < alias.size()) {
                            entries.try_emplace(
                                alias.substr(separator + 1u),
                                pose[subObject.boneIndex]);
                        }
                    }
                }
            }

            W3dPristinePose compiled;
            compiled.canonicalModelPath = identity->sourcePath;
            compiled.prototype = identity->prototype;
            compiled.animation = lowerAscii(animation);
            compiled.finalFrame = finalFrame;
            compiled.bones.reserve(entries.size());
            for (auto& [bone, transform] : entries) {
                compiled.bones.push_back({
                    .foldedName = bone,
                    .local = transform,
                });
            }
            output.poses.emplace(key, std::move(compiled));
            binding.poseKeys[ruleIndex] = key;
        }
        output.archetypes.push_back(std::move(binding));
    }

    for (const auto& [path, file] : sources.files()) {
        output.sources.emplace(path, SourceFingerprintRecord{
            .byteFingerprint = file.byteFingerprint,
            .byteSize = file.byteSize,
            .parsed = static_cast<bool>(file.parsed),
        });
    }
    return output;
}

} // namespace

bool W3dPristineBoneCatalog::build(
    const ThingFactory& things, container::String* error) {
    clear();
    if (error) error->clear();

    container::Vector<container::String> stableNames;
    stableNames.reserve(things.all().size());
    for (const auto& [name, ignored] : things.all()) {
        static_cast<void>(ignored);
        stableNames.push_back(name);
    }
    std::sort(stableNames.begin(), stableNames.end());

    const size_t workerCount = std::max<size_t>(
        1u, platform::runtime::resourceWorkerCount());
    const size_t taskCount = stableNames.empty()
        ? 0u : std::min(workerCount, stableNames.size());
    const size_t namesPerTask = taskCount == 0
        ? 0u : (stableNames.size() + taskCount - 1u) / taskCount;
    container::Vector<CompileBatchResult> batches(taskCount);
    tf::Taskflow parseTaskflow;
    for (size_t taskIndex = 0; taskIndex < taskCount; ++taskIndex) {
        const size_t begin = taskIndex * namesPerTask;
        const size_t end = std::min(stableNames.size(), begin + namesPerTask);
        parseTaskflow.emplace([&, taskIndex, begin, end] {
            platform::runtime::ThreadRoleScope role(
                platform::runtime::ThreadRole::Resource);
            batches[taskIndex] = compileBatch(
                things,
                container::Span<const container::String>{
                    stableNames.data() + begin, end - begin});
        });
    }
    if (taskCount != 0) {
        platform::runtime::resourceExecutor().run(parseTaskflow).wait();
    }

    // Worker tasks own all VFS reads, W3D parsing and fixed-pose evaluation.
    // Only this caller/owner thread assigns stable pose IDs and publishes the
    // immutable catalog, preserving the original name/rule commit order.
    container::HashMap<container::String, W3dPristinePoseId> poseIds;
    container::TreeMap<container::String, SourceFingerprintRecord>
        fingerprintSources;
    for (CompileBatchResult& batch : batches) {
        m_diagnostics.insert(m_diagnostics.end(),
            std::make_move_iterator(batch.diagnostics.begin()),
            std::make_move_iterator(batch.diagnostics.end()));
        for (const auto& [path, source] : batch.sources) {
            fingerprintSources.try_emplace(path, source);
        }
        for (StagedArchetype& staged : batch.archetypes) {
            ObjectPristinePoseBinding binding;
            binding.assetScale = staged.assetScale;
            binding.poseByVisualRule.resize(staged.poseKeys.size());
            for (size_t ruleIndex = 0;
                 ruleIndex < staged.poseKeys.size(); ++ruleIndex) {
                const container::String& key = staged.poseKeys[ruleIndex];
                if (key.empty()) continue;
                auto known = poseIds.find(key);
                if (known == poseIds.end()) {
                    auto compiled = batch.poses.find(key);
                    if (compiled == batch.poses.end()) continue;
                    m_poses.push_back(std::move(compiled->second));
                    const W3dPristinePoseId id{
                        .value = static_cast<uint32_t>(m_poses.size())};
                    known = poseIds.emplace(key, id).first;
                }
                binding.poseByVisualRule[ruleIndex] = known->second;
            }
            m_bindings.emplace(
                std::move(staged.foldedName), std::move(binding));
        }
    }

    uint64_t sourceFingerprint = 14695981039346656037ull;
    fingerprintString(sourceFingerprint,
        "W3dPristineBoneCatalog.source-fingerprint.v2");
    fingerprintU64(sourceFingerprint, fingerprintSources.size());
    for (const auto& [path, source] : fingerprintSources) {
        fingerprintString(sourceFingerprint, path);
        fingerprintByte(sourceFingerprint, source.parsed ? 1u : 0u);
        fingerprintU64(sourceFingerprint, source.byteSize);
        fingerprintU64(sourceFingerprint, source.byteFingerprint);
    }
    m_sourceFingerprint = sourceFingerprint;
    m_loaded = true;
    return true;
}

} // namespace game

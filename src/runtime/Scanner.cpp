#include "Scanner.hpp"

#include "SignaturePatterns.hpp"
#include "core/memory/Rtti.hpp"

#include <bedrocktools/memory/Signatures.hpp>

#include <array>
#include <atomic>
#include <cstring>
#include <link.h>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace bedrockreplay::scanner {
namespace {

struct Segment {
    const std::uint8_t* start;
    std::size_t size;
};

using bedrocktools::memory::SignatureId;

constexpr std::array<SignatureId, 56> ActiveSignatures{
    SignatureId::VersionString,
    SignatureId::ReplayActorRenderDispatcherRender,
    SignatureId::ReplayLoadAllPackResources,
    SignatureId::ReplayLoadedResourceDtor,
    SignatureId::ReplayBlockCacheComponentData,
    SignatureId::ReplayLevelLoadBlockDefinitions,
    SignatureId::BlockDefinitionGroupRegisterBlocks,
    SignatureId::BlockDefinitionGroupDigestServerBlockProperties,
    SignatureId::BlockTypeRegistryFinalizeForRendering,
    SignatureId::GeometryGroupLoadModelPackFromStringSync,
    SignatureId::ReplayPacksInfoCollectKeys,
    SignatureId::ReplayResourceLocationCtor,
    SignatureId::ReplayMinEngineVersionFromString,
    SignatureId::ReplayInternedStringDtor,
    SignatureId::ReplayBlockFinalizeComponentStorage,
    SignatureId::ReplayLegacyBlockMaterialCtor,
    SignatureId::NormalTick,
    SignatureId::Time,
    SignatureId::SetTime,
    SignatureId::LevelInit,
    SignatureId::LevelDtor,
    SignatureId::ActorManagerList,
    SignatureId::ClientInstanceUpdate,
    SignatureId::ClientInstanceGetMinecraftGame,
    SignatureId::RenderLevel,
    SignatureId::ItemInHandRendererRenderFirstPerson,
    SignatureId::LevelRendererPlayerSetupCamera,
    SignatureId::FrustumCullerSetFromMatrix,
    SignatureId::LevelRendererCameraPreRenderUpdate,
    SignatureId::LevelRendererPlayerUpdateViewArea,
    SignatureId::ClientInstanceGetRenderCameraComponent,
    SignatureId::GetFov,
    SignatureId::LocalPlayerApplyTurnDelta,
    SignatureId::LocalPlayerSwing,
    SignatureId::ActorIsPlayer,
    SignatureId::ActorGetOrCreateUniqueID,
    SignatureId::ActorGetRuntimeID,
    SignatureId::LoopbackPacketSenderSendToServer,
    SignatureId::MinecraftPacketsCreatePacket,
    SignatureId::RenderChunkCoordinatorSetAllDirty,
    SignatureId::ResourcePacksInfoPacketHandle,
    SignatureId::ResourcePackStackPacketHandle,
    SignatureId::NetworkStatisticsPacketSentTo,
    SignatureId::NetworkStatisticsPacketReceivedFrom,
    SignatureId::MinecraftGameCanStartLocalServer,
    SignatureId::MinecraftGameStartLocalServerAsync,
    SignatureId::LevelSettingsDefaultCtor,
    SignatureId::LevelSettingsCopyCtor,
    SignatureId::LevelSettingsDtor,
    SignatureId::SubChunkSerialize,
    SignatureId::LevelChunkSerializeBiomes,
    SignatureId::LevelChunkSerializeBorderBlocks,
    SignatureId::LevelChunkSerializeBlockEntities,
    SignatureId::LevelChunkSerializeBlockEntitiesForSubChunk,
    SignatureId::LevelChunkPopulateHeightMapDataForSubChunkPacket,
    SignatureId::ScreenControllerHandleEvent,
};

std::array<std::atomic<std::uintptr_t>, bedrocktools::memory::SignatureCount> gAddresses{};
std::array<std::atomic<std::uint8_t>, bedrocktools::memory::SignatureCount> gAttempted{};
std::vector<Segment> gSegments;
std::mutex gMutex;

std::string_view basename(std::string_view path) {
    const auto position = path.find_last_of('/');
    return position == std::string_view::npos ? path : path.substr(position + 1U);
}

int collectSegments(dl_phdr_info* info, std::size_t, void* opaque) {
    if (!info || !info->dlpi_name) return 0;
    if (basename(info->dlpi_name) != "libminecraftpe.so") return 0;
    auto& segments = *static_cast<std::vector<Segment>*>(opaque);
    for (std::size_t index = 0; index < info->dlpi_phnum; ++index) {
        const auto& header = info->dlpi_phdr[index];
        if (header.p_type != PT_LOAD || (header.p_flags & PF_X) == 0 || (header.p_flags & PF_R) == 0 || header.p_memsz == 0) continue;
        segments.push_back({reinterpret_cast<const std::uint8_t*>(info->dlpi_addr + header.p_vaddr), static_cast<std::size_t>(header.p_memsz)});
    }
    return 1;
}

struct Pattern {
    std::size_t id{};
    std::vector<std::uint8_t> bytes;
    std::vector<bool> wildcards;
    std::size_t anchorOffset{};
    std::size_t anchorLength{};
    bool required{};
};

unsigned hexDigit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return 16;
}

bool parsePattern(std::size_t id, Pattern& pattern) {
    auto [signature, required, text] = SignaturePatterns[id];
    if (static_cast<std::size_t>(signature) != id) return false;
    pattern.id = id;
    pattern.required = required;
    std::size_t run = 0;
    while (!text.empty()) {
        const auto space = text.find(' ');
        const auto byte = text.substr(0, space);
        if (byte == "?") {
            pattern.bytes.push_back(0);
            pattern.wildcards.push_back(true);
            run = 0;
        } else {
            if (byte.size() != 2) return false;
            const auto high = hexDigit(byte[0]);
            const auto low = hexDigit(byte[1]);
            if (high >= 16 || low >= 16) return false;
            pattern.bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
            pattern.wildcards.push_back(false);
            if (++run > pattern.anchorLength) {
                pattern.anchorLength = run;
                pattern.anchorOffset = pattern.bytes.size() - run;
            }
        }
        if (space == std::string_view::npos) break;
        text.remove_prefix(space + 1);
    }
    return pattern.anchorLength >= 4;
}

bool matches(const std::uint8_t* candidate, const Pattern& pattern) {
    for (std::size_t index = 0; index < pattern.bytes.size(); ++index) {
        if (!pattern.wildcards[index] && candidate[index] != pattern.bytes[index]) return false;
    }
    return true;
}

bool ensureSegmentsUnlocked() {
    if (!gSegments.empty()) return true;
    dl_iterate_phdr(collectSegments, &gSegments);
    return !gSegments.empty();
}

bool resolveSelectedUnlocked(const std::vector<std::size_t>& ids) {
    if (!ensureSegmentsUnlocked()) return false;

    std::vector<Pattern> patterns;
    patterns.reserve(ids.size());
    for (const auto id : ids) {
        if (id >= gAddresses.size() || gAttempted[id].load(std::memory_order_acquire)) continue;
        if (id == static_cast<std::size_t>(SignatureId::ScreenControllerHandleEvent)) {

            const auto address = reinterpret_cast<std::uintptr_t>(
                bedrocktools::core::rtti::virtualFunction("16ScreenController", 4));
            gAddresses[id].store(address, std::memory_order_relaxed);
            gAttempted[id].store(1U, std::memory_order_release);
            if (!address) return false;
            continue;
        }
        Pattern pattern;
        if (!parsePattern(id, pattern)) return false;
        patterns.push_back(std::move(pattern));
    }
    if (patterns.empty()) return true;

    std::vector<std::uint8_t> matchesPerPattern(patterns.size(), 0U);
    std::array<std::vector<std::size_t>, 256> buckets;
    for (std::size_t index = 0; index < patterns.size(); ++index) {
        const auto& pattern = patterns[index];
        buckets[pattern.bytes[pattern.anchorOffset]].push_back(index);
    }

    for (const auto& segment : gSegments) {
        const auto* begin = segment.start;
        const auto* end = segment.start + segment.size;
        for (const auto* anchor = begin; anchor < end; ++anchor) {
            const auto& bucket = buckets[*anchor];
            if (bucket.empty()) continue;
            for (const auto index : bucket) {
                const auto& pattern = patterns[index];
                if (static_cast<std::size_t>(end - anchor) < pattern.anchorLength) continue;
                if (std::memcmp(anchor, pattern.bytes.data() + pattern.anchorOffset, pattern.anchorLength) != 0) continue;
                if (static_cast<std::size_t>(anchor - begin) < pattern.anchorOffset) continue;
                const auto* candidate = anchor - pattern.anchorOffset;
                if ((reinterpret_cast<std::uintptr_t>(candidate) & 3U) != 0) continue;
                if (static_cast<std::size_t>(end - candidate) < pattern.bytes.size() || !matches(candidate, pattern)) continue;
                if (matchesPerPattern[index] < 0xFFU) ++matchesPerPattern[index];
                if (matchesPerPattern[index] == 1U) gAddresses[pattern.id].store(reinterpret_cast<std::uintptr_t>(candidate), std::memory_order_relaxed);
            }
        }
    }

    bool success = true;
    for (std::size_t index = 0; index < patterns.size(); ++index) {
        const auto id = patterns[index].id;
        if (matchesPerPattern[index] != 1U) gAddresses[id].store(0, std::memory_order_relaxed);
        const auto address = gAddresses[id].load(std::memory_order_relaxed);
        gAttempted[id].store(1U, std::memory_order_release);
        if (patterns[index].required && address == 0) success = false;
    }
    return success;
}

void clearResolvedUnlocked() {
    for (auto& address : gAddresses) address.store(0, std::memory_order_relaxed);
    for (auto& attempted : gAttempted) attempted.store(0, std::memory_order_relaxed);
    gSegments.clear();
}

}

bool resolveAll() {
    std::scoped_lock lock(gMutex);
    clearResolvedUnlocked();
    std::vector<std::size_t> ids;
    ids.reserve(ActiveSignatures.size());
    for (const auto id : ActiveSignatures) ids.push_back(static_cast<std::size_t>(id));
    const bool success = resolveSelectedUnlocked(ids);
    if (!success) clearResolvedUnlocked();
    return success;
}

std::uintptr_t resolve(std::size_t id) {
    if (id >= gAddresses.size()) return 0;
    if (gAttempted[id].load(std::memory_order_acquire)) {
        return gAddresses[id].load(std::memory_order_relaxed);
    }
    std::scoped_lock lock(gMutex);
    if (!gAttempted[id].load(std::memory_order_acquire)) {
        const std::vector<std::size_t> ids{id};
        if (!resolveSelectedUnlocked(ids)) return 0;
    }
    return gAddresses[id].load(std::memory_order_relaxed);
}

void clearResolved() {
    std::scoped_lock lock(gMutex);
    clearResolvedUnlocked();
}

}

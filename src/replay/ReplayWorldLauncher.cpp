#include "ReplayWorldLauncher.hpp"

#include "ReplayWorldSettings.hpp"
#include "core/Runtime.hpp"
#include "core/memory/Rtti.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

namespace bedrocktools::replay {
namespace {

struct alignas(8) ContentIdentityStorage {
    std::array<std::byte, 0x18> bytes{};
};

struct AsyncVoidStorage {
    void* first{};
    void* second{};
    AsyncVoidStorage() = default;
    AsyncVoidStorage(const AsyncVoidStorage&) = delete;
    AsyncVoidStorage& operator=(const AsyncVoidStorage&) = delete;
    ~AsyncVoidStorage() {}
};

std::atomic<std::uint64_t> launchCounter{0};
constexpr std::string_view ReplayLevelPrefix = "__playback_replay_world__";
constexpr std::uint32_t CleanupGraceTicks = 60;
constexpr std::uint32_t CleanupPollInterval = 20;
constexpr std::uint32_t StoragePollInterval = 20;

struct OpenWorldLocation {
    std::filesystem::path root;
    std::filesystem::path world;
};

std::optional<OpenWorldLocation> openWorldLocation(std::filesystem::path path) {
    if (!path.is_absolute()) return std::nullopt;
    path = path.lexically_normal();
    for (int depth = 0; depth < 16 && !path.empty(); ++depth) {
        const auto parent = path.parent_path();
        if (parent.filename() == "minecraftWorlds" && !path.filename().empty()) {
            return OpenWorldLocation{parent, path};
        }
        if (parent.empty() || parent == path) break;
        path = parent;
    }
    return std::nullopt;
}

void appendWorldRoots(const std::filesystem::path& seed, std::vector<std::filesystem::path>& roots,
                      std::set<std::string>& seen) {
    auto cursor = seed;
    for (int depth = 0; depth < 16 && !cursor.empty(); ++depth) {
        const std::array<std::filesystem::path, 3> candidates{
            cursor.filename() == "minecraftWorlds" ? cursor : std::filesystem::path{},
            cursor / "minecraftWorlds",
            cursor / "games" / "com.mojang" / "minecraftWorlds"
        };
        for (const auto& candidate : candidates) {
            if (candidate.empty()) continue;
            const auto normalized = candidate.lexically_normal();
            if (seen.insert(normalized.string()).second) roots.push_back(normalized);
        }
        const auto parent = cursor.parent_path();
        if (parent.empty() || parent == cursor) break;
        cursor = parent;
    }
}

std::vector<std::filesystem::path> replayWorldRoots() {
    std::vector<std::filesystem::path> roots;
    std::set<std::string> seen;
    appendWorldRoots(bedrocktools::core::Runtime::get().configDirectory(), roots, seen);
    appendWorldRoots(bedrocktools::core::Runtime::get().resourceDirectory(), roots, seen);
    std::error_code error;
    const auto current = std::filesystem::current_path(error);
    if (!error) appendWorldRoots(current, roots, seen);
    if (const char* home = std::getenv("HOME"); home && *home) {
        appendWorldRoots(std::filesystem::path(home), roots, seen);
    }
    return roots;
}

bool validReplayWorldTarget(const std::filesystem::path& target) {
    return target.parent_path().filename() == "minecraftWorlds"
        && target.filename().string().starts_with(ReplayLevelPrefix);
}

bool validReplayWorldTarget(const std::filesystem::path& target, std::string_view levelId) {
    return validReplayWorldTarget(target) && target.filename() == levelId;
}

bool pathExists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(std::filesystem::symlink_status(path, error)) && !error;
}

bool removeReplayWorld(const std::filesystem::path& target) {
    if (!validReplayWorldTarget(target)) return false;
    std::error_code error;
    const auto status = std::filesystem::symlink_status(target, error);
    if (error || !std::filesystem::exists(status)) return false;
    if (std::filesystem::is_symlink(status)) {
        return std::filesystem::remove(target, error) && !error;
    }
    if (!std::filesystem::is_directory(status)) return false;
    std::filesystem::remove_all(target, error);
    return !error && !std::filesystem::exists(target, error);
}

void* getMinecraftGame(bedrocktools::sdk::ClientInstance* client, std::uintptr_t getterAddress) {
    if (!client || !getterAddress) return nullptr;
    using GetterFn = void*(*)(bedrocktools::sdk::ClientInstance*);
    return reinterpret_cast<GetterFn>(getterAddress)(client);
}

bool isExpectedMinecraftGame(void* candidate) {
    const auto candidateAddress = reinterpret_cast<std::uintptr_t>(candidate);
    if (candidateAddress < 0x10000 ||
        (candidateAddress & (alignof(void*) - 1)) != 0) {
        return false;
    }

    return bedrocktools::core::rtti::isInstance(candidate, "13MinecraftGame");
}

}

ReplayWorldLauncher& ReplayWorldLauncher::get() {
    static ReplayWorldLauncher launcher;
    return launcher;
}

bool ReplayWorldLauncher::canLaunch() const {
    auto* client = clientInstance();
    if (!client || client->localPlayer()) return false;
    const auto getterAddress = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ClientInstanceGetMinecraftGame);
    const auto canStartAddress = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MinecraftGameCanStartLocalServer);
    const auto startAddress = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MinecraftGameStartLocalServerAsync);
    if (!getterAddress || !canStartAddress || !startAddress) return false;
    auto* game = getMinecraftGame(client, getterAddress);
    if (!isExpectedMinecraftGame(game)) return false;
    using CanStartFn = bool(*)(void*);
    return reinterpret_cast<CanStartFn>(canStartAddress)(game);
}

std::string ReplayWorldLauncher::makeLevelId() const {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ticks = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
    const auto serial = launchCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    return "__playback_replay_world__" + std::to_string(ticks) + "_" + std::to_string(serial);
}

bool ReplayWorldLauncher::launch() {
    auto* client = clientInstance();
    if (!client) {
        mStatus = "Minecraft client is unavailable";
        return false;
    }
    if (client->localPlayer()) {
        mStatus = "Return to the main menu before opening a replay";
        return false;
    }
    if (!ReplayWorldSettings::get().prepareFresh()) {
        mStatus = "Minecraft 1.26.62 replay world settings could not be rebuilt";
        return false;
    }
    const void* settings = ReplayWorldSettings::get().data();
    if (!settings) {
        mStatus = "Replay world settings are unavailable in this Minecraft session";
        return false;
    }
    const auto getterAddress = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ClientInstanceGetMinecraftGame);
    if (!getterAddress) {
        mStatus = "Minecraft 1.26.62 ClientInstance getter is unavailable";
        return false;
    }
    const auto canStartAddress = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MinecraftGameCanStartLocalServer);
    const auto startAddress = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MinecraftGameStartLocalServerAsync);
    if (!canStartAddress || !startAddress) {
        mStatus = "Minecraft 1.26.62 local-server functions are unavailable";
        return false;
    }
    auto* game = getMinecraftGame(client, getterAddress);
    if (!game) {
        mStatus = "ClientInstance getter returned no MinecraftGame";
        return false;
    }
    if (!isExpectedMinecraftGame(game)) {
        mStatus = "MinecraftGame candidate failed 1.26.62 vtable validation";
        return false;
    }
    using CanStartFn = bool(*)(void*);
    if (!reinterpret_cast<CanStartFn>(canStartAddress)(game)) {
        mStatus = "Minecraft is not ready to start the replay world";
        return false;
    }

    mLevelId = makeLevelId();
    mActiveWorldPath.clear();
    mStoragePollTicks = 0;
    mOrphanCleanupPending = true;
    const std::string levelName = "Playback Replay";
    ContentIdentityStorage contentIdentity;
    void* launchContext = nullptr;
    using StartFn = AsyncVoidStorage(*)(void*, const std::string&, const std::string&, const ContentIdentityStorage&, const void*, void**, int);
    [[maybe_unused]] auto result = reinterpret_cast<StartFn>(startAddress)(
        game,
        mLevelId,
        levelName,
        contentIdentity,
        settings,
        &launchContext,
        0
    );
    mStatus = "Starting replay world";
    return true;
}

void ReplayWorldLauncher::rememberWorldRoot(const std::filesystem::path& root) {
    if (root.filename() != "minecraftWorlds") return;
    const auto normalized = root.lexically_normal();
    if (std::find(mKnownWorldRoots.begin(), mKnownWorldRoots.end(), normalized) == mKnownWorldRoots.end()) {
        mKnownWorldRoots.push_back(normalized);
        mOrphanCleanupPending = true;
    }
}

void ReplayWorldLauncher::scanOpenWorldStorage() {
    std::error_code error;
    const std::filesystem::path descriptors("/proc/self/fd");
    std::filesystem::directory_iterator entry(
        descriptors,
        std::filesystem::directory_options::skip_permission_denied,
        error
    );
    const std::filesystem::directory_iterator end;
    while (!error && entry != end) {
        std::error_code linkError;
        const auto target = std::filesystem::read_symlink(entry->path(), linkError);
        if (!linkError && !target.empty()
            && target.string().find(" (deleted)") == std::string::npos) {
            if (const auto location = openWorldLocation(target)) {
                rememberWorldRoot(location->root);
                if (mLevelId.starts_with(ReplayLevelPrefix)
                    && validReplayWorldTarget(location->world, mLevelId)) {
                    mActiveWorldPath = location->world.lexically_normal();
                }
            }
        }
        entry.increment(error);
    }
}

void ReplayWorldLauncher::observeWorldStorage() {
    auto* client = clientInstance();
    if (!client || !client->localPlayer()) {
        mStoragePollTicks = 0;
        return;
    }
    if (++mStoragePollTicks < StoragePollInterval) return;
    mStoragePollTicks = 0;
    scanOpenWorldStorage();
}

void ReplayWorldLauncher::scheduleCleanup() {
    if (!mLevelId.starts_with(ReplayLevelPrefix)) return;

    scanOpenWorldStorage();
    const auto found = std::find_if(mPendingCleanup.begin(), mPendingCleanup.end(), [&](const auto& pending) {
        return pending.levelId == mLevelId;
    });
    if (found == mPendingCleanup.end()) {
        mPendingCleanup.push_back(PendingCleanup{mLevelId, mActiveWorldPath, CleanupGraceTicks});
    } else if (found->worldPath.empty() && !mActiveWorldPath.empty()) {
        found->worldPath = mActiveWorldPath;
    }
    mActiveWorldPath.clear();
    mCleanupPollTicks = 0;
    mOrphanCleanupPending = true;
}

void ReplayWorldLauncher::cleanupExitedWorlds() {
    auto* client = clientInstance();
    if (!client || client->localPlayer()) return;
    for (auto& pending : mPendingCleanup) {
        if (pending.graceTicks > 0) --pending.graceTicks;
    }
    if (++mCleanupPollTicks < CleanupPollInterval) return;
    mCleanupPollTicks = 0;
    auto roots = replayWorldRoots();
    std::set<std::string> seen;
    for (const auto& root : roots) seen.insert(root.lexically_normal().string());
    for (const auto& root : mKnownWorldRoots) {
        const auto normalized = root.lexically_normal();
        if (seen.insert(normalized.string()).second) roots.push_back(normalized);
    }

    for (auto pending = mPendingCleanup.begin(); pending != mPendingCleanup.end();) {
        if (pending->graceTicks != 0) {
            ++pending;
            continue;
        }
        bool removed = false;
        if (!pending->worldPath.empty()
            && validReplayWorldTarget(pending->worldPath, pending->levelId)) {
            removed = !pathExists(pending->worldPath) || removeReplayWorld(pending->worldPath);
        }
        if (!removed) {
            for (const auto& root : roots) {
                const auto target = root / pending->levelId;
                if (!validReplayWorldTarget(target, pending->levelId) || !pathExists(target)) continue;
                if (removeReplayWorld(target)) {
                    removed = true;
                    break;
                }
            }
        }
        if (removed) pending = mPendingCleanup.erase(pending);
        else ++pending;
    }

    bool foundWorldRoot = false;
    bool scannedKnownWorldRoot = false;
    for (const auto& root : roots) {
        std::error_code error;
        if (!std::filesystem::is_directory(root, error) || error) continue;
        foundWorldRoot = true;
        if (std::find(mKnownWorldRoots.begin(), mKnownWorldRoots.end(), root.lexically_normal())
            != mKnownWorldRoots.end()) {
            scannedKnownWorldRoot = true;
        }
        if (!mOrphanCleanupPending) continue;
        for (std::filesystem::directory_iterator entry(root, std::filesystem::directory_options::skip_permission_denied, error), end;
             !error && entry != end; entry.increment(error)) {
            const auto name = entry->path().filename().string();
            if (!name.starts_with(ReplayLevelPrefix)) continue;
            const auto pending = std::find_if(mPendingCleanup.begin(), mPendingCleanup.end(), [&](const auto& item) {
                return item.levelId == name && item.graceTicks > 0;
            });
            if (pending == mPendingCleanup.end()) removeReplayWorld(entry->path());
        }
    }

    if (foundWorldRoot && scannedKnownWorldRoot) mOrphanCleanupPending = false;
}

void ReplayWorldLauncher::setClientInstance(bedrocktools::sdk::ClientInstance* client) noexcept {
    mClientInstance.store(client, std::memory_order_release);
}

bedrocktools::sdk::ClientInstance* ReplayWorldLauncher::clientInstance() const noexcept {
    return mClientInstance.load(std::memory_order_acquire);
}

}

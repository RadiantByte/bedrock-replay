#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bedrocktools::sdk { class ClientInstance; }

namespace bedrocktools::replay {

class ReplayWorldLauncher {
public:
    static ReplayWorldLauncher& get();

    bool canLaunch() const;
    bool launch();
    void observeWorldStorage();
    void scheduleCleanup();
    void cleanupExitedWorlds();
    void setClientInstance(bedrocktools::sdk::ClientInstance* client) noexcept;
    bedrocktools::sdk::ClientInstance* clientInstance() const noexcept;
    const std::string& levelId() const noexcept { return mLevelId; }
    const std::string& status() const noexcept { return mStatus; }

private:
    ReplayWorldLauncher() = default;
    std::string makeLevelId() const;
    void scanOpenWorldStorage();
    void rememberWorldRoot(const std::filesystem::path& root);

    struct PendingCleanup {
        std::string levelId;
        std::filesystem::path worldPath;
        std::uint32_t graceTicks{};
    };

    std::string mLevelId;
    std::string mStatus{"Replay world ready"};
    std::filesystem::path mActiveWorldPath;
    std::vector<std::filesystem::path> mKnownWorldRoots;
    std::vector<PendingCleanup> mPendingCleanup;
    std::uint32_t mStoragePollTicks{};
    std::uint32_t mCleanupPollTicks{};
    bool mOrphanCleanupPending{true};
    std::atomic<bedrocktools::sdk::ClientInstance*> mClientInstance{nullptr};
};

}

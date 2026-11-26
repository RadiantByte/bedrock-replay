#pragma once

#include "ReplayArchive.hpp"
#include "NativePacket.hpp"
#include <bedrocktools/sdk/network/MinecraftPacketIds.hpp>
#include <bedrocktools/sdk/network/Packet.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bedrocktools::replay {

class ReplayRecorder {
public:
    enum class State { Idle, Starting, Recording, Paused, Closing };

    static ReplayRecorder& get();

    bool observeClientPacket(const bedrocktools::sdk::Packet& packet, bool preserveTimeline = true);
    void observeBlockChange(
        bedrocktools::sdk::BlockSource* source,
        const bedrocktools::sdk::BlockPos& position,
        int layer,
        const void* block,
        const void* previousBlock,
        std::uint32_t flags
    );
    bool observeSpawnedActor(const bedrocktools::sdk::Packet& packet);
    void observeLocalPlayerSwing();
    void tick(bedrocktools::sdk::Player* player);
    void onClientUpdate();

    bool start();
    bool pause();
    bool stop();
    void resetWorldCache();
    void setStorageDirectory(std::filesystem::path path);
    void setCurrentWorldName(std::string name);

    State state() const noexcept;
    std::string status() const;
    std::filesystem::path lastReplayPath() const;
    std::filesystem::path activeReplayPath() const;
    std::string currentWorldName() const;
    std::uint64_t recordedTicks() const noexcept;
    bool hasActivePlayer() const noexcept;
    std::filesystem::path libraryDirectory() const;

private:
    bool beginRecordingLocked(bedrocktools::sdk::Player& player);
    bool flushPendingLocked();
    bool writePacketLocked(ReplayAction action, const SerializedPacket& packet);
    bool captureFreshLevelChunksLocked(bedrocktools::sdk::Player& player, int dimensionId, std::vector<SerializedPacket>& packets);
    bool captureNewLevelChunksLocked(bedrocktools::sdk::Player& player, int dimensionId, std::vector<SerializedPacket>& packets);
    bool captureFreshActorsLocked(
        bedrocktools::sdk::Player& player,
        std::vector<SerializedPacket>& packets,
        std::vector<SerializedPacket>& skinPackets
    );
    bool captureNewActorsLocked(
        bedrocktools::sdk::Player& player,
        const std::vector<bedrocktools::sdk::Actor*>& actors,
        std::vector<SerializedPacket>& packets,
        std::vector<SerializedPacket>& statePackets
    );
    void appendActorStateLocked(
        std::uint64_t runtimeId,
        std::vector<SerializedPacket>& packets,
        bool includeArmor = true,
        bool includeHeldEquipment = true
    ) const;
    void rememberActorIdentityLocked(std::uint64_t uniqueId, std::uint64_t runtimeId);
    void retireActorLocked(std::uint64_t runtimeId);
    void updateConfigurationLocked(const SerializedPacket& packet, bedrocktools::sdk::MinecraftPacketIds id);
    std::filesystem::path replayDirectory() const;
    static std::string replayFileName();
    void setStatusLocked(std::string value);

    mutable std::mutex mMutex;
    std::atomic<State> mState{State::Idle};
    ReplayArchiveWriter mWriter;
    std::filesystem::path mTemporaryPath;
    std::filesystem::path mFinalPath;
    std::filesystem::path mLastReplayPath;
    std::filesystem::path mStorageDirectory;
    std::vector<SerializedPacket> mConfigurationPackets;
    std::vector<SerializedPacket> mPlayerListSnapshotPackets;
    std::unordered_map<std::uint64_t, SerializedPacket> mAddPlayerSnapshotPackets;
    std::unordered_map<std::uint64_t, SerializedPacket> mSpawnSnapshotPackets;
    std::unordered_map<std::uint64_t, std::vector<SerializedPacket>> mActorStateSnapshotPackets;
    std::vector<SerializedPacket> mPlayerSkinSnapshotPackets;
    std::unordered_map<std::int32_t, std::size_t> mConfigurationLatest;
    std::vector<SerializedPacket> mPendingPackets;
    std::vector<SerializedPacket> mPausedPackets;
    std::string mStatus{"Replay ready"};
    std::string mWorldName{"Unknown world"};
    std::string mRecordingWorldName{"Unknown world"};
    std::uint64_t mTicks{};
    std::unordered_map<std::uint64_t, ReplayMovementState> mLastEntityMovements;
    std::optional<std::int32_t> mLastObservedWorldTime;
    std::optional<float> mLastRainTarget;
    std::optional<float> mLastLightningTarget;
    std::optional<std::uint64_t> mSourceLocalUniqueId;
    std::optional<std::uint64_t> mSourceLocalRuntimeId;
    std::optional<std::uint64_t> mRecordedLocalRuntimeId;
    std::unordered_set<std::uint64_t> mCapturedChunkColumns;
    std::unordered_set<std::uint64_t> mCapturedActorRuntimeIds;
    std::unordered_set<std::uint64_t> mCapturedArmorRuntimeIds;
    std::unordered_set<std::uint64_t> mCapturedHeldRuntimeIds;
    std::unordered_map<std::uint64_t, std::uint64_t> mActorUniqueToRuntimeId;
    std::unordered_map<std::uint64_t, std::uint64_t> mActorRuntimeToUniqueId;
    std::unordered_set<std::uint64_t> mRemovedActorRuntimeIds;
    std::optional<bedrocktools::sdk::ChunkPos> mChunkCaptureCenter;
    std::size_t mChunkCaptureCursor{};
    std::unordered_map<std::int32_t, bool> mChunkRequestModes;
    std::atomic<bedrocktools::sdk::Player*> mCurrentPlayer{nullptr};
    std::atomic<bedrocktools::sdk::BlockSource*> mCurrentBlockSource{nullptr};
};

}

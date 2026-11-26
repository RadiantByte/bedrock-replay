#pragma once

#include "ReplayArchive.hpp"
#include "PlayerView.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <array>
#include <bedrocktools/sdk/offsets/Network.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bedrocktools::sdk { class Actor; class Level; class LevelChunk; class Player; class Packet; }
namespace bedrocktools::events { struct LevelStateEvent; struct PacketHandleEvent; }

namespace bedrocktools::replay {

class ReplayPlayer {
public:
    enum class State { Idle, StartingWorld, LoadingSnapshot, ResettingSnapshot, Playing, Paused, Finished, Failed, Stopping };

    struct ObserverCameraState {
        bedrocktools::sdk::Vec3 position{};
        bedrocktools::sdk::Vec2 rotation{};
    };

    static ReplayPlayer& get();

    bool open(const std::filesystem::path& path);
    bool open(const std::filesystem::path& path, ReplayArchiveData replay);
    void stop();
    bool pause();
    bool resume();
    bool seek(std::uint32_t tick);
    void seekForward(std::uint32_t tick);
    bool rewindForExport(std::uint32_t startTick);
    bool beginExportClock(std::uint32_t startTick);
    bool queueExportFrame(double renderTick);
    std::uint64_t exportRenderToken() const;
    void completeExportRender(std::uint64_t token);
    bool exportFrameReady() const;
    void endExportClock();
    bool seeking() const;
    void setPlaybackSpeed(float speed);
    float playbackSpeed() const;
    void setMuted(bool muted);
    bool muted() const;
    void tick(bedrocktools::sdk::Player* player);
    void onClientUpdate();
    void onLevelState(const bedrocktools::events::LevelStateEvent& event);
    void onPacketHandle(const bedrocktools::events::PacketHandleEvent& event);
    void onConfigurationPacket(std::shared_ptr<bedrocktools::sdk::Packet>& packet);
    void requestManualTimeOverride();
    bool beginRenderInterpolation();
    void endRenderInterpolation();
    bool renderReady() const;
    bool replayWorldActive() const;
    bool rendererActive(void* renderer, bool levelRenderer = false) const;
    bool ownsActor(const bedrocktools::sdk::Actor* actor) const;
    void refreshPlayerViews(bedrocktools::sdk::Player* player);
    std::vector<RecordedPlayerView> recordedPlayers() const;
    void beginPlayerViewFrame();
    std::optional<PlayerViewPose> playerView(const PlayerViewId& id);
    bedrocktools::sdk::Vec3 clipPlayerCamera(const bedrocktools::sdk::Vec3& eye,
        const bedrocktools::sdk::Vec3& desired) const;

    State state() const;
    std::string status() const;
    std::filesystem::path replayPath() const;
    std::uint32_t currentTick() const;
    std::uint32_t durationTicks() const;
    double renderTick() const;
    float preparationProgress() const;
    bedrocktools::sdk::Player* localPlayer() const;
    std::optional<ObserverCameraState> observerCameraState() const;

private:
    enum class SnapshotResetPhase { Idle, RemovingActors, LevelChunks, WaitingForSubChunks, ApplyingSubChunks, ApplyingState };

    ReplayPlayer() = default;
    bool dispatchPacketLocked(const SerializedPacket& serialized);
    const SerializedPacket* configurationPacketLocked(std::int32_t id) const;
    bool loadSnapshotLocked(bedrocktools::sdk::Player* player);
    bool resetSnapshotLocked(bedrocktools::sdk::Player* player);
    bool advanceOneTickLocked(bedrocktools::sdk::Player* player);
    bool pinExportLightingLocked(bedrocktools::sdk::Player* player);
    void setFailedLocked(std::string message);
    void resetLocked(std::string status);
    void placeCameraLocked(bedrocktools::sdk::Player* player);
    bool applyMovementLocked(bedrocktools::sdk::Player* player, std::string_view payload);
    float renderPartialLocked() const;
    void snapRenderPosesLocked();
    bool renderReadyLocked() const;
    bool indexPlayersLocked();
    void indexPlayerPacketLocked(const SerializedPacket& packet);
    void rememberPlayerLocked(const PlayerViewId& id, std::string_view name, bool recordingPlayer);

public:
    struct EntityRenderPose {
        bedrocktools::sdk::Vec3 position{};
        float pitch{};
        float yaw{};
        float headYaw{};
        float bodyYaw{};
    };

    struct EntityRenderHistory {
        EntityRenderPose previous{};
        EntityRenderPose current{};
        std::size_t renderStateIndex{static_cast<std::size_t>(-1)};
    };

    struct RenderActorState {
        void* stateVector{};
        bedrocktools::sdk::Vec3 statePosition{};
        bedrocktools::sdk::Vec3 statePreviousPosition{};
        bedrocktools::sdk::Vec3 statePositionDelta{};
        void* aabbShape{};
        bedrocktools::sdk::AABB aabb{};
        void* renderPosition{};
        bedrocktools::sdk::Vec3 renderPositionValue{};
        void* movementInterpolator{};
        bedrocktools::sdk::Vec3 movementPosition{};
        bedrocktools::sdk::Vec2 movementRotation{};
        float movementHeadYaw{};
        int movementPositionSteps{};
        int movementRotationSteps{};
        int movementHeadYawSteps{};
        void* renderRotation{};
        bedrocktools::sdk::Vec2 renderRotationValue{};
        void* actorRotation{};
        bedrocktools::sdk::Vec2 actorRotationValue{};
        bedrocktools::sdk::Vec2 actorRotationPreviousValue{};
        void* headRotation{};
        float headYaw{};
        float previousHeadYaw{};
        void* bodyRotation{};
        float bodyYaw{};
        float previousBodyYaw{};
        std::uint64_t runtimeId{};
        std::uint64_t uniqueId{};
    };

private:
    mutable std::recursive_mutex mMutex;
    ReplayArchiveData mReplay;
    std::vector<std::int32_t> mStartupConfigurationApplied;
    std::filesystem::path mPath;
    State mState{State::Idle};
    std::string mStatus{"Replay ready"};
    std::size_t mTimelineIndex{};
    std::uint32_t mTick{};
    float mTickAccumulator{};
    float mPlaybackSpeed{1.0f};
    bool mMuted{};
    std::optional<std::uint32_t> mSeekTarget;
    std::optional<std::uint32_t> mRewindTarget;
    bool mResumeAfterSeek{};
    std::optional<std::int32_t> mReplayTime;
    std::uint32_t mManualTimeOverrideTicks{};
    alignas(8) std::array<std::byte, bedrocktools::sdk::offsets::NetworkIdentifier::Size> mNetworkIdentifierStorage{};
    void* mNetworkIdentifier{};
    void* mNetworkCallback{};
    bool mReplayLevelInitialized{};
    bool mSnapshotLoaded{};
    bool mSnapshotConfigurationApplied{};
    std::size_t mSnapshotPacketIndex{};
    bool mSnapshotRecordedPlayerApplied{};
    bool mResetPlayerApplied{};
    std::vector<std::uint64_t> mResetActorIds;
    std::size_t mResetActorIndex{};
    std::uint32_t mSnapshotSettleTicks{};
    bool mSnapshotLevelChunksDispatched{};
    bool mSnapshotSubChunksDispatched{};
    bool mSnapshotPlayerListApplied{};
    std::uint32_t mSnapshotChunkWaitTicks{};
    std::uint32_t mSnapshotColumnsReadyTicks{};
    SnapshotResetPhase mSnapshotResetPhase{SnapshotResetPhase::Idle};
    std::uint32_t mSnapshotResetWaitTicks{};
    bool mSnapshotResetForExport{};
    bool mRenderRefreshPending{};
    std::uint32_t mSceneWarmupTicks{};
    std::uint32_t mSceneWarmupTotalTicks{};
    std::vector<bedrocktools::sdk::ChunkPos> mSnapshotChunkColumns;
    std::vector<std::shared_ptr<bedrocktools::sdk::LevelChunk>> mRetainedSnapshotChunks;
    bool mSawHudClosedAfterLaunch{};
    bool mLaunchPending{};
    std::uint32_t mStoppingTicks{};
    std::chrono::steady_clock::time_point mStartupBegan{};
    std::chrono::steady_clock::time_point mTickAdvancedAt{};
    float mFrozenRenderPartial{-1.0f};
    bool mExportClockActive{};
    bool mExportFrameReady{};
    double mExportRenderTick{};
    double mExportTargetRenderTick{};
    std::uint64_t mExportFrameGeneration{};
    std::uint64_t mExportRenderedGeneration{};
    bedrocktools::sdk::Player* mLocalPlayer{};
    bedrocktools::sdk::Level* mReplayLevel{};
    bedrocktools::sdk::Level* mInterpolatedLevel{};
    std::uint32_t mInterpolatedTick{};
    std::optional<ObserverCameraState> mObserverCameraState;
    std::unordered_map<std::uint64_t, EntityRenderHistory> mEntityRenderHistories;
    std::vector<RenderActorState> mRenderActorStates;
    struct PlayerViewHistory {
        EntityRenderHistory pose;
        bedrocktools::sdk::Vec3 previousEyeOffset{};
        bedrocktools::sdk::Vec3 eyeOffset{};
        std::uint64_t runtimeId{};
        std::uint32_t tick{};
        bool refreshed{};
    };
    std::vector<RecordedPlayerView> mRecordedPlayers;
    std::unordered_map<std::uint64_t, PlayerViewId> mPlayerIdentities;
    std::map<PlayerViewId, PlayerViewHistory> mPlayerViews;
    std::optional<float> mPlayerViewRenderPartial;
    std::size_t mPlayerIndexStage{};
    std::size_t mPlayerIndexPosition{};
};

}

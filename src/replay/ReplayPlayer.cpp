#include "ReplayPlayer.hpp"
#include "ReplayActorAnimation.hpp"

#include "NativePacket.hpp"
#include "ReplayContent.hpp"
#include "PacketLifecycle.hpp"
#include "ReplayEntityComponents.hpp"
#include "ReplayWorldLauncher.hpp"
#include "ReplayWorldSettings.hpp"
#include "runtime/InputBridge.hpp"
#include <limits>
#include <bedrocktools/events/LevelStateEvent.hpp>
#include <bedrocktools/events/PacketHandleEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/network/MinecraftPacketIds.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <bedrocktools/sdk/input/MoveInput.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/world/ChunkSource.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <new>
#include <functional>

namespace bedrocktools::sdk { class Block; }

namespace bedrocktools::replay {
namespace {
constexpr std::uint64_t ReplayRecordedPlayerUniqueId =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - 1024);
constexpr std::uint64_t ReplayRecordedPlayerRuntimeId = std::uint64_t{1} << 62;
constexpr std::uint32_t SceneWarmupTicks = 120;
constexpr std::uint32_t ExportSceneWarmupTicks = 60;
constexpr std::uint32_t InteractiveSceneWarmupTicks = 20;

class SnapshotBudget {
public:
    bool consumed(std::size_t bytes) {
        mBytes += bytes;
        return ++mPackets >= 4 || mBytes >= 512 * 1024
            || std::chrono::steady_clock::now() - mStarted >= std::chrono::milliseconds(4);
    }
private:
    std::chrono::steady_clock::time_point mStarted{std::chrono::steady_clock::now()};
    std::size_t mBytes{};
    std::size_t mPackets{};
};

using bedrocktools::sdk::MinecraftPacketIds;
constexpr std::string_view ReplayLevelPrefix = "__playback_replay_world__";

bool isHandshakePacket(std::int32_t id) {
    return id == static_cast<std::int32_t>(MinecraftPacketIds::ResourcePacksInfo)
        || id == static_cast<std::int32_t>(MinecraftPacketIds::ResourcePackStack)
        || id == static_cast<std::int32_t>(MinecraftPacketIds::StartGame);
}

bool isReplayWorldOwnedConfigurationPacket(std::int32_t id) {
    return id == static_cast<std::int32_t>(MinecraftPacketIds::DimensionDataPacket)
        || id == static_cast<std::int32_t>(MinecraftPacketIds::FeatureRegistryPacket);
}

bool isChunkPacket(std::int32_t id) {
    return id == static_cast<std::int32_t>(MinecraftPacketIds::FullChunkData)
        || id == static_cast<std::int32_t>(MinecraftPacketIds::SubChunkPacket);
}

bool isLevelChunkPacket(std::int32_t id) {
    return id == static_cast<std::int32_t>(MinecraftPacketIds::FullChunkData);
}

bool isSubChunkPacket(std::int32_t id) {
    return id == static_cast<std::int32_t>(MinecraftPacketIds::SubChunkPacket);
}

bool refreshReplayRenderChunks() {
    auto* client = ReplayWorldLauncher::get().clientInstance();
    auto* renderer = client ? client->levelRenderer() : nullptr;
    const auto address = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::RenderChunkCoordinatorSetAllDirty
    );
    if (!renderer || !address) return false;

    using SetAllDirtyFn = void (*)(void*, bool, bool);
    const auto setAllDirty = reinterpret_cast<SetAllDirtyFn>(address);
    void* node = bedrocktools::sdk::field<void*>(
        renderer,
        bedrocktools::sdk::offsets::LevelRenderer::mRenderChunkCoordinators
            + bedrocktools::sdk::offsets::HashTable::mFirstNode
    );
    bool refreshed = false;
    std::size_t visited = 0;
    while (node && visited++ < bedrocktools::sdk::offsets::RenderChunkCoordinator::MaxNodes) {
        void* next = bedrocktools::sdk::field<void*>(node, bedrocktools::sdk::offsets::HashNode::mNext);
        void* coordinator = bedrocktools::sdk::field<void*>(
            node,
            bedrocktools::sdk::offsets::HashNode::mValuePointer
        );
        if (coordinator) {
            setAllDirty(coordinator, true, false);
            refreshed = true;
        }
        node = next;
    }
    return refreshed;
}

bool finitePose(const ReplayPlayer::EntityRenderPose& pose) {
    return std::isfinite(pose.position.x) && std::isfinite(pose.position.y)
        && std::isfinite(pose.position.z) && std::isfinite(pose.pitch)
        && std::isfinite(pose.yaw) && std::isfinite(pose.headYaw)
        && std::isfinite(pose.bodyYaw);
}

float interpolateAngle(float previous, float current, float amount) {
    return previous + std::remainder(current - previous, 360.0f) * amount;
}

ReplayPlayer::EntityRenderPose actorPose(bedrocktools::sdk::Actor& actor) {
    const auto position = actor.position();
    const auto rotation = actor.rotation();
    ReplayPlayer::EntityRenderPose pose{position, rotation.x, rotation.y, rotation.y, rotation.y};
    auto* context = reinterpret_cast<EntityContext*>(actor.entityContext());
    if (!context) return pose;
    if (auto* head = context->tryGetComponent<ActorHeadRotationComponent>()) pose.headYaw = head->mYHeadRot;
    if (auto* body = context->tryGetComponent<MobBodyRotationComponent>()) pose.bodyYaw = body->mYBodyRot;
    return pose;
}

ReplayPlayer::EntityRenderPose interpolatePose(
    const ReplayPlayer::EntityRenderHistory& history,
    float amount
) {
    return {
        {
            std::lerp(history.previous.position.x, history.current.position.x, amount),
            std::lerp(history.previous.position.y, history.current.position.y, amount),
            std::lerp(history.previous.position.z, history.current.position.z, amount)
        },
        interpolateAngle(history.previous.pitch, history.current.pitch, amount),
        interpolateAngle(history.previous.yaw, history.current.yaw, amount),
        interpolateAngle(history.previous.headYaw, history.current.headYaw, amount),
        interpolateAngle(history.previous.bodyYaw, history.current.bodyYaw, amount)
    };
}

bool finiteVector(const bedrocktools::sdk::Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

std::string playerDisplayName(std::string_view name) {
    std::string result;
    for (std::size_t i = 0; i < name.size();) {
        const auto byte = static_cast<unsigned char>(name[i]);
        if (byte == 0xC2 && i + 2 < name.size()
            && static_cast<unsigned char>(name[i + 1]) == 0xA7) {
            i += 3;
            continue;
        }
        if (byte < 0x20 || byte == 0x7F) {
            ++i;
            continue;
        }
        const std::size_t width = byte < 0x80 ? 1 : (byte >= 0xC2 && byte <= 0xDF) ? 2
            : (byte >= 0xE0 && byte <= 0xEF) ? 3 : (byte >= 0xF0 && byte <= 0xF4) ? 4 : 0;
        if (!width || i + width > name.size()) {
            ++i;
            continue;
        }
        bool valid = true;
        for (std::size_t j = 1; j < width; ++j) {
            if ((static_cast<unsigned char>(name[i + j]) & 0xC0) != 0x80) valid = false;
        }
        if (!valid) {
            ++i;
            continue;
        }
        if (result.size() + width > 128) break;
        result.append(name.substr(i, width));
        i += width;
    }
    return result;
}

std::optional<SerializedPacket> playerPacket(std::string_view payload) {
    std::uint32_t raw{};
    for (std::size_t i = 0; i < payload.size() && i < 5; ++i) {
        const auto byte = static_cast<std::uint8_t>(payload[i]);
        raw |= static_cast<std::uint32_t>(byte & 0x7F) << (i * 7);
        if (byte & 0x80) continue;
        const auto id = static_cast<std::int32_t>((raw >> 1) ^ (0u - (raw & 1u)));
        if (id != static_cast<int>(MinecraftPacketIds::AddPlayer)
            && id != static_cast<int>(MinecraftPacketIds::PlayerList)) return std::nullopt;
        return SerializedPacket{id, std::string(payload.substr(i + 1))};
    }
    return std::nullopt;
}

struct CameraHitResult {
    bedrocktools::sdk::Vec3 start;
    bedrocktools::sdk::Vec3 ray;
    std::int32_t type;
    std::uint8_t face;
    std::array<std::byte, 3> facePadding;
    bedrocktools::sdk::BlockPos block;
    bedrocktools::sdk::Vec3 position;
    std::weak_ptr<void> registry;
    std::uint32_t entity;
    std::array<std::byte, 4> entityPadding;
    bedrocktools::sdk::AABB bounds;
    bool liquid;
    std::uint8_t liquidFace;
    std::array<std::byte, 2> liquidPadding;
    bedrocktools::sdk::BlockPos liquidBlock;
    bedrocktools::sdk::Vec3 liquidPosition;
    bool indirect;
    std::array<std::byte, 3> tailPadding;
};

static_assert(sizeof(CameraHitResult) == 0x88);
static_assert(offsetof(CameraHitResult, type) == 0x18);
static_assert(offsetof(CameraHitResult, position) == 0x2C);
static_assert(offsetof(CameraHitResult, registry) == 0x38);
static_assert(offsetof(CameraHitResult, bounds) == 0x50);

std::string encodeSignedVarInt64(std::uint64_t value) {
    std::uint64_t encoded = (value << 1u) ^ (0u - (value >> 63u));
    std::string output;
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(encoded & 0x7Fu);
        encoded >>= 7u;
        if (encoded != 0) byte |= 0x80u;
        output.push_back(static_cast<char>(byte));
    } while (encoded != 0);
    return output;
}

std::optional<std::int32_t> readWorldTime(bedrocktools::sdk::Player* player) {
    if (!player) return std::nullopt;
    auto* level = player->level();
    const auto getTimeAddress = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::Time);
    if (!level || !getTimeAddress) return std::nullopt;
    using GetTimeFn = int (*)(const bedrocktools::sdk::Level*);
    return reinterpret_cast<GetTimeFn>(getTimeAddress)(level);
}

bool writeWorldTime(bedrocktools::sdk::Player* player, std::int32_t worldTime) {
    if (!player) return false;
    auto* level = player->level();
    const auto setTimeAddress = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::SetTime);
    if (!level || !setTimeAddress) return false;
    using SetTimeFn = void (*)(bedrocktools::sdk::Level*, int);
    reinterpret_cast<SetTimeFn>(setTimeAddress)(level, worldTime);
    return true;
}

bool applyEnvironment(bedrocktools::sdk::Player* player, const ReplayEnvironmentState& state) {
    if (!player) return false;
    if (state.setTime && !writeWorldTime(player, state.worldTime)) return false;
    if (state.setWeather) {
        auto* dimension = player->dimension();
        auto* weather = dimension ? dimension->weather() : nullptr;
        if (!weather) return false;
        bedrocktools::sdk::field<float>(
            weather, bedrocktools::sdk::offsets::Weather::mOldRainLevel
        ) = state.rainLevel;
        bedrocktools::sdk::field<float>(
            weather, bedrocktools::sdk::offsets::Weather::mRainLevel
        ) = state.rainLevel;
        bedrocktools::sdk::field<float>(
            weather, bedrocktools::sdk::offsets::Weather::mTargetRainLevel
        ) = state.rainTarget;
        bedrocktools::sdk::field<float>(
            weather, bedrocktools::sdk::offsets::Weather::mOldLightningLevel
        ) = state.lightningLevel;
        bedrocktools::sdk::field<float>(
            weather, bedrocktools::sdk::offsets::Weather::mLightningLevel
        ) = state.lightningLevel;
        bedrocktools::sdk::field<float>(
            weather, bedrocktools::sdk::offsets::Weather::mTargetLightningLevel
        ) = state.lightningTarget;
    }
    return true;
}

}

ReplayPlayer& ReplayPlayer::get() {
    static ReplayPlayer player;
    return player;
}

bool ReplayPlayer::open(const std::filesystem::path& path) {
    {
        std::scoped_lock lock(mMutex);
        if (mState == State::Failed && !mReplayLevelInitialized) resetLocked("Replay ready");
        if (mState != State::Idle) return false;
    }

    ReplayArchiveData replay;
    std::string error;
    if (!readReplayArchive(path, replay, &error)) {
        std::scoped_lock lock(mMutex);
        mStatus = "Unable to open replay: " + error;
        mState = State::Failed;
        return false;
    }
    return open(path, std::move(replay));
}

bool ReplayPlayer::open(const std::filesystem::path& path, ReplayArchiveData replay) {
    {
        std::scoped_lock lock(mMutex);
        if (mState == State::Failed && !mReplayLevelInitialized) resetLocked("Replay ready");
        if (mState != State::Idle) return false;
    }
    if (std::none_of(replay.snapshotPackets.begin(), replay.snapshotPackets.end(), [](const auto& packet) {
            return isChunkPacket(packet.id);
        })) {
        std::scoped_lock lock(mMutex);
        mStatus = "This replay has no terrain snapshot; record a new replay with the updated mod";
        mState = State::Failed;
        return false;
    }

    if (!ReplayWorldSettings::get().setDefaultSpawn(replay.context.x, replay.context.y, replay.context.z)) {
        std::scoped_lock lock(mMutex);
        mStatus = "This replay has an invalid recorded player position";
        mState = State::Failed;
        return false;
    }
    ReplayWorldSettings::get().setInitialTime(replay.context.worldTime);

    {
        std::scoped_lock lock(mMutex);
        if (mState != State::Idle) return false;
        mReplay = std::move(replay);
        mStartupConfigurationApplied.clear();
        mPath = path;
        mTimelineIndex = 0;
        mTick = 0;
        mNetworkIdentifier = nullptr;
        mNetworkCallback = nullptr;
        mReplayLevelInitialized = false;
        mSnapshotLoaded = false;
        mSnapshotConfigurationApplied = false;
        mSnapshotPacketIndex = 0;
        mSnapshotRecordedPlayerApplied = false;
        mResetPlayerApplied = false;
        mResetActorIds.clear();
        mResetActorIndex = 0;
        mSnapshotSettleTicks = 0;
        mSnapshotLevelChunksDispatched = false;
        mSnapshotSubChunksDispatched = false;
        mSnapshotPlayerListApplied = false;
        mSnapshotChunkWaitTicks = 0;
        mSnapshotColumnsReadyTicks = 0;
        mSnapshotResetPhase = SnapshotResetPhase::Idle;
        mSnapshotResetWaitTicks = 0;
        mSnapshotResetForExport = false;
        mRenderRefreshPending = false;
        mSceneWarmupTicks = 0;
        mSceneWarmupTotalTicks = 0;
        mSnapshotChunkColumns.clear();
        mRetainedSnapshotChunks.clear();
        if (const auto hudOpen = bedrockreplay::input::hudScreenOpen()) {
            mSawHudClosedAfterLaunch = !*hudOpen;
        } else {
            mSawHudClosedAfterLaunch = true;
        }
        mLaunchPending = true;
        mStoppingTicks = 0;
        mStartupBegan = {};
        mTickAdvancedAt = {};
        mFrozenRenderPartial = -1.0f;
        mExportClockActive = false;
        mExportFrameReady = false;
        mExportRenderTick = 0.0;
        mExportTargetRenderTick = 0.0;
        mExportFrameGeneration = 0;
        mExportRenderedGeneration = 0;
        mLocalPlayer = nullptr;
        mReplayLevel = nullptr;
        mObserverCameraState.reset();
        mEntityRenderHistories.clear();
        mRenderActorStates.clear();
        mInterpolatedLevel = nullptr;
        mInterpolatedTick = 0;
        mRecordedPlayers.clear();
        mPlayerIdentities.clear();
        mPlayerViews.clear();
        mPlayerViewRenderPartial.reset();
        mPlayerIndexStage = 0;
        mPlayerIndexPosition = 0;
        mState = State::StartingWorld;
        mStatus = "Replay launch queued";
    }
    return true;
}

void ReplayPlayer::stop() {
    bool leaveWorld = false;
    {
        std::scoped_lock lock(mMutex);
        if (mState == State::Idle || mState == State::Stopping) return;
        if (mLaunchPending) {
            resetLocked("Replay stopped");
            return;
        }
        leaveWorld = mReplayLevelInitialized || mState == State::StartingWorld || mState == State::LoadingSnapshot
            || mState == State::ResettingSnapshot
            || mState == State::Playing || mState == State::Paused || mState == State::Finished;
        if (leaveWorld && ReplayWorldLauncher::get().clientInstance()) {
            mState = State::Stopping;
            mStatus = "Leaving replay world";
            mStoppingTicks = 0;
            mPlayerViews.clear();
            mPlayerViewRenderPartial.reset();
        } else {
            resetLocked("Replay stopped");
            return;
        }
    }
    ReplayWorldLauncher::get().scheduleCleanup();
    if (auto* client = ReplayWorldLauncher::get().clientInstance()) client->requestLeaveGameAsync();
}

bool ReplayPlayer::pause() {
    std::scoped_lock lock(mMutex);
    if (mState != State::Playing) return false;
    mFrozenRenderPartial = renderPartialLocked();
    mState = State::Paused;
    mStatus = "Replay paused";
    return true;
}

bool ReplayPlayer::resume() {
    std::scoped_lock lock(mMutex);
    if (mState != State::Paused) return false;
    const float partial = std::max(0.0f, mFrozenRenderPartial);
    const auto offset = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<float>(partial / (20.0f * mPlaybackSpeed))
    );
    mTickAdvancedAt = std::chrono::steady_clock::now() - offset;
    mFrozenRenderPartial = -1.0f;
    mState = State::Playing;
    mStatus = "Replay playing";
    return true;
}

bool ReplayPlayer::seek(std::uint32_t tick) {
    std::scoped_lock lock(mMutex);
    if (mState != State::Playing && mState != State::Paused && mState != State::Finished) return false;
    tick = std::min(tick, mReplay.durationTicks);
    mResumeAfterSeek = mState == State::Playing;
    if (tick < mTick) {
        mSnapshotResetForExport = false;
        mState = State::ResettingSnapshot;
        mRewindTarget = tick;
        mSeekTarget.reset();
        mStatus = "Resetting replay for backward seek";
    } else {
        mState = State::Paused;
        mSeekTarget = tick;
        mStatus = "Seeking replay";
    }
    return true;
}

void ReplayPlayer::seekForward(std::uint32_t tick) {
    std::scoped_lock lock(mMutex);
    if (tick < mTick || mReplay.durationTicks == 0) return;
    mSeekTarget = std::min(tick, mReplay.durationTicks);
}

bool ReplayPlayer::rewindForExport(std::uint32_t startTick) {
    std::scoped_lock lock(mMutex);
    if (mState != State::Playing && mState != State::Paused && mState != State::Finished) return false;
    mState = State::Paused;
    mRewindTarget = std::min(startTick, mReplay.durationTicks);
    mSeekTarget.reset();
    mResumeAfterSeek = true;
    mSnapshotResetForExport = true;
    mState = State::LoadingSnapshot;
    mStatus = "Preparing replay export";
    return true;
}

bool ReplayPlayer::beginExportClock(std::uint32_t startTick) {
    std::scoped_lock lock(mMutex);
    startTick = std::min(startTick, mReplay.durationTicks);
    if (mState != State::Playing || mTick != startTick || mSeekTarget || mRewindTarget) return false;
    mTickAccumulator = 0.0f;
    mExportClockActive = true;
    mExportFrameReady = true;
    mExportRenderTick = static_cast<double>(startTick);
    mExportTargetRenderTick = mExportRenderTick;
    mExportFrameGeneration = 1;
    mExportRenderedGeneration = 0;
    mFrozenRenderPartial = 0.0f;
    return true;
}

bool ReplayPlayer::queueExportFrame(double renderTick) {
    std::scoped_lock lock(mMutex);
    if (!mExportClockActive || mState != State::Playing || !std::isfinite(renderTick)) return false;
    renderTick = std::clamp(renderTick, static_cast<double>(mTick), static_cast<double>(mReplay.durationTicks));
    if (renderTick + 0.000001 < mExportRenderTick) return false;
    mExportTargetRenderTick = renderTick;

    ++mExportFrameGeneration;
    if (mExportFrameGeneration == 0) mExportFrameGeneration = 1;
    const auto targetTick = static_cast<std::uint32_t>(std::floor(renderTick + 0.000001));
    if (targetTick <= mTick) {
        mExportRenderTick = renderTick;
        mExportFrameReady = true;
        mFrozenRenderPartial = static_cast<float>(std::clamp(renderTick - static_cast<double>(mTick), 0.0, 1.0));
    } else {
        mExportFrameReady = false;
    }
    return true;
}

std::uint64_t ReplayPlayer::exportRenderToken() const {
    std::scoped_lock lock(mMutex);
    return mExportClockActive && mExportFrameReady && mState == State::Playing
        ? mExportFrameGeneration
        : 0;
}

void ReplayPlayer::completeExportRender(std::uint64_t token) {
    if (token == 0) return;
    std::scoped_lock lock(mMutex);
    if (mExportClockActive && mExportFrameReady && mState == State::Playing
        && token == mExportFrameGeneration) {
        mExportRenderedGeneration = token;
    }
}

bool ReplayPlayer::exportFrameReady() const {
    std::scoped_lock lock(mMutex);
    return mExportClockActive && mExportFrameReady && mState == State::Playing
        && mExportRenderedGeneration == mExportFrameGeneration;
}

void ReplayPlayer::endExportClock() {
    std::scoped_lock lock(mMutex);
    if (!mExportClockActive) return;
    mExportClockActive = false;
    mExportFrameReady = false;
    mExportRenderTick = 0.0;
    mExportTargetRenderTick = 0.0;
    mExportFrameGeneration = 0;
    mExportRenderedGeneration = 0;
    mTickAccumulator = 0.0f;
    mFrozenRenderPartial = 0.0f;
}

bool ReplayPlayer::seeking() const {
    std::scoped_lock lock(mMutex);
    return mRewindTarget.has_value() || mSeekTarget.has_value();
}

void ReplayPlayer::setPlaybackSpeed(float speed) {
    std::scoped_lock lock(mMutex);
    const float partial = renderPartialLocked();
    mPlaybackSpeed = std::clamp(speed, 0.1f, 8.0f);
    if (mState == State::Playing) {
        const auto offset = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<float>(partial / (20.0f * mPlaybackSpeed))
        );
        mTickAdvancedAt = std::chrono::steady_clock::now() - offset;
    } else if (mFrozenRenderPartial >= 0.0f) {
        mFrozenRenderPartial = partial;
    }
}

float ReplayPlayer::playbackSpeed() const {
    std::scoped_lock lock(mMutex);
    return mPlaybackSpeed;
}

void ReplayPlayer::setMuted(bool muted) {
    std::scoped_lock lock(mMutex);
    mMuted = muted;
}

bool ReplayPlayer::muted() const {
    std::scoped_lock lock(mMutex);
    return mMuted;
}

bool ReplayPlayer::dispatchPacketLocked(const SerializedPacket& serialized) {
    const auto packetId = static_cast<MinecraftPacketIds>(serialized.id);

    if (isRecordedContainerUiPacket(packetId) || isHandshakePacket(serialized.id)) return true;
    if (!content::failure().empty()) return false;
    if (mMuted && (packetId == MinecraftPacketIds::LevelSoundEventV1Deprecated
            || packetId == MinecraftPacketIds::LevelSoundEventV2Deprecated
            || packetId == MinecraftPacketIds::LevelSoundEvent
            || packetId == MinecraftPacketIds::PlaySound)) return true;
    auto packet = native::createPacket(serialized.id);
    if (!packet || !native::deserialize(*packet, serialized.payload)) return false;
    if (!content::prepareReplayPacket(*packet)) return false;

    if (packetId == MinecraftPacketIds::AddPlayer
        && native::addPlayerRuntimeId(*packet) == ReplayRecordedPlayerRuntimeId
        && !native::rewriteAddPlayerIdentity(*packet, ReplayRecordedPlayerUniqueId, ReplayRecordedPlayerRuntimeId)) {
        return false;
    }
    if (!native::normalizeAddPlayerGameType(*packet)) return false;
    const auto identities = packetId == MinecraftPacketIds::AddPlayer
        ? native::playerIdentities(*packet) : std::vector<native::PlayerIdentity>{};
    const auto runtimeId = packetId == MinecraftPacketIds::AddPlayer ? native::addPlayerRuntimeId(*packet) : 0;
    const bool dispatched = native::dispatch(packet, mNetworkIdentifier, mNetworkCallback);
    if (dispatched && runtimeId) {
        for (const auto& identity : identities) {
            mPlayerIdentities[runtimeId] = identity.uuid;
            rememberPlayerLocked(identity.uuid, identity.name, runtimeId == ReplayRecordedPlayerRuntimeId);
        }
    }
    return dispatched;
}

const SerializedPacket* ReplayPlayer::configurationPacketLocked(std::int32_t id) const {
    for (auto it = mReplay.configurationPackets.rbegin(); it != mReplay.configurationPackets.rend(); ++it) {
        if (it->id == id && !it->payload.empty()) return &*it;
    }
    return nullptr;
}

void ReplayPlayer::placeCameraLocked(bedrocktools::sdk::Player* player) {
    if (!player) return;
    if (auto* state = player->stateVectorComponent()) {
        const bedrocktools::sdk::Vec3 position{mReplay.context.x, mReplay.context.y, mReplay.context.z};
        bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(state, bedrocktools::sdk::offsets::StateVectorComponent::mPos) = position;
        bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(state, bedrocktools::sdk::offsets::StateVectorComponent::mPosPrev) = position;
        bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(state, bedrocktools::sdk::offsets::StateVectorComponent::mPosDelta) = {};
    }
    if (auto* rotation = player->rotationComponent()) {
        const bedrocktools::sdk::Vec2 value{mReplay.context.pitch, mReplay.context.yaw};
        bedrocktools::sdk::field<bedrocktools::sdk::Vec2>(rotation, bedrocktools::sdk::offsets::ActorRotationComponent::mRot) = value;
        bedrocktools::sdk::field<bedrocktools::sdk::Vec2>(rotation, bedrocktools::sdk::offsets::ActorRotationComponent::mRotPrev) = value;
    }
}

bool ReplayPlayer::applyMovementLocked(bedrocktools::sdk::Player* player, std::string_view payload) {
    if (!player || !player->level()) return false;
    std::vector<ReplayMovementState> movements;
    if (!decodeMoveEntities(payload, movements)) return false;
    if (movements.empty()) return true;

    auto actors = native::runtimeActors(*player->level());
    for (const auto& movement : movements) {
        const auto runtimeId = static_cast<std::uint64_t>(movement.actorId);
        bedrocktools::sdk::Actor* target = nullptr;
        bedrocktools::sdk::Actor* recordedPlayerFallback = nullptr;
        for (auto* actor : actors) {
            if (!actor || actor == player) continue;
            if (native::actorRuntimeId(*actor) == runtimeId) {
                target = actor;
                break;
            }
            if (runtimeId == ReplayRecordedPlayerRuntimeId
                && native::actorUniqueId(*actor) == ReplayRecordedPlayerUniqueId) {
                recordedPlayerFallback = actor;
            }
        }
        if (!target) target = recordedPlayerFallback;
        if (!target) continue;

        const bedrocktools::sdk::Vec3 position{movement.x, movement.y, movement.z};
        const bedrocktools::sdk::Vec2 rotation{movement.pitch, movement.yaw};
        bedrocktools::sdk::Vec3 previousPosition = position;
        EntityRenderPose previousPose = actorPose(*target);
        EntityRenderPose currentPose{position, movement.pitch, movement.yaw, movement.headYaw, movement.bodyYaw};
        if (const auto history = mEntityRenderHistories.find(runtimeId); history != mEntityRenderHistories.end()) {
            previousPose = history->second.current;
        }
        const float dx = currentPose.position.x - previousPose.position.x;
        const float dy = currentPose.position.y - previousPose.position.y;
        const float dz = currentPose.position.z - previousPose.position.z;
        if (!finitePose(previousPose) || !finitePose(currentPose) || dx * dx + dy * dy + dz * dz > 4096.0f) {
            previousPose = currentPose;
        }
        mEntityRenderHistories.insert_or_assign(runtimeId, EntityRenderHistory{previousPose, currentPose});

        supplyRecordedWalkDistance(target, previousPose.position, currentPose.position);

        if (native::actorIsPlayer(*target)) {
            const bool gliding = movement.glidingKnown ? movement.gliding : native::actorIsGliding(*target);
            if (movement.glidingKnown || gliding) native::setActorGliding(*target, gliding);
        }

        if (auto* state = target->stateVectorComponent()) {
            previousPosition = bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(
                state, bedrocktools::sdk::offsets::StateVectorComponent::mPos
            );
            bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(
                state, bedrocktools::sdk::offsets::StateVectorComponent::mPos
            ) = position;
            bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(
                state, bedrocktools::sdk::offsets::StateVectorComponent::mPosPrev
            ) = previousPosition;
            bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(
                state, bedrocktools::sdk::offsets::StateVectorComponent::mPosDelta
            ) = {
                position.x - previousPosition.x,
                position.y - previousPosition.y,
                position.z - previousPosition.z
            };
            auto* aabbShape = bedrocktools::sdk::field<void*>(
                target,
                bedrocktools::sdk::offsets::Actor::mStateVectorComponent
                    + bedrocktools::sdk::offsets::BuiltInActorComponents::mAABBShapeComponent
            );
            if (aabbShape) {
                auto& bounds = bedrocktools::sdk::field<bedrocktools::sdk::AABB>(
                    aabbShape,
                    bedrocktools::sdk::offsets::AABBShapeComponent::mAABB
                );
                const bedrocktools::sdk::Vec3 delta{
                    position.x - previousPosition.x,
                    position.y - previousPosition.y,
                    position.z - previousPosition.z
                };
                bounds.min = {
                    bounds.min.x + delta.x,
                    bounds.min.y + delta.y,
                    bounds.min.z + delta.z
                };
                bounds.max = {
                    bounds.max.x + delta.x,
                    bounds.max.y + delta.y,
                    bounds.max.z + delta.z
                };
            }
        }

        if (auto* actorRotation = target->rotationComponent()) {
            bedrocktools::sdk::field<bedrocktools::sdk::Vec2>(
                actorRotation, bedrocktools::sdk::offsets::ActorRotationComponent::mRot
            ) = rotation;
            bedrocktools::sdk::field<bedrocktools::sdk::Vec2>(
                actorRotation, bedrocktools::sdk::offsets::ActorRotationComponent::mRotPrev
            ) = rotation;
        }

        auto* context = reinterpret_cast<EntityContext*>(target->entityContext());
        if (!context) continue;

        if (auto* interpolator = context->tryGetComponent<MovementInterpolatorComponent>()) {
            interpolator->mPos = position;
            interpolator->mRot = rotation;
            interpolator->mHeadYaw = movement.headYaw;
            interpolator->mPositionSteps = 0;
            interpolator->mRotationSteps = 0;
            interpolator->mHeadYawSteps = 0;
        }
        if (auto* renderPosition = context->tryGetComponent<RenderPositionComponent>()) {
            renderPosition->mValue = position;
        }
        if (auto* renderRotation = context->tryGetComponent<RenderRotationComponent>()) {
            renderRotation->mRot = rotation;
        }
        if (auto* headRotation = context->tryGetComponent<ActorHeadRotationComponent>()) {
            headRotation->mYHeadRot = movement.headYaw;
            headRotation->mYHeadRotO = movement.headYaw;
        }
        if (auto* bodyRotation = context->tryGetComponent<MobBodyRotationComponent>()) {
            bodyRotation->mYBodyRot = movement.bodyYaw;
            bodyRotation->mYBodyRotO = movement.bodyYaw;
        }
    }
    return true;
}

bool ReplayPlayer::loadSnapshotLocked(bedrocktools::sdk::Player* player) {
    if (!mNetworkIdentifier || !mNetworkCallback || !mReplayLevelInitialized || !player) return false;
    mState = State::LoadingSnapshot;

    SnapshotBudget budget;
    if (!mSnapshotConfigurationApplied) {
        while (mSnapshotPacketIndex < mReplay.configurationPackets.size()) {
            const auto& packet = mReplay.configurationPackets[mSnapshotPacketIndex++];
            if (packet.payload.empty() || isHandshakePacket(packet.id)
                || isReplayWorldOwnedConfigurationPacket(packet.id)
                || std::find(mStartupConfigurationApplied.begin(), mStartupConfigurationApplied.end(), packet.id)
                    != mStartupConfigurationApplied.end()) continue;
            if (!dispatchPacketLocked(packet)) {
                setFailedLocked("Replay configuration packet failed: " + std::to_string(packet.id));
                return false;
            }
            if (budget.consumed(packet.payload.size())) return true;
        }
        mSnapshotConfigurationApplied = true;
        mSnapshotPacketIndex = 0;
        mStatus = "Preparing replay terrain";
        return true;
    }

    auto* dimension = player->dimension();
    auto* region = dimension ? dimension->blockSource() : nullptr;
    auto* source = region ? region->chunkSource() : nullptr;
    if (!source) {
        setFailedLocked("Replay terrain source is unavailable");
        return false;
    }

    mRetainedSnapshotChunks.resize(mSnapshotChunkColumns.size());
    std::size_t readyColumns = 0;
    for (std::size_t index = 0; index < mSnapshotChunkColumns.size(); ++index) {
        auto& chunk = mRetainedSnapshotChunks[index];
        if (!chunk) chunk = source->existingChunk(mSnapshotChunkColumns[index]);
        if (chunk) ++readyColumns;
    }

    if (!mSnapshotLevelChunksDispatched) {
        while (mSnapshotPacketIndex < mReplay.snapshotPackets.size()) {
            const auto& packet = mReplay.snapshotPackets[mSnapshotPacketIndex++];
            if (!isLevelChunkPacket(packet.id)) continue;
            auto decoded = native::createPacket(packet.id);
            if (!decoded || !native::deserialize(*decoded, packet.payload)) {
                setFailedLocked("Replay LevelChunk snapshot could not be decoded");
                return false;
            }
            const auto position = native::levelChunkPosition(*decoded);
            if (!position || !native::dispatch(decoded, mNetworkIdentifier, mNetworkCallback)) {
                setFailedLocked("Replay LevelChunk snapshot failed");
                return false;
            }
            const auto duplicate = std::find_if(
                mSnapshotChunkColumns.begin(),
                mSnapshotChunkColumns.end(),
                [&position](const auto& existing) {
                    return existing.x == position->x && existing.z == position->z;
                }
            );
            if (duplicate == mSnapshotChunkColumns.end()) {
                mSnapshotChunkColumns.push_back(*position);
            }
            if (budget.consumed(packet.payload.size())) return true;
        }
        mSnapshotPacketIndex = 0;
        if (mSnapshotChunkColumns.empty()) {
            setFailedLocked("Replay snapshot contains no LevelChunk columns");
            return false;
        }
        mSnapshotLevelChunksDispatched = true;
        mSnapshotChunkWaitTicks = 0;
        mSnapshotColumnsReadyTicks = 0;
        mStatus = "Preparing replay terrain";
        return true;
    }

    ++mSnapshotChunkWaitTicks;
    if (readyColumns != mSnapshotChunkColumns.size()) {
        mSnapshotColumnsReadyTicks = 0;
        if (mSnapshotChunkWaitTicks >= 1200) {
            setFailedLocked("Replay LevelChunk columns did not finish loading");
            return false;
        }
        mStatus = "Preparing replay terrain ("
            + std::to_string(readyColumns) + "/"
            + std::to_string(mSnapshotChunkColumns.size()) + ")";
        return true;
    }

    if (++mSnapshotColumnsReadyTicks < 2) return true;

    if (!mSnapshotSubChunksDispatched) {
        while (mSnapshotPacketIndex < mReplay.snapshotPackets.size()) {
            const auto& packet = mReplay.snapshotPackets[mSnapshotPacketIndex++];
            if (!isSubChunkPacket(packet.id)) continue;
            if (!dispatchPacketLocked(packet)) {
                setFailedLocked("Replay SubChunk snapshot packet failed");
                return false;
            }
            if (budget.consumed(packet.payload.size())) return true;
        }
        mSnapshotPacketIndex = 0;
        mSnapshotSubChunksDispatched = true;
        mSnapshotSettleTicks = 2;
        return true;
    }
    if (mSnapshotSettleTicks > 0) {
        --mSnapshotSettleTicks;
        return true;
    }

    const auto isPlayerListPacket = [](std::int32_t id) {
        return id == static_cast<std::int32_t>(MinecraftPacketIds::PlayerList);
    };
    if (!mSnapshotPlayerListApplied) {
        while (mSnapshotPacketIndex < mReplay.snapshotPackets.size()) {
            const auto& packet = mReplay.snapshotPackets[mSnapshotPacketIndex++];
            if (!isPlayerListPacket(packet.id)) continue;
            if (!dispatchPacketLocked(packet)) {
                setFailedLocked("Replay player-list snapshot failed");
                return false;
            }
            if (budget.consumed(packet.payload.size())) return true;
        }
        mSnapshotPacketIndex = 0;
        mSnapshotPlayerListApplied = true;
        mStatus = "Preparing replay entities";
        return true;
    }
    if (!mSnapshotRecordedPlayerApplied) {
        if (!mReplay.localPlayerPayload.empty()) {
            SerializedPacket recordedPlayer{static_cast<std::int32_t>(MinecraftPacketIds::AddPlayer), mReplay.localPlayerPayload};
            if (!dispatchPacketLocked(recordedPlayer)) {
                setFailedLocked("Replay recorded-player snapshot failed");
                return false;
            }
        }
        mSnapshotRecordedPlayerApplied = true;
        return true;
    }
    while (mSnapshotPacketIndex < mReplay.snapshotPackets.size()) {
        const auto& packet = mReplay.snapshotPackets[mSnapshotPacketIndex++];
        if (isChunkPacket(packet.id) || isPlayerListPacket(packet.id)) continue;
        if (!dispatchPacketLocked(packet)) {
            setFailedLocked("Replay snapshot packet failed: " + std::to_string(packet.id));
            return false;
        }
        if (budget.consumed(packet.payload.size())) return true;
    }
    mSnapshotPacketIndex = 0;
    ReplayEnvironmentState initialEnvironment{};
    initialEnvironment.setTime = true;
    initialEnvironment.setWeather = true;
    initialEnvironment.worldTime = mReplay.context.worldTime;
    initialEnvironment.rainLevel = mReplay.context.rainLevel;
    initialEnvironment.rainTarget = mReplay.context.rainTarget;
    initialEnvironment.lightningLevel = mReplay.context.lightningLevel;
    initialEnvironment.lightningTarget = mReplay.context.lightningTarget;
    if (!applyEnvironment(player, initialEnvironment)) {
        setFailedLocked("Replay environment snapshot failed");
        return false;
    }
    mReplayTime = initialEnvironment.worldTime;
    mManualTimeOverrideTicks = 0;
    placeCameraLocked(player);
    mSnapshotLoaded = true;
    mReplayLevel = player->level();
    mRenderRefreshPending = true;
    mSceneWarmupTicks = SceneWarmupTicks;
    mSceneWarmupTotalTicks = mSceneWarmupTicks;
    mState = State::LoadingSnapshot;
    mStatus = "Stabilizing replay scene";
    return true;
}

bool ReplayPlayer::resetSnapshotLocked(bedrocktools::sdk::Player* player) {
    if (!player || !player->level() || !mNetworkIdentifier || !mNetworkCallback) {
        setFailedLocked("Replay reset is unavailable");
        return false;
    }

    SnapshotBudget budget;
    if (mSnapshotResetPhase == SnapshotResetPhase::Idle) {
        mResetActorIds.clear();
        mResetActorIndex = 0;
        mSnapshotPacketIndex = 0;
        mResetPlayerApplied = false;
        for (auto* actor : native::runtimeActors(*player->level())) {
            if (!actor || actor == player) continue;
            const auto runtimeId = native::actorRuntimeId(*actor);
            const auto uniqueId = native::actorUniqueId(*actor);
            if (runtimeId == ReplayRecordedPlayerRuntimeId || uniqueId == ReplayRecordedPlayerUniqueId) continue;
            if (uniqueId != 0) mResetActorIds.push_back(uniqueId);
        }
        mPlayerViews.clear();
        mPlayerIdentities.clear();
        mPlayerIdentities[ReplayRecordedPlayerRuntimeId] = native::replayPlayerUuid();
        mSnapshotResetPhase = SnapshotResetPhase::RemovingActors;
    }
    if (mSnapshotResetPhase == SnapshotResetPhase::RemovingActors) {
        while (mResetActorIndex < mResetActorIds.size()) {
            const auto uniqueId = mResetActorIds[mResetActorIndex++];
            SerializedPacket remove{
                static_cast<std::int32_t>(MinecraftPacketIds::RemoveActor),
                encodeSignedVarInt64(uniqueId)
            };
            if (!dispatchPacketLocked(remove)) {
                setFailedLocked("Replay entity reset failed");
                return false;
            }
            if (budget.consumed(remove.payload.size())) return true;
        }
        mResetActorIds.clear();
        mSnapshotResetPhase = SnapshotResetPhase::LevelChunks;
        return true;
    }
    if (mSnapshotResetPhase == SnapshotResetPhase::LevelChunks) {
        while (mSnapshotPacketIndex < mReplay.snapshotPackets.size()) {
            const auto& packet = mReplay.snapshotPackets[mSnapshotPacketIndex++];
            if (!isLevelChunkPacket(packet.id)) continue;
            if (!dispatchPacketLocked(packet)) {
                setFailedLocked("Replay terrain reset failed");
                return false;
            }
            if (budget.consumed(packet.payload.size())) return true;
        }
        mSnapshotPacketIndex = 0;
        mSnapshotResetPhase = SnapshotResetPhase::WaitingForSubChunks;
        mSnapshotResetWaitTicks = 0;
        mState = mSnapshotResetForExport ? State::LoadingSnapshot : State::ResettingSnapshot;
        mStatus = mSnapshotResetForExport ? "Restoring replay terrain" : "Seeking replay";
        return true;
    }

    if (mSnapshotResetPhase == SnapshotResetPhase::WaitingForSubChunks) {
        if (++mSnapshotResetWaitTicks < 2) return true;
        auto* dimension = player->dimension();
        auto* region = dimension ? dimension->blockSource() : nullptr;
        auto* source = region ? region->chunkSource() : nullptr;
        if (!source) {
            setFailedLocked("Replay terrain source is unavailable");
            return false;
        }
        for (const auto position : mSnapshotChunkColumns) {
            if (!source->existingChunk(position)) {
                if (mSnapshotResetWaitTicks >= 1200) {
                    setFailedLocked("Replay terrain reset timed out");
                    return false;
                }
                return true;
            }
        }
        mSnapshotResetPhase = SnapshotResetPhase::ApplyingSubChunks;
        return true;
    }
    if (mSnapshotResetPhase == SnapshotResetPhase::ApplyingSubChunks) {
        while (mSnapshotPacketIndex < mReplay.snapshotPackets.size()) {
            const auto& packet = mReplay.snapshotPackets[mSnapshotPacketIndex++];
            if (!isSubChunkPacket(packet.id)) continue;
            if (!dispatchPacketLocked(packet)) {
                setFailedLocked("Replay subchunk reset failed");
                return false;
            }
            if (budget.consumed(packet.payload.size())) return true;
        }
        mSnapshotPacketIndex = 0;
        mSnapshotSettleTicks = 2;
        mSnapshotResetPhase = SnapshotResetPhase::ApplyingState;
        mStatus = "Restoring replay state";
        return true;
    }

    if (mSnapshotSettleTicks > 0) {
        --mSnapshotSettleTicks;
        return true;
    }
    if (!mResetPlayerApplied) {
        bedrocktools::sdk::Actor* recordedPlayer = nullptr;
        for (auto* actor : native::runtimeActors(*player->level())) {
            if (!actor || actor == player) continue;
            const auto runtimeId = native::actorRuntimeId(*actor);
            const auto uniqueId = native::actorUniqueId(*actor);
            if (runtimeId == ReplayRecordedPlayerRuntimeId || uniqueId == ReplayRecordedPlayerUniqueId) {
                recordedPlayer = actor;
                break;
            }
        }
        if (!recordedPlayer && !mReplay.localPlayerPayload.empty()) {
            SerializedPacket packet{
                static_cast<std::int32_t>(MinecraftPacketIds::AddPlayer),
                mReplay.localPlayerPayload
            };
            if (!dispatchPacketLocked(packet)) {
                setFailedLocked("Replay recorded-player reset failed");
                return false;
            }
        }
        ReplayMovementState recordedPlayerState{};
        recordedPlayerState.actorId = static_cast<std::int64_t>(ReplayRecordedPlayerRuntimeId);
        recordedPlayerState.x = mReplay.context.x;
        recordedPlayerState.y = mReplay.context.y;
        recordedPlayerState.z = mReplay.context.z;
        recordedPlayerState.pitch = mReplay.context.pitch;
        recordedPlayerState.yaw = mReplay.context.yaw;
        recordedPlayerState.headYaw = mReplay.context.yaw;
        recordedPlayerState.bodyYaw = mReplay.context.yaw;
        recordedPlayerState.glidingKnown = true;
        const std::array<ReplayMovementState, 1> recordedPlayerStates{recordedPlayerState};
        if (!applyMovementLocked(player, encodeMoveEntities(recordedPlayerStates))) {
            setFailedLocked("Replay recorded-player reset movement failed");
            return false;
        }
        mResetPlayerApplied = true;
        return true;
    }
    while (mSnapshotPacketIndex < mReplay.snapshotPackets.size()) {
        const auto& packet = mReplay.snapshotPackets[mSnapshotPacketIndex++];
        if (isChunkPacket(packet.id)
            || packet.id == static_cast<std::int32_t>(MinecraftPacketIds::PlayerList)) continue;
        if (!dispatchPacketLocked(packet)) {
            setFailedLocked("Replay snapshot reset failed: " + std::to_string(packet.id));
            return false;
        }
        if (budget.consumed(packet.payload.size())) return true;
    }
    mSnapshotPacketIndex = 0;

    ReplayEnvironmentState environment{};
    environment.setTime = true;
    environment.setWeather = true;
    environment.worldTime = mReplay.context.worldTime;
    environment.rainLevel = mReplay.context.rainLevel;
    environment.rainTarget = mReplay.context.rainTarget;
    environment.lightningLevel = mReplay.context.lightningLevel;
    environment.lightningTarget = mReplay.context.lightningTarget;
    if (!applyEnvironment(player, environment)) {
        setFailedLocked("Replay environment reset failed");
        return false;
    }

    mTimelineIndex = 0;
    mTick = 0;
    mTickAccumulator = 0.0f;
    mReplayTime = environment.worldTime;
    mManualTimeOverrideTicks = 0;
    mSnapshotResetPhase = SnapshotResetPhase::Idle;
    mSnapshotResetWaitTicks = 0;
    auto* client = ReplayWorldLauncher::get().clientInstance();
    const bool canRefresh = client
        && client->levelRenderer()
        && bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::RenderChunkCoordinatorSetAllDirty
        );
    mRenderRefreshPending = canRefresh;
    mSceneWarmupTicks = mSnapshotResetForExport
        ? (canRefresh ? ExportSceneWarmupTicks : SceneWarmupTicks)
        : (canRefresh ? InteractiveSceneWarmupTicks : SceneWarmupTicks);
    mSceneWarmupTotalTicks = mSceneWarmupTicks;
    mState = mSnapshotResetForExport ? State::LoadingSnapshot : State::ResettingSnapshot;
    mTickAdvancedAt = {};
    mFrozenRenderPartial = 1.0f;
    mEntityRenderHistories.clear();
    mRenderActorStates.clear();
    mInterpolatedLevel = nullptr;
    mInterpolatedTick = 0;
    mStatus = mSnapshotResetForExport ? "Stabilizing replay scene" : "Seeking replay";
    return true;
}

void ReplayPlayer::setFailedLocked(std::string message) {
    mSeekTarget.reset();
    mRewindTarget.reset();
    mRenderRefreshPending = false;
    mSceneWarmupTicks = 0;
    mSceneWarmupTotalTicks = 0;
    mExportClockActive = false;
    mExportFrameReady = false;
    mExportFrameGeneration = 0;
    mExportRenderedGeneration = 0;
    mState = State::Failed;
    mStatus = std::move(message);
}

void ReplayPlayer::resetLocked(std::string status) {
    content::release();
    mStartupConfigurationApplied.clear();
    mReplay = {};
    mPath.clear();
    mTimelineIndex = 0;
    mTick = 0;
    mTickAccumulator = 0.0f;
    mSeekTarget.reset();
    mRewindTarget.reset();
    mResumeAfterSeek = false;
    mReplayTime.reset();
    mManualTimeOverrideTicks = 0;
    mNetworkIdentifier = nullptr;
    mNetworkCallback = nullptr;
    mReplayLevelInitialized = false;
    mSnapshotLoaded = false;
    mSnapshotConfigurationApplied = false;
    mSnapshotPacketIndex = 0;
    mSnapshotRecordedPlayerApplied = false;
    mResetPlayerApplied = false;
    mResetActorIds.clear();
    mResetActorIndex = 0;
    mSnapshotSettleTicks = 0;
    mSnapshotLevelChunksDispatched = false;
    mSnapshotSubChunksDispatched = false;
    mSnapshotPlayerListApplied = false;
    mSnapshotChunkWaitTicks = 0;
    mSnapshotColumnsReadyTicks = 0;
    mSnapshotResetPhase = SnapshotResetPhase::Idle;
    mSnapshotResetWaitTicks = 0;
    mSnapshotResetForExport = false;
    mRenderRefreshPending = false;
    mSceneWarmupTicks = 0;
    mSceneWarmupTotalTicks = 0;
    mSnapshotChunkColumns.clear();
    mRetainedSnapshotChunks.clear();
    mSawHudClosedAfterLaunch = false;
    mLaunchPending = false;
    mStoppingTicks = 0;
    mStartupBegan = {};
    mTickAdvancedAt = {};
    mFrozenRenderPartial = -1.0f;
    mExportClockActive = false;
    mExportFrameReady = false;
    mExportRenderTick = 0.0;
    mExportTargetRenderTick = 0.0;
    mExportFrameGeneration = 0;
    mExportRenderedGeneration = 0;
    mLocalPlayer = nullptr;
    mReplayLevel = nullptr;
    mObserverCameraState.reset();
    mEntityRenderHistories.clear();
    mRenderActorStates.clear();
    mInterpolatedLevel = nullptr;
    mInterpolatedTick = 0;
    mRecordedPlayers.clear();
    mPlayerIdentities.clear();
    mPlayerViews.clear();
    mPlayerViewRenderPartial.reset();
    mPlayerIndexStage = 0;
    mPlayerIndexPosition = 0;
    mState = State::Idle;
    mStatus = std::move(status);
}

bool ReplayPlayer::advanceOneTickLocked(bedrocktools::sdk::Player* player) {
    if (mReplayTime) {
        if (mManualTimeOverrideTicks > 0) {
            if (const auto observed = readWorldTime(player)) {
                const auto delta = static_cast<std::int64_t>(*observed)
                    - static_cast<std::int64_t>(*mReplayTime);
                if (delta < -40 || delta > 40) {
                    mReplayTime = *observed;
                    mManualTimeOverrideTicks = 0;
                }
            }
            if (mManualTimeOverrideTicks > 0) --mManualTimeOverrideTicks;
        }
        if (!writeWorldTime(player, *mReplayTime)) {
            setFailedLocked("Replay time synchronization failed");
            return false;
        }
    }

    while (mTimelineIndex < mReplay.timeline.size() && mReplay.timeline[mTimelineIndex].tick <= mTick) {
        const auto& action = mReplay.timeline[mTimelineIndex++];
        if (action.action == ReplayAction::GamePacket || action.action == ReplayAction::ConfigurationPacket) {
            SerializedPacket packet;
            if (!decodePacket(action.payload, packet)) {
                setFailedLocked("Replay timeline packet failed");
                return false;
            }
            if (action.action == ReplayAction::ConfigurationPacket
                && isReplayWorldOwnedConfigurationPacket(packet.id)) continue;
            if (!dispatchPacketLocked(packet)) {
                setFailedLocked("Replay timeline packet failed");
                return false;
            }
        } else if (action.action == ReplayAction::MoveEntities) {
            if (!applyMovementLocked(player, action.payload)) {
                setFailedLocked("Replay movement action failed");
                return false;
            }
        } else if (action.action == ReplayAction::EnvironmentState) {
            ReplayEnvironmentState environment;
            if (!decodeEnvironmentState(action.payload, environment) || !applyEnvironment(player, environment)) {
                setFailedLocked("Replay environment action failed");
                return false;
            }
            if (environment.setTime) {
                mReplayTime = environment.worldTime;
                mManualTimeOverrideTicks = 0;
            }
        }
    }

    if (mTick >= mReplay.durationTicks && mTimelineIndex >= mReplay.timeline.size()) {
        mState = State::Finished;
        mStatus = "Replay finished";
        return false;
    }
    if (mReplayTime) ++*mReplayTime;
    ++mTick;
    mTickAdvancedAt = std::chrono::steady_clock::now();
    if (mState == State::Playing) mFrozenRenderPartial = -1.0f;
    return true;
}

bool ReplayPlayer::pinExportLightingLocked(bedrocktools::sdk::Player* player) {
    if (!mReplayTime || writeWorldTime(player, *mReplayTime)) return true;
    setFailedLocked("Replay export lighting synchronization failed");
    return false;
}

void ReplayPlayer::tick(bedrocktools::sdk::Player* player) {
    std::scoped_lock lock(mMutex);
    mLocalPlayer = player;
    if (player) {
        const auto position = player->position();
        const auto rotation = player->rotation();
        if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)
            && std::isfinite(rotation.x) && std::isfinite(rotation.y)) {
            mObserverCameraState = ObserverCameraState{position, rotation};
        }
    }
    if (mState == State::Idle || mState == State::Failed || mState == State::Stopping) return;
    try {
        if ((mState == State::StartingWorld || mState == State::LoadingSnapshot) && !mSnapshotLoaded) {
            if (const auto hudOpen = bedrockreplay::input::hudScreenOpen()) {
                if (!*hudOpen) mSawHudClosedAfterLaunch = true;
                if (!mSawHudClosedAfterLaunch || !*hudOpen) {
                    mStatus = "Waiting for the replay gameplay HUD";
                    return;
                }
            } else if (!mReplayLevelInitialized) {
                mStatus = "Waiting for the replay world";
                return;
            }
            loadSnapshotLocked(player);
            return;
        }
        if (!mSnapshotLoaded) return;

        if (mRewindTarget) {
            const auto target = std::min(*mRewindTarget, mReplay.durationTicks);
            if (!resetSnapshotLocked(player)) return;
            if (mSnapshotResetPhase != SnapshotResetPhase::Idle) return;
            mRewindTarget.reset();
            mSeekTarget = target;
            return;
        }

        if (mSceneWarmupTicks > 0) {
            if (mState == State::ResettingSnapshot
                && (mSceneWarmupTicks == InteractiveSceneWarmupTicks / 2 || mSceneWarmupTicks == 1)) {
                mRenderRefreshPending = true;
            }
            --mSceneWarmupTicks;
            if (mSceneWarmupTicks == 0) {
                mState = mSeekTarget && !mSnapshotResetForExport ? State::ResettingSnapshot
                    : mSeekTarget ? State::Paused : State::Playing;
                mTickAdvancedAt = std::chrono::steady_clock::now();
                mFrozenRenderPartial = mSeekTarget ? 1.0f : -1.0f;
                mStatus = mSeekTarget ? "Applying replay seek" : "Replay playing";
            }
            return;
        }

        if (mSeekTarget) {
            const auto target = std::min(*mSeekTarget, mReplay.durationTicks);
            const bool exportSeek = mSnapshotResetForExport;
            const auto seekWorkStarted = std::chrono::steady_clock::now();
            const std::uint32_t maximumSteps = exportSeek ? 2000u : 200u;
            std::uint32_t steps = 0;
            while (mTick < target && steps++ < maximumSteps && advanceOneTickLocked(player)) {
                if (exportSeek && steps >= 200
                    && std::chrono::steady_clock::now() - seekWorkStarted >= std::chrono::milliseconds(4)) break;
            }
            if (mTick >= target || mState == State::Failed || mState == State::Finished) {
                mSeekTarget.reset();
                if (mState != State::Failed && mState != State::Finished) {
                    mState = mResumeAfterSeek ? State::Playing : State::Paused;
                    snapRenderPosesLocked();
                    mTickAdvancedAt = std::chrono::steady_clock::now();
                    mFrozenRenderPartial = mState == State::Paused ? 1.0f : -1.0f;
                    mStatus = "Replay seek complete";
                }
                mResumeAfterSeek = false;
                mSnapshotResetForExport = false;
            }
            return;
        }

        if (mState != State::Playing) return;
        if (mExportClockActive) {
            if (!mExportFrameReady) {
                const auto targetTick = static_cast<std::uint32_t>(std::floor(mExportTargetRenderTick + 0.000001));
                while (mTick < targetTick && advanceOneTickLocked(player)) {}
                if (mState == State::Failed || mState == State::Finished) return;
                if (mTick >= targetTick) {
                    mExportRenderTick = mExportTargetRenderTick;
                    mExportFrameReady = true;
                    mFrozenRenderPartial = static_cast<float>(std::clamp(
                        mExportRenderTick - static_cast<double>(mTick), 0.0, 1.0
                    ));
                }
            }

            pinExportLightingLocked(player);
            return;
        }
        mTickAccumulator += mPlaybackSpeed;
        int steps = std::clamp(static_cast<int>(mTickAccumulator), 0, 8);
        mTickAccumulator -= static_cast<float>(steps);
        while (steps-- > 0 && advanceOneTickLocked(player)) {}
    } catch (const std::bad_alloc&) {
        setFailedLocked("Not enough memory to load this replay");
    }
}

float ReplayPlayer::renderPartialLocked() const {
    if (mExportClockActive) {
        return static_cast<float>(std::clamp(
            mExportRenderTick - static_cast<double>(mTick), 0.0, 1.0
        ));
    }
    if (mFrozenRenderPartial >= 0.0f) return std::clamp(mFrozenRenderPartial, 0.0f, 1.0f);
    if (mTickAdvancedAt == std::chrono::steady_clock::time_point{}) return 1.0f;
    const float elapsed = std::chrono::duration_cast<std::chrono::duration<float>>(
        std::chrono::steady_clock::now() - mTickAdvancedAt
    ).count();
    return std::clamp(elapsed * 20.0f * mPlaybackSpeed, 0.0f, 1.0f);
}

void ReplayPlayer::snapRenderPosesLocked() {
    for (auto& [runtimeId, history] : mEntityRenderHistories) history.previous = history.current;
}

bool ReplayPlayer::renderReadyLocked() const {
    return mReplayLevelInitialized && mSnapshotLoaded && mReplayLevel && mLocalPlayer
        && (mState == State::Playing || mState == State::Paused || mState == State::Finished
            || mState == State::LoadingSnapshot || mState == State::ResettingSnapshot);
}

bool ReplayPlayer::renderReady() const {
    std::scoped_lock lock(mMutex);
    return renderReadyLocked();
}

bool ReplayPlayer::replayWorldActive() const {
    std::scoped_lock lock(mMutex);
    if (!mReplayLevelInitialized || !mReplayLevel || !mLocalPlayer) return false;
    if (mState == State::Idle || mState == State::Failed || mState == State::Stopping) return false;
    auto* client = ReplayWorldLauncher::get().clientInstance();
    return client && client->localPlayer() == mLocalPlayer && mLocalPlayer->level() == mReplayLevel;
}

bool ReplayPlayer::rendererActive(void* renderer, bool levelRenderer) const {
    std::scoped_lock lock(mMutex);
    if (!renderer || !renderReadyLocked()) return false;
    auto* client = ReplayWorldLauncher::get().clientInstance();
    auto* player = client ? client->localPlayer() : nullptr;
    if (!player || player != mLocalPlayer || player->level() != mReplayLevel) return false;
    auto* active = client->levelRenderer();
    return active && (levelRenderer ? renderer == active : renderer == active->playerRenderer());
}

bool ReplayPlayer::ownsActor(const bedrocktools::sdk::Actor* actor) const {
    std::scoped_lock lock(mMutex);
    return actor && renderReadyLocked() && actor->level() == mReplayLevel;
}

bool ReplayPlayer::beginRenderInterpolation() {
    std::scoped_lock lock(mMutex);
    if (!renderReadyLocked() || mSeekTarget || mRewindTarget
        || (mState != State::Playing && mState != State::Paused && mState != State::Finished)
        || mEntityRenderHistories.empty() || !mRenderActorStates.empty()) return false;
    auto* client = ReplayWorldLauncher::get().clientInstance();
    auto* player = client ? client->localPlayer() : nullptr;
    if (!player || player != mLocalPlayer || player->level() != mReplayLevel) return false;
    mInterpolatedLevel = mReplayLevel;
    mInterpolatedTick = mTick;
    const float amount = mPlayerViewRenderPartial.value_or(renderPartialLocked());
    auto actors = native::runtimeActors(*mReplayLevel);
    mRenderActorStates.reserve(mEntityRenderHistories.size());
    for (auto* actor : actors) {
        if (!actor || actor == mLocalPlayer) continue;
        const auto runtimeId = native::actorRuntimeId(*actor);
        const auto history = mEntityRenderHistories.find(runtimeId);
        if (history == mEntityRenderHistories.end()) continue;
        const auto pose = interpolatePose(history->second, amount);
        if (!finitePose(pose)) continue;
        const bedrocktools::sdk::Vec3 movementDelta{
            history->second.current.position.x - history->second.previous.position.x,
            history->second.current.position.y - history->second.previous.position.y,
            history->second.current.position.z - history->second.previous.position.z
        };
        auto* stateVector = actor->stateVectorComponent();
        auto* actorRotation = actor->rotationComponent();
        auto* context = reinterpret_cast<EntityContext*>(actor->entityContext());
        if (!stateVector || !actorRotation || !context) continue;
        auto* aabbShape = bedrocktools::sdk::field<void*>(
            actor,
            bedrocktools::sdk::offsets::Actor::mStateVectorComponent
                + bedrocktools::sdk::offsets::BuiltInActorComponents::mAABBShapeComponent
        );
        auto* renderPosition = context->tryGetComponent<RenderPositionComponent>();
        auto* movementInterpolator = context->tryGetComponent<MovementInterpolatorComponent>();
        auto* renderRotation = context->tryGetComponent<RenderRotationComponent>();
        auto* headRotation = context->tryGetComponent<ActorHeadRotationComponent>();
        auto* bodyRotation = context->tryGetComponent<MobBodyRotationComponent>();
        history->second.renderStateIndex = mRenderActorStates.size();
        mRenderActorStates.push_back({
            stateVector,
            bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(stateVector, bedrocktools::sdk::offsets::StateVectorComponent::mPos),
            bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(stateVector, bedrocktools::sdk::offsets::StateVectorComponent::mPosPrev),
            bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(stateVector, bedrocktools::sdk::offsets::StateVectorComponent::mPosDelta),
            aabbShape,
            aabbShape ? bedrocktools::sdk::field<bedrocktools::sdk::AABB>(
                aabbShape,
                bedrocktools::sdk::offsets::AABBShapeComponent::mAABB
            ) : bedrocktools::sdk::AABB{},
            renderPosition,
            renderPosition ? renderPosition->mValue : bedrocktools::sdk::Vec3{},
            movementInterpolator,
            movementInterpolator ? movementInterpolator->mPos : bedrocktools::sdk::Vec3{},
            movementInterpolator ? movementInterpolator->mRot : bedrocktools::sdk::Vec2{},
            movementInterpolator ? movementInterpolator->mHeadYaw : 0.0f,
            movementInterpolator ? movementInterpolator->mPositionSteps : 0,
            movementInterpolator ? movementInterpolator->mRotationSteps : 0,
            movementInterpolator ? movementInterpolator->mHeadYawSteps : 0,
            renderRotation,
            renderRotation ? renderRotation->mRot : bedrocktools::sdk::Vec2{},
            actorRotation,
            bedrocktools::sdk::field<bedrocktools::sdk::Vec2>(actorRotation, bedrocktools::sdk::offsets::ActorRotationComponent::mRot),
            bedrocktools::sdk::field<bedrocktools::sdk::Vec2>(actorRotation, bedrocktools::sdk::offsets::ActorRotationComponent::mRotPrev),
            headRotation,
            headRotation ? headRotation->mYHeadRot : 0.0f,
            headRotation ? headRotation->mYHeadRotO : 0.0f,
            bodyRotation,
            bodyRotation ? bodyRotation->mYBodyRot : 0.0f,
            bodyRotation ? bodyRotation->mYBodyRotO : 0.0f,
            runtimeId,
            native::actorUniqueId(*actor)
        });
        bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(stateVector, bedrocktools::sdk::offsets::StateVectorComponent::mPos) = pose.position;
        bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(stateVector, bedrocktools::sdk::offsets::StateVectorComponent::mPosPrev) = {
            pose.position.x,
            pose.position.y,
            pose.position.z
        };
        bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(stateVector, bedrocktools::sdk::offsets::StateVectorComponent::mPosDelta) = movementDelta;
        if (aabbShape) {
            const bedrocktools::sdk::Vec3 delta{
                pose.position.x - mRenderActorStates.back().statePosition.x,
                pose.position.y - mRenderActorStates.back().statePosition.y,
                pose.position.z - mRenderActorStates.back().statePosition.z
            };
            auto bounds = mRenderActorStates.back().aabb;
            bounds.min = {
                bounds.min.x + delta.x,
                bounds.min.y + delta.y,
                bounds.min.z + delta.z
            };
            bounds.max = {
                bounds.max.x + delta.x,
                bounds.max.y + delta.y,
                bounds.max.z + delta.z
            };
            bedrocktools::sdk::field<bedrocktools::sdk::AABB>(
                aabbShape,
                bedrocktools::sdk::offsets::AABBShapeComponent::mAABB
            ) = bounds;
        }
        if (renderPosition) renderPosition->mValue = pose.position;
        if (movementInterpolator) {
            movementInterpolator->mPos = pose.position;
            movementInterpolator->mRot = {pose.pitch, pose.yaw};
            movementInterpolator->mHeadYaw = pose.headYaw;
            movementInterpolator->mPositionSteps = 0;
            movementInterpolator->mRotationSteps = 0;
            movementInterpolator->mHeadYawSteps = 0;
        }
        if (renderRotation) renderRotation->mRot = {pose.pitch, pose.yaw};
        bedrocktools::sdk::field<bedrocktools::sdk::Vec2>(actorRotation, bedrocktools::sdk::offsets::ActorRotationComponent::mRot) = {pose.pitch, pose.yaw};
        bedrocktools::sdk::field<bedrocktools::sdk::Vec2>(actorRotation, bedrocktools::sdk::offsets::ActorRotationComponent::mRotPrev) = {pose.pitch, pose.yaw};
        if (headRotation) headRotation->mYHeadRot = headRotation->mYHeadRotO = pose.headYaw;
        if (bodyRotation) bodyRotation->mYBodyRot = bodyRotation->mYBodyRotO = pose.bodyYaw;
    }
    return !mRenderActorStates.empty();
}

void ReplayPlayer::endRenderInterpolation() {
    std::scoped_lock lock(mMutex);
    if (mRenderActorStates.empty()) return;
    auto* client = ReplayWorldLauncher::get().clientInstance();
    auto* player = client ? client->localPlayer() : nullptr;
    if (!renderReadyLocked() || mReplayLevel != mInterpolatedLevel || mTick != mInterpolatedTick
        || !player || player != mLocalPlayer || player->level() != mReplayLevel) {
        mRenderActorStates.clear();
        mInterpolatedLevel = nullptr;
        mInterpolatedTick = 0;
        return;
    }
    const auto actors = native::runtimeActors(*mReplayLevel);
    for (auto* actor : actors) {
        if (!actor || actor == mLocalPlayer) continue;
        const auto runtimeId = native::actorRuntimeId(*actor);
        const auto history = mEntityRenderHistories.find(runtimeId);
        if (history == mEntityRenderHistories.end()) continue;
        const auto index = history->second.renderStateIndex;
        if (index >= mRenderActorStates.size()) continue;
        const auto* state = &mRenderActorStates[index];
        if (state->runtimeId != runtimeId || native::actorUniqueId(*actor) != state->uniqueId) continue;
        if (actor->stateVectorComponent() == state->stateVector) {
            bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(state->stateVector, bedrocktools::sdk::offsets::StateVectorComponent::mPos) = state->statePosition;
            bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(state->stateVector, bedrocktools::sdk::offsets::StateVectorComponent::mPosPrev) = state->statePreviousPosition;
            bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(state->stateVector, bedrocktools::sdk::offsets::StateVectorComponent::mPosDelta) = state->statePositionDelta;
        }
        auto* aabbShape = bedrocktools::sdk::field<void*>(actor,
            bedrocktools::sdk::offsets::Actor::mStateVectorComponent
                + bedrocktools::sdk::offsets::BuiltInActorComponents::mAABBShapeComponent);
        if (state->aabbShape && aabbShape == state->aabbShape) {
            bedrocktools::sdk::field<bedrocktools::sdk::AABB>(aabbShape,
                bedrocktools::sdk::offsets::AABBShapeComponent::mAABB) = state->aabb;
        }
        auto* context = reinterpret_cast<EntityContext*>(actor->entityContext());
        if (!context) continue;
        auto* renderPosition = context->tryGetComponent<RenderPositionComponent>();
        if (renderPosition && renderPosition == state->renderPosition) renderPosition->mValue = state->renderPositionValue;
        auto* movementInterpolator = context->tryGetComponent<MovementInterpolatorComponent>();
        if (movementInterpolator && movementInterpolator == state->movementInterpolator) {
            movementInterpolator->mPos = state->movementPosition;
            movementInterpolator->mRot = state->movementRotation;
            movementInterpolator->mHeadYaw = state->movementHeadYaw;
            movementInterpolator->mPositionSteps = state->movementPositionSteps;
            movementInterpolator->mRotationSteps = state->movementRotationSteps;
            movementInterpolator->mHeadYawSteps = state->movementHeadYawSteps;
        }
        auto* renderRotation = context->tryGetComponent<RenderRotationComponent>();
        if (renderRotation && renderRotation == state->renderRotation) renderRotation->mRot = state->renderRotationValue;
        if (actor->rotationComponent() == state->actorRotation) {
            bedrocktools::sdk::field<bedrocktools::sdk::Vec2>(state->actorRotation, bedrocktools::sdk::offsets::ActorRotationComponent::mRot) = state->actorRotationValue;
            bedrocktools::sdk::field<bedrocktools::sdk::Vec2>(state->actorRotation, bedrocktools::sdk::offsets::ActorRotationComponent::mRotPrev) = state->actorRotationPreviousValue;
        }
        auto* headRotation = context->tryGetComponent<ActorHeadRotationComponent>();
        if (headRotation && headRotation == state->headRotation) {
            headRotation->mYHeadRot = state->headYaw;
            headRotation->mYHeadRotO = state->previousHeadYaw;
        }
        auto* bodyRotation = context->tryGetComponent<MobBodyRotationComponent>();
        if (bodyRotation && bodyRotation == state->bodyRotation) {
            bodyRotation->mYBodyRot = state->bodyYaw;
            bodyRotation->mYBodyRotO = state->previousBodyYaw;
        }
    }
    mRenderActorStates.clear();
    mInterpolatedLevel = nullptr;
    mInterpolatedTick = 0;
}

void ReplayPlayer::rememberPlayerLocked(const PlayerViewId& id, std::string_view name, bool recordingPlayer) {
    auto it = std::find_if(mRecordedPlayers.begin(), mRecordedPlayers.end(),
        [&](const auto& entry) { return entry.id == id; });
    const auto displayName = playerDisplayName(name);
    if (it != mRecordedPlayers.end()) {
        if (!displayName.empty()) it->name = displayName;
        it->recordingPlayer = it->recordingPlayer || recordingPlayer;
    } else if (mRecordedPlayers.size() < 4096) {
        mRecordedPlayers.push_back({id, displayName.empty() ? "Unnamed player" : displayName, recordingPlayer, false});
    }
}

void ReplayPlayer::indexPlayerPacketLocked(const SerializedPacket& serialized) {
    if (serialized.id != static_cast<int>(MinecraftPacketIds::AddPlayer)
        && serialized.id != static_cast<int>(MinecraftPacketIds::PlayerList)) return;
    auto packet = native::createPacket(serialized.id);
    if (!packet || !native::deserialize(*packet, serialized.payload)) return;
    const bool recorded = serialized.id == static_cast<int>(MinecraftPacketIds::AddPlayer)
        && native::addPlayerRuntimeId(*packet) == ReplayRecordedPlayerRuntimeId;
    if (recorded && !native::rewriteAddPlayerIdentity(*packet,
        ReplayRecordedPlayerUniqueId, ReplayRecordedPlayerRuntimeId)) return;
    for (const auto& identity : native::playerIdentities(*packet)) {
        rememberPlayerLocked(identity.uuid, identity.name, recorded || identity.uuid == native::replayPlayerUuid());
    }
}

bool ReplayPlayer::indexPlayersLocked() {
    const auto began = std::chrono::steady_clock::now();
    std::size_t visited{};
    while (mPlayerIndexStage < 4) {
        if (mPlayerIndexStage == 0) {
            if (const auto packet = playerPacket(mReplay.localPlayerPayload)) indexPlayerPacketLocked(*packet);
            ++mPlayerIndexStage;
        } else if (mPlayerIndexStage == 1 || mPlayerIndexStage == 2) {
            const auto& packets = mPlayerIndexStage == 1 ? mReplay.configurationPackets : mReplay.snapshotPackets;
            if (mPlayerIndexPosition == packets.size()) {
                ++mPlayerIndexStage;
                mPlayerIndexPosition = 0;
                continue;
            }
            indexPlayerPacketLocked(packets[mPlayerIndexPosition++]);
        } else {
            if (mPlayerIndexPosition == mReplay.timeline.size()) {
                ++mPlayerIndexStage;
                mPlayerIndexPosition = 0;
                break;
            }
            const auto& action = mReplay.timeline[mPlayerIndexPosition++];
            if (action.action == ReplayAction::GamePacket || action.action == ReplayAction::ConfigurationPacket
                || action.action == ReplayAction::CreateLocalPlayer) {
                if (const auto packet = playerPacket(action.payload)) indexPlayerPacketLocked(*packet);
            }
        }
        if (++visited >= 512 || std::chrono::steady_clock::now() - began >= std::chrono::milliseconds(1)) return false;
    }
    return true;
}

void ReplayPlayer::refreshPlayerViews(bedrocktools::sdk::Player* player) {
    std::scoped_lock lock(mMutex);
    if (!player || player != mLocalPlayer || !mSnapshotLoaded || mSeekTarget || mRewindTarget
        || (mState != State::Playing && mState != State::Paused && mState != State::Finished)
        || !player->level()) {
        mPlayerViews.clear();
        return;
    }
    try {
        for (auto& [id, view] : mPlayerViews) view.refreshed = false;
        for (auto* actor : native::runtimeActors(*player->level())) {
            if (!actor || actor == player) continue;
            const auto runtimeId = native::actorRuntimeId(*actor);
            if (!runtimeId) continue;
            const auto identity = mPlayerIdentities.find(runtimeId);
            if (identity == mPlayerIdentities.end()
                && native::actorUniqueId(*actor) != ReplayRecordedPlayerUniqueId) continue;
            if (!native::actorIsPlayer(*actor)) continue;
            const auto id = identity == mPlayerIdentities.end() ? native::replayPlayerUuid() : identity->second;
            const auto historyId = id == native::replayPlayerUuid() ? ReplayRecordedPlayerRuntimeId : runtimeId;
            const auto current = actorPose(*actor);
            if (!finitePose(current)) continue;
            auto offset = bedrocktools::sdk::Vec3{0, 1.62f, 0};
            auto* context = reinterpret_cast<EntityContext*>(actor->entityContext());
            if (context) {
                if (const auto* offsets = context->tryGetComponent<OffsetsComponent>()) {
                    const auto& eye = offsets->mEyeOffset;
                    if (finiteVector(eye) && std::abs(eye.x) <= 8 && std::abs(eye.y) <= 8 && std::abs(eye.z) <= 8) offset = eye;
                }
            }
            auto previousOffset = offset;
            if (const auto old = mPlayerViews.find(id); old != mPlayerViews.end()
                && old->second.runtimeId == runtimeId) {
                previousOffset = old->second.tick == mTick ? old->second.previousEyeOffset : old->second.eyeOffset;
            }
            const auto history = mEntityRenderHistories.find(historyId);
            mPlayerViews.insert_or_assign(id, PlayerViewHistory{
                history == mEntityRenderHistories.end() ? EntityRenderHistory{current, current} : history->second,
                previousOffset, offset, runtimeId, mTick, true
            });
        }
        std::erase_if(mPlayerViews, [](const auto& entry) { return !entry.second.refreshed; });
    } catch (const std::bad_alloc&) {
        mPlayerViews.clear();
    }
}

std::vector<RecordedPlayerView> ReplayPlayer::recordedPlayers() const {
    std::scoped_lock lock(mMutex);
    auto result = mRecordedPlayers;
    for (auto& entry : result) entry.available = mPlayerViews.contains(entry.id);
    return result;
}

void ReplayPlayer::beginPlayerViewFrame() {
    std::scoped_lock lock(mMutex);
    mPlayerViewRenderPartial.reset();
}

std::optional<PlayerViewPose> ReplayPlayer::playerView(const PlayerViewId& id) {
    std::scoped_lock lock(mMutex);
    if (mSeekTarget || mRewindTarget || (mState != State::Playing && mState != State::Paused && mState != State::Finished)) return std::nullopt;
    const auto view = mPlayerViews.find(id);
    if (view == mPlayerViews.end()) return std::nullopt;
    if (!mPlayerViewRenderPartial) mPlayerViewRenderPartial = renderPartialLocked();
    const auto amount = *mPlayerViewRenderPartial;
    const auto pose = interpolatePose(view->second.pose, amount);
    const auto& previous = view->second.previousEyeOffset;
    const auto& current = view->second.eyeOffset;
    const auto eye = bedrocktools::sdk::Vec3{
        pose.position.x + std::lerp(previous.x, current.x, amount),
        pose.position.y + std::lerp(previous.y, current.y, amount),
        pose.position.z + std::lerp(previous.z, current.z, amount)
    };
    if (!finiteVector(eye) || !finitePose(pose)) return std::nullopt;
    return PlayerViewPose{eye, std::remainder(pose.headYaw, 360.0f), std::clamp(pose.pitch, -90.0f, 90.0f), view->second.runtimeId};
}

bedrocktools::sdk::Vec3 ReplayPlayer::clipPlayerCamera(const bedrocktools::sdk::Vec3& eye,
    const bedrocktools::sdk::Vec3& desired) const {
    std::scoped_lock lock(mMutex);
    if (!finiteVector(eye) || !finiteVector(desired) || !renderReadyLocked()
        || mSeekTarget || mRewindTarget) return eye;
    auto* client = ReplayWorldLauncher::get().clientInstance();
    auto* player = client ? client->localPlayer() : nullptr;
    if (!player || player != mLocalPlayer || player->level() != mReplayLevel) return eye;
    auto* dimension = mLocalPlayer->dimension();
    auto* source = dimension ? dimension->blockSource() : nullptr;
    if (!source) return eye;
    auto** table = *reinterpret_cast<void***>(source);
    if (!table || !table[bedrocktools::sdk::offsets::VTable::BlockSource_clip]) return eye;
    const bedrocktools::sdk::Vec3 delta{desired.x - eye.x, desired.y - eye.y, desired.z - eye.z};
    const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    if (!std::isfinite(length) || length < 0.001f || length > 8.0f) return eye;
    const bedrocktools::sdk::Vec3 direction{delta.x / length, delta.y / length, delta.z / length};
    using Predicate = std::function<bool(const bedrocktools::sdk::BlockSource&, const bedrocktools::sdk::Block&, bool)>;
    using ClipFn = CameraHitResult(*)(const bedrocktools::sdk::BlockSource*,
        const bedrocktools::sdk::Vec3&, const bedrocktools::sdk::Vec3&, bool, std::uint8_t,
        int, bool, bool, bedrocktools::sdk::Actor*, const Predicate&, bool);
    const auto clip = reinterpret_cast<ClipFn>(table[bedrocktools::sdk::offsets::VTable::BlockSource_clip]);
    const Predicate predicate;
    float distance = length;
    for (int corner = 0; corner < 8; ++corner) {
        const bedrocktools::sdk::Vec3 offset{
            (corner & 1) ? 0.1f : -0.1f, (corner & 2) ? 0.1f : -0.1f, (corner & 4) ? 0.1f : -0.1f};
        const bedrocktools::sdk::Vec3 start{eye.x + offset.x, eye.y + offset.y, eye.z + offset.z};
        const bedrocktools::sdk::Vec3 end{desired.x + offset.x, desired.y + offset.y, desired.z + offset.z};
        const auto hit = clip(source, start, end, false, 2, 16, false, false, nullptr, predicate, false);
        if (hit.type != 0 || !finiteVector(hit.position)) continue;
        const auto hitDistance = (hit.position.x - start.x) * direction.x
            + (hit.position.y - start.y) * direction.y + (hit.position.z - start.z) * direction.z;
        distance = std::min(distance, std::clamp(hitDistance - 0.15f, 0.0f, length));
    }
    return {eye.x + direction.x * distance, eye.y + direction.y * distance, eye.z + direction.z * distance};
}

void ReplayPlayer::onClientUpdate() {
    bool launchNow = false;
    bool leaveTimedOutWorld = false;
    bool refreshRenderChunks = false;
    {
        std::scoped_lock lock(mMutex);
        if (mState == State::StartingWorld && mLaunchPending) {
            mLaunchPending = false;
            mStatus = "Starting replay world";
            launchNow = true;
        }
        if (mSnapshotLoaded && mReplayLevelInitialized && mPlayerIndexStage < 4
            && !mSeekTarget && !mRewindTarget
            && (mState == State::LoadingSnapshot || mState == State::Playing
                || mState == State::Paused || mState == State::Finished)) {
            try {
                indexPlayersLocked();
            } catch (const std::bad_alloc&) {
                mPlayerIndexStage = 4;
            }
        }
        if (mRenderRefreshPending) {
            mRenderRefreshPending = false;
            refreshRenderChunks = true;
        }
    }

    if (refreshRenderChunks && !refreshReplayRenderChunks()) {
        std::scoped_lock lock(mMutex);
        if (mState == State::LoadingSnapshot || mState == State::ResettingSnapshot) {
            mRenderRefreshPending = true;
        }
    }

    if (launchNow) {
        std::string contentError;
        {
            std::scoped_lock lock(mMutex);
            if (mState != State::StartingWorld) return;
            if (!content::prepare(mReplay.configurationPackets, contentError)) {
                setFailedLocked(std::move(contentError));
                return;
            }
        }
        const bool launched = ReplayWorldLauncher::get().launch();
        std::scoped_lock lock(mMutex);
        if (!launched) {
            content::release();
            mStatus = ReplayWorldLauncher::get().status();
            mState = State::Failed;
            return;
        }
        if (mState == State::StartingWorld || mState == State::LoadingSnapshot) {
            mStartupBegan = std::chrono::steady_clock::now();
        }
    }

    {
        std::scoped_lock lock(mMutex);
        if ((mState == State::StartingWorld || mState == State::LoadingSnapshot) && !mSnapshotLoaded) {
            if (auto contentError = content::failure(); !contentError.empty()) {
                mState = State::Stopping;
                mStatus = std::move(contentError);
                mStoppingTicks = 0;
                leaveTimedOutWorld = true;
            } else if (mStartupBegan != std::chrono::steady_clock::time_point{}
                && std::chrono::steady_clock::now() - mStartupBegan >= std::chrono::seconds(120)) {
                if (mReplayLevelInitialized) {
                    mState = State::Stopping;
                    mStatus = "Replay startup timed out; leaving replay world";
                    mStoppingTicks = 0;
                    leaveTimedOutWorld = true;
                } else {
                    setFailedLocked("Replay startup timed out");
                }
            }
        }

        if (!leaveTimedOutWorld && mState == State::Stopping && !mReplayLevelInitialized) {
            auto* client = ReplayWorldLauncher::get().clientInstance();
            if (!client || !client->localPlayer()) {
                if (++mStoppingTicks >= 180) resetLocked("Replay stopped");
            }
        }
    }
    if (leaveTimedOutWorld) {
        ReplayWorldLauncher::get().scheduleCleanup();
        if (auto* client = ReplayWorldLauncher::get().clientInstance()) client->requestLeaveGameAsync();
    }
}

void ReplayPlayer::onLevelState(const bedrocktools::events::LevelStateEvent& event) {
    std::scoped_lock lock(mMutex);
    if (mState == State::Idle) return;
    if (event.phase == bedrocktools::events::LevelPhase::Initializing && event.levelId && std::string_view(*event.levelId).starts_with(ReplayLevelPrefix)) {
        mReplayLevelInitialized = true;
        mReplayLevel = static_cast<bedrocktools::sdk::Level*>(event.level);
        mStatus = "Replay world initialized";
    } else if (event.phase == bedrocktools::events::LevelPhase::Destroyed && mReplayLevelInitialized
        && event.level == mReplayLevel) {
        ReplayWorldLauncher::get().scheduleCleanup();
        resetLocked("Replay world closed");
    }
}

void ReplayPlayer::onPacketHandle(const bedrocktools::events::PacketHandleEvent& event) {
    std::scoped_lock lock(mMutex);
    if (mState != State::StartingWorld && mState != State::LoadingSnapshot) return;
    if (!event.networkIdentifier || !event.networkCallback) return;

    if (event.kind == bedrocktools::events::PacketHandleKind::ResourcePacksInfo) {
        std::memcpy(mNetworkIdentifierStorage.data(), event.networkIdentifier, mNetworkIdentifierStorage.size());
        mNetworkIdentifier = mNetworkIdentifierStorage.data();
        mNetworkCallback = event.networkCallback;
        mStatus = "Replay network handler ready";
    }

    if (!event.packet || !*event.packet) return;
    const auto id = event.kind == bedrocktools::events::PacketHandleKind::ResourcePacksInfo
        ? static_cast<std::int32_t>(MinecraftPacketIds::ResourcePacksInfo)
        : static_cast<std::int32_t>(MinecraftPacketIds::ResourcePackStack);
    if (auto replacement = content::handshakePacket(id)) *event.packet = std::move(replacement);
}

void ReplayPlayer::onConfigurationPacket(std::shared_ptr<bedrocktools::sdk::Packet>& packet) {
    std::scoped_lock lock(mMutex);
    if (!packet || (mState != State::StartingWorld && mState != State::LoadingSnapshot)
        || mSnapshotConfigurationApplied) return;
    const auto id = native::packetId(*packet);
    const auto* recorded = configurationPacketLocked(id);
    if (!recorded) return;
    auto replacement = native::createPacket(id);
    if (!replacement || !native::deserialize(*replacement, recorded->payload)) {
        content::reportFailure("Startup configuration packet cannot be decoded: " + std::to_string(id));
        return;
    }
    packet = std::move(replacement);
    if (std::find(mStartupConfigurationApplied.begin(), mStartupConfigurationApplied.end(), id)
        == mStartupConfigurationApplied.end()) mStartupConfigurationApplied.push_back(id);
}

void ReplayPlayer::requestManualTimeOverride() {
    std::scoped_lock lock(mMutex);
    if (mState == State::Playing || mState == State::Paused || mState == State::Finished) {
        mManualTimeOverrideTicks = 20;
    }
}

ReplayPlayer::State ReplayPlayer::state() const {
    std::scoped_lock lock(mMutex);
    return mState;
}

std::string ReplayPlayer::status() const {
    std::scoped_lock lock(mMutex);
    return mStatus;
}

std::filesystem::path ReplayPlayer::replayPath() const {
    std::scoped_lock lock(mMutex);
    return mPath;
}

std::uint32_t ReplayPlayer::currentTick() const {
    std::scoped_lock lock(mMutex);
    return mTick;
}

std::uint32_t ReplayPlayer::durationTicks() const {
    std::scoped_lock lock(mMutex);
    return mReplay.durationTicks;
}

double ReplayPlayer::renderTick() const {
    std::scoped_lock lock(mMutex);
    if (mExportClockActive) return std::min<double>(mReplay.durationTicks, mExportRenderTick);
    const double partial = mState == State::Playing ? renderPartialLocked() : 0.0;
    return std::min<double>(mReplay.durationTicks, static_cast<double>(mTick) + partial);
}

bedrocktools::sdk::Player* ReplayPlayer::localPlayer() const {
    std::scoped_lock lock(mMutex);
    return mLocalPlayer;
}

std::optional<ReplayPlayer::ObserverCameraState> ReplayPlayer::observerCameraState() const {
    std::scoped_lock lock(mMutex);
    return mObserverCameraState;
}

float ReplayPlayer::preparationProgress() const {
    std::scoped_lock lock(mMutex);
    if (mState != State::StartingWorld && mState != State::LoadingSnapshot) return 1.0f;
    if (mSnapshotResetPhase == SnapshotResetPhase::WaitingForSubChunks) {
        return 0.24f + std::min(mSnapshotResetWaitTicks, 2u) * 0.06f;
    }
    if (mSnapshotResetPhase == SnapshotResetPhase::ApplyingState) return 0.42f;
    if (mSceneWarmupTicks > 0) {
        const float remaining = static_cast<float>(mSceneWarmupTicks)
            / static_cast<float>(std::max(1u, mSceneWarmupTotalTicks));
        return 0.56f + (1.0f - remaining) * 0.44f;
    }
    if (!mSnapshotLevelChunksDispatched) return 0.06f;
    const auto readyColumns = std::count_if(mRetainedSnapshotChunks.begin(), mRetainedSnapshotChunks.end(),
        [](const auto& chunk) { return static_cast<bool>(chunk); });
    if (!mSnapshotChunkColumns.empty() && static_cast<std::size_t>(readyColumns) < mSnapshotChunkColumns.size()) {
        const float loaded = static_cast<float>(readyColumns);
        const float total = static_cast<float>(mSnapshotChunkColumns.size());
        return 0.12f + loaded / total * 0.24f;
    }
    if (!mSnapshotSubChunksDispatched) return 0.40f;
    if (!mSnapshotPlayerListApplied) return 0.48f;
    if (!mSnapshotLoaded) return 0.54f;
    return 1.0f;
}

}

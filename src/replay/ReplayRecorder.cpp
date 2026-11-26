#include "ReplayRecorder.hpp"

#include "NativePacket.hpp"
#include "ReplayContent.hpp"
#include "PacketLifecycle.hpp"
#include "ReplayEntityComponents.hpp"
#include <bedrocktools/sdk/world/ChunkSource.hpp>
#include <bedrocktools/sdk/input/MoveInput.hpp>
#include <bedrocktools/sdk/offsets/Network.hpp>
#include <bedrocktools/sdk/offsets/Content.hpp>
#include <bedrocktools/memory/Signatures.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <limits>
#include <random>
#include <sstream>

namespace bedrocktools::replay {
namespace {

using bedrocktools::sdk::MinecraftPacketIds;

std::int32_t packetValue(MinecraftPacketIds id) { return static_cast<std::int32_t>(id); }

bool isMovementPacket(MinecraftPacketIds id) {
    return id == MinecraftPacketIds::MovePlayer || id == MinecraftPacketIds::MoveAbsoluteActor;
}

bool isActorStatePacket(MinecraftPacketIds id) {
    switch (id) {
    case MinecraftPacketIds::MobEffect:
    case MinecraftPacketIds::UpdateAttributes:
    case MinecraftPacketIds::PlayerEquipment:
    case MinecraftPacketIds::MobArmorEquipment:
    case MinecraftPacketIds::SetActorData:
    case MinecraftPacketIds::SetActorMotion:
        return true;
    default:
        return false;
    }
}

bool referencesRecordedPlayer(
    const bedrocktools::sdk::Packet& packet,
    MinecraftPacketIds id,
    std::uint64_t uniqueId,
    std::uint64_t runtimeId
) {
    const auto* payload = static_cast<const std::byte*>(packet.payload());
    const auto equals = [&](std::size_t offset, std::uint64_t value) {
        std::uint64_t actual{};
        std::memcpy(&actual, payload + offset, sizeof(actual));
        return actual == value;
    };
    switch (id) {
    case MinecraftPacketIds::AddPlayer:
        return native::addPlayerRuntimeId(packet) == runtimeId;
    case MinecraftPacketIds::ActorEvent:
    case MinecraftPacketIds::MobEffect:
    case MinecraftPacketIds::UpdateAttributes:
    case MinecraftPacketIds::PlayerEquipment:
    case MinecraftPacketIds::MobArmorEquipment:
    case MinecraftPacketIds::SetActorData:
    case MinecraftPacketIds::SetActorMotion:
    case MinecraftPacketIds::Animate:
        return equals(0, runtimeId);
    case MinecraftPacketIds::TakeItemActor:
        return equals(0, runtimeId) || equals(8, runtimeId);
    case MinecraftPacketIds::PlayerAction:
        return equals(0x20, runtimeId);
    case MinecraftPacketIds::RemoveActor:
        return equals(0, uniqueId);
    case MinecraftPacketIds::SetActorLink:
        return equals(8, uniqueId) || equals(16, uniqueId);
    case MinecraftPacketIds::UpdatePlayerGameType:
        return equals(8, uniqueId);
    default:
        return false;
    }
}

std::optional<std::uint64_t> readVarUInt64(std::string_view payload, std::size_t& cursor) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64 && cursor < payload.size(); shift += 7) {
        const auto byte = static_cast<std::uint8_t>(payload[cursor++]);
        if (shift == 63 && (byte & 0xfeu) != 0) return std::nullopt;
        value |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0) return value;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> firstRuntimeId(std::string_view payload) {
    std::size_t cursor = 0;
    return readVarUInt64(payload, cursor);
}

constexpr std::uint64_t ReplayRecordedPlayerUniqueId =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - 1024);
constexpr std::uint64_t ReplayRecordedPlayerRuntimeId = std::uint64_t{1} << 62;
constexpr int ReplayWorldDimensionId = 0;

std::uint64_t chunkColumnKey(bedrocktools::sdk::ChunkPos position) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(position.x)) << 32)
        | static_cast<std::uint32_t>(position.z);
}

bedrocktools::sdk::ChunkPos playerChunkColumn(const bedrocktools::sdk::Vec3& playerPosition) {
    return {
        static_cast<int>(std::floor(playerPosition.x / 16.0f)),
        static_cast<int>(std::floor(playerPosition.z / 16.0f))
    };
}

template <int Radius>
const auto& nearbyChunkOffsets() {
    static const auto offsets = [] {
        std::array<bedrocktools::sdk::ChunkPos, (Radius * 2 + 1) * (Radius * 2 + 1)> positions{};
        std::size_t index = 0;
        for (int z = -Radius; z <= Radius; ++z) {
            for (int x = -Radius; x <= Radius; ++x) positions[index++] = {x, z};
        }
        std::sort(positions.begin(), positions.end(), [](const auto& left, const auto& right) {
            const auto leftDistance = left.x * left.x + left.z * left.z;
            const auto rightDistance = right.x * right.x + right.z * right.z;
            if (leftDistance != rightDistance) return leftDistance < rightDistance;
            if (left.x != right.x) return left.x < right.x;
            return left.z < right.z;
        });
        return positions;
    }();
    return offsets;
}

bool serializePortableChunk(
    bedrocktools::sdk::LevelChunk& chunk,
    bedrocktools::sdk::ChunkPos position,
    int dimensionId,
    bool requestMode,
    std::vector<SerializedPacket>& levelPackets,
    std::vector<SerializedPacket>& subChunkPackets
) {
    auto snapshot = requestMode
        ? native::createRequestModeChunkSnapshot(chunk, position, dimensionId)
        : native::createPortableChunkSnapshot(chunk, position, dimensionId);
    if (!snapshot || !snapshot->levelChunk) return false;

    std::string levelPayload;
    if (!native::serialize(*snapshot->levelChunk, levelPayload)) return false;
    std::string subChunkPayload;
    if (snapshot->subChunk) {
        if (!native::serialize(*snapshot->subChunk, subChunkPayload)) return false;
    }
    levelPackets.push_back({static_cast<int>(MinecraftPacketIds::FullChunkData), std::move(levelPayload)});
    if (snapshot->subChunk) {
        subChunkPackets.push_back({static_cast<int>(MinecraftPacketIds::SubChunkPacket), std::move(subChunkPayload)});
    }
    return true;
}

void appendChunkPackets(
    std::vector<SerializedPacket>& output,
    std::vector<SerializedPacket>& levelPackets,
    std::vector<SerializedPacket>& subChunkPackets
) {
    output.reserve(output.size() + levelPackets.size() + subChunkPackets.size());
    output.insert(
        output.end(),
        std::make_move_iterator(levelPackets.begin()),
        std::make_move_iterator(levelPackets.end())
    );
    output.insert(
        output.end(),
        std::make_move_iterator(subChunkPackets.begin()),
        std::make_move_iterator(subChunkPackets.end())
    );
}

std::optional<SerializedPacket> createRecordedSwingPacket(std::uint64_t runtimeId) {
    auto packet = native::createPacket(static_cast<int>(MinecraftPacketIds::Animate));
    if (!packet || !packet->payload()) return std::nullopt;
    auto* payload = static_cast<std::byte*>(packet->payload());
    std::memcpy(payload, &runtimeId, sizeof(runtimeId));
    payload[sizeof(runtimeId)] = std::byte{1};
    if (!native::validateRoundTrip(*packet)) return std::nullopt;
    std::string serialized;
    if (!native::serialize(*packet, serialized)) return std::nullopt;
    return SerializedPacket{static_cast<int>(MinecraftPacketIds::Animate), std::move(serialized)};
}

std::optional<SerializedPacket> createArmorStatePacket(bedrocktools::sdk::Actor& actor) {
    auto packet = native::createArmorEquipmentPacket(actor);
    if (!packet) return std::nullopt;
    std::string payload;
    if (!native::serialize(*packet, payload)) return std::nullopt;
    return SerializedPacket{
        static_cast<int>(MinecraftPacketIds::MobArmorEquipment),
        std::move(payload)
    };
}

std::vector<SerializedPacket> createHeldStatePackets(bedrocktools::sdk::Actor& actor) {
    std::vector<SerializedPacket> packets;
    packets.reserve(2);
    for (const bool offhand : {false, true}) {
        auto packet = native::createHeldEquipmentPacket(actor, offhand);
        std::string payload;
        if (packet && content::serializeReplayPacket(*packet, payload)) {
            packets.push_back({static_cast<int>(MinecraftPacketIds::PlayerEquipment), std::move(payload)});
        }
    }
    return packets;
}

}

ReplayRecorder& ReplayRecorder::get() {
    static ReplayRecorder recorder;
    return recorder;
}

void ReplayRecorder::setStatusLocked(std::string value) {
    mStatus = std::move(value);
}

std::filesystem::path ReplayRecorder::replayDirectory() const {
    auto base = mStorageDirectory;
    if (base.empty()) base = "/sdcard/games/BedrockReplay";
    return base / "replays";
}

std::string ReplayRecorder::replayFileName() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time, &tm);
    std::ostringstream out;
    out << "Replay " << std::put_time(&tm, "%Y-%m-%d %H-%M-%S") << ".playback";
    return out.str();
}

void ReplayRecorder::updateConfigurationLocked(const SerializedPacket& packet, MinecraftPacketIds id) {
    const auto semantics = describePacketLifecycle(id);
    if (semantics.startsConfigurationEpoch) {
        mConfigurationPackets.clear();
        mConfigurationLatest.clear();
        mChunkRequestModes.clear();
        mPlayerListSnapshotPackets.clear();
        mAddPlayerSnapshotPackets.clear();
        mSpawnSnapshotPackets.clear();
        mActorStateSnapshotPackets.clear();
        mPlayerSkinSnapshotPackets.clear();
        mActorUniqueToRuntimeId.clear();
        mActorRuntimeToUniqueId.clear();
        mRemovedActorRuntimeIds.clear();
    }
    if (semantics.keepsSequence()) {
        mConfigurationPackets.push_back(packet);
        return;
    }
    if (semantics.supersedes) {
        const auto superseded = packetValue(*semantics.supersedes);
        const auto found = mConfigurationLatest.find(superseded);
        if (found != mConfigurationLatest.end()) {
            mConfigurationPackets[found->second].payload.clear();
            mConfigurationLatest.erase(found);
        }
    }
    const auto found = mConfigurationLatest.find(packet.id);
    if (found != mConfigurationLatest.end()) {
        mConfigurationPackets[found->second] = packet;
    } else {
        mConfigurationLatest.emplace(packet.id, mConfigurationPackets.size());
        mConfigurationPackets.push_back(packet);
    }
}

bool ReplayRecorder::captureFreshLevelChunksLocked(
    bedrocktools::sdk::Player& player,
    int dimensionId,
    std::vector<SerializedPacket>& packets
) {
    packets.clear();
    auto* dimension = player.dimension();
    auto* region = dimension ? dimension->blockSource() : nullptr;
    auto* source = region ? region->chunkSource() : nullptr;
    if (!source) return false;

    const auto mode = mChunkRequestModes.find(region->dimensionId());
    const bool requestMode = mode != mChunkRequestModes.end() && mode->second;

    const auto center = playerChunkColumn(player.position());
    const auto& positions = nearbyChunkOffsets<4>();

    std::vector<SerializedPacket> levelPackets;
    std::vector<SerializedPacket> subChunkPackets;
    levelPackets.reserve(positions.size());
    subChunkPackets.reserve(positions.size());
    for (const auto offset : positions) {
        const bedrocktools::sdk::ChunkPos position{center.x + offset.x, center.z + offset.z};
        auto chunk = source->existingChunk(position);
        if (!chunk) continue;
        if (!serializePortableChunk(*chunk, position, dimensionId, requestMode, levelPackets, subChunkPackets)) continue;
        mCapturedChunkColumns.emplace(chunkColumnKey(position));
    }
    appendChunkPackets(packets, levelPackets, subChunkPackets);
    return !packets.empty();
}

bool ReplayRecorder::captureNewLevelChunksLocked(
    bedrocktools::sdk::Player& player,
    int dimensionId,
    std::vector<SerializedPacket>& packets
) {
    packets.clear();
    auto* dimension = player.dimension();
    auto* region = dimension ? dimension->blockSource() : nullptr;
    auto* source = region ? region->chunkSource() : nullptr;
    if (!source) return false;

    const auto mode = mChunkRequestModes.find(region->dimensionId());
    const bool requestMode = mode != mChunkRequestModes.end() && mode->second;

    std::vector<SerializedPacket> levelPackets;
    std::vector<SerializedPacket> subChunkPackets;
    const auto center = playerChunkColumn(player.position());
    if (!mChunkCaptureCenter || mChunkCaptureCenter->x != center.x || mChunkCaptureCenter->z != center.z) {
        mChunkCaptureCenter = center;
        mChunkCaptureCursor = 0;
    }
    const auto& offsets = nearbyChunkOffsets<8>();
    constexpr std::size_t maxScansPerTick = 32;
    constexpr std::size_t maxSerializationAttemptsPerTick = 2;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
    std::size_t attempts = 0;
    for (std::size_t scanned = 0; scanned < maxScansPerTick; ++scanned) {
        if (attempts >= maxSerializationAttemptsPerTick || std::chrono::steady_clock::now() >= deadline) break;
        const auto offset = offsets[mChunkCaptureCursor];
        mChunkCaptureCursor = (mChunkCaptureCursor + 1) % offsets.size();
        const bedrocktools::sdk::ChunkPos position{center.x + offset.x, center.z + offset.z};
        const auto key = chunkColumnKey(position);
        if (mCapturedChunkColumns.contains(key)) continue;
        auto chunk = source->existingChunk(position);
        if (!chunk) continue;
        ++attempts;
        if (!serializePortableChunk(*chunk, position, dimensionId, requestMode, levelPackets, subChunkPackets)) continue;
        mCapturedChunkColumns.emplace(key);
    }
    appendChunkPackets(packets, levelPackets, subChunkPackets);
    return true;
}

void ReplayRecorder::appendActorStateLocked(
    std::uint64_t runtimeId,
    std::vector<SerializedPacket>& packets,
    bool includeArmor,
    bool includeHeldEquipment
) const {
    const auto found = mActorStateSnapshotPackets.find(runtimeId);
    if (found == mActorStateSnapshotPackets.end()) return;
    for (const auto& packet : found->second) {
        if (!includeArmor && packet.id == static_cast<int>(MinecraftPacketIds::MobArmorEquipment)) continue;
        if (!includeHeldEquipment && packet.id == static_cast<int>(MinecraftPacketIds::PlayerEquipment)) continue;
        packets.push_back(packet);
    }
}

bool ReplayRecorder::captureFreshActorsLocked(
    bedrocktools::sdk::Player& player,
    std::vector<SerializedPacket>& packets,
    std::vector<SerializedPacket>& skinPackets
) {
    packets.clear();
    skinPackets.clear();
    auto* level = player.level();
    if (!level) return false;
    mCapturedActorRuntimeIds.clear();
    mCapturedArmorRuntimeIds.clear();
    mCapturedHeldRuntimeIds.clear();
    mActorUniqueToRuntimeId.clear();
    mActorRuntimeToUniqueId.clear();
    mRemovedActorRuntimeIds.clear();
    const auto localRuntimeId = native::actorRuntimeId(player);
    const auto localUniqueId = native::actorUniqueId(player);
    auto actors = native::runtimeActors(*level);
    packets.reserve(actors.size() * 2);
    for (auto* actor : actors) {
        if (!actor || actor == &player) continue;
        const auto runtimeId = native::actorRuntimeId(*actor);
        if (!runtimeId || runtimeId == localRuntimeId) continue;
        if (localUniqueId && native::actorUniqueId(*actor) == localUniqueId) continue;
        const bool playerActor = native::actorIsPlayer(*actor);

        SerializedPacket serialized;
        bool captured = false;
        if (auto packet = native::tryCreateAddActorPacket(*actor)) {
            const int id = native::packetId(*packet);
            if (id >= 0 && id < static_cast<int>(MinecraftPacketIds::EndId)) {
                std::string payload;
                if (content::serializeReplayPacket(*packet, payload)) {
                    serialized = {id, std::move(payload)};
                    captured = true;
                }
            }
        }
        if (!captured && playerActor) {
            const auto cached = mAddPlayerSnapshotPackets.find(runtimeId);
            if (cached != mAddPlayerSnapshotPackets.end()) {
                serialized = cached->second;
                captured = true;
            }
        }
        if (!captured) {
            const auto cached = mSpawnSnapshotPackets.find(runtimeId);
            if (cached != mSpawnSnapshotPackets.end()) {
                serialized = cached->second;
                captured = true;
            }
        }
        if (!captured) continue;
        if (serialized.id == static_cast<int>(MinecraftPacketIds::AddPlayer)) {
            auto addPlayer = native::createPacket(serialized.id);
            if (addPlayer && native::deserialize(*addPlayer, serialized.payload)) {
                if (const auto uuid = native::addPlayerUuid(*addPlayer)) {
                    if (auto skin = native::createPlayerSkinPacket(*actor, *uuid)) {
                        std::string payload;
                        if (native::serialize(*skin, payload)) {
                            skinPackets.push_back({static_cast<int>(MinecraftPacketIds::PlayerSkin), std::move(payload)});
                        }
                    }
                }
            }
        }
        std::optional<SerializedPacket> armor;
        if (playerActor) armor = createArmorStatePacket(*actor);
        auto held = playerActor ? createHeldStatePackets(*actor) : std::vector<SerializedPacket>{};
        packets.push_back(std::move(serialized));
        appendActorStateLocked(runtimeId, packets, !armor.has_value(), held.size() != 2);
        packets.insert(packets.end(), std::make_move_iterator(held.begin()), std::make_move_iterator(held.end()));
        if (playerActor) mCapturedHeldRuntimeIds.emplace(runtimeId);
        if (armor) {
            packets.push_back(std::move(*armor));
            mCapturedArmorRuntimeIds.emplace(runtimeId);
        }
        mCapturedActorRuntimeIds.emplace(runtimeId);
        if (const auto uniqueId = native::actorUniqueId(*actor)) rememberActorIdentityLocked(uniqueId, runtimeId);
    }
    return true;
}

bool ReplayRecorder::captureNewActorsLocked(
    bedrocktools::sdk::Player& player,
    const std::vector<bedrocktools::sdk::Actor*>& actors,
    std::vector<SerializedPacket>& packets,
    std::vector<SerializedPacket>& statePackets
) {
    packets.clear();
    statePackets.clear();
    auto* level = player.level();
    if (!level) return false;
    const auto localRuntimeId = native::actorRuntimeId(player);
    const auto localUniqueId = native::actorUniqueId(player);
    std::unordered_set<std::uint64_t> removedActorsStillPresent;
    for (auto* actor : actors) {
        if (!actor || actor == &player) continue;
        const auto runtimeId = native::actorRuntimeId(*actor);
        if (!runtimeId || runtimeId == localRuntimeId) continue;
        if (mRemovedActorRuntimeIds.contains(runtimeId)) {
            removedActorsStillPresent.emplace(runtimeId);
            continue;
        }
        if (localUniqueId && native::actorUniqueId(*actor) == localUniqueId) continue;
        const bool playerActor = native::actorIsPlayer(*actor);
        if (mCapturedActorRuntimeIds.contains(runtimeId)) {
            if (playerActor && !mCapturedHeldRuntimeIds.contains(runtimeId)) {
                auto held = createHeldStatePackets(*actor);
                statePackets.insert(statePackets.end(), std::make_move_iterator(held.begin()), std::make_move_iterator(held.end()));
                mCapturedHeldRuntimeIds.emplace(runtimeId);
            }
            if (playerActor && !mCapturedArmorRuntimeIds.contains(runtimeId)) {
                if (auto armor = createArmorStatePacket(*actor)) {
                    statePackets.push_back(std::move(*armor));
                    mCapturedArmorRuntimeIds.emplace(runtimeId);
                }
            }
            continue;
        }

        SerializedPacket serialized;
        bool captured = false;
        if (auto packet = native::tryCreateAddActorPacket(*actor)) {
            const int id = native::packetId(*packet);
            if (id >= 0 && id < static_cast<int>(MinecraftPacketIds::EndId)) {
                std::string payload;
                if (content::serializeReplayPacket(*packet, payload)) {
                    serialized = {id, std::move(payload)};
                    captured = true;
                }
            }
        }
        if (!captured && playerActor) {
            const auto cached = mAddPlayerSnapshotPackets.find(runtimeId);
            if (cached != mAddPlayerSnapshotPackets.end()) {
                serialized = cached->second;
                captured = true;
            }
        }
        if (!captured) {
            const auto cached = mSpawnSnapshotPackets.find(runtimeId);
            if (cached != mSpawnSnapshotPackets.end()) {
                serialized = cached->second;
                captured = true;
            }
        }
        if (!captured) continue;
        std::optional<SerializedPacket> armor;
        if (playerActor) armor = createArmorStatePacket(*actor);
        auto held = playerActor ? createHeldStatePackets(*actor) : std::vector<SerializedPacket>{};
        packets.push_back(std::move(serialized));
        appendActorStateLocked(runtimeId, packets, !armor.has_value(), held.size() != 2);
        packets.insert(packets.end(), std::make_move_iterator(held.begin()), std::make_move_iterator(held.end()));
        if (playerActor) mCapturedHeldRuntimeIds.emplace(runtimeId);
        if (armor) {
            packets.push_back(std::move(*armor));
            mCapturedArmorRuntimeIds.emplace(runtimeId);
        }
        mCapturedActorRuntimeIds.emplace(runtimeId);
        if (const auto uniqueId = native::actorUniqueId(*actor)) rememberActorIdentityLocked(uniqueId, runtimeId);
    }
    mRemovedActorRuntimeIds = std::move(removedActorsStillPresent);
    return true;
}

bool ReplayRecorder::observeSpawnedActor(const bedrocktools::sdk::Packet& packet) {
    const int id = native::packetId(packet);
    if (id != static_cast<int>(MinecraftPacketIds::AddActor)
        && id != static_cast<int>(MinecraftPacketIds::AddItemActor)) return false;
    std::string payload;
    if (!content::serializeReplayPacket(packet, payload)) return false;
    std::size_t cursor = 0;
    const auto encodedUniqueId = readVarUInt64(payload, cursor);
    const auto runtimeId = readVarUInt64(payload, cursor);
    if (!encodedUniqueId || !runtimeId || !*runtimeId) return false;
    const auto uniqueId = (*encodedUniqueId >> 1) ^ (std::uint64_t{0} - (*encodedUniqueId & 1));
    SerializedPacket serialized{id, std::move(payload)};

    std::scoped_lock lock(mMutex);
    mSpawnSnapshotPackets.insert_or_assign(*runtimeId, serialized);
    mActorStateSnapshotPackets.erase(*runtimeId);
    rememberActorIdentityLocked(uniqueId, *runtimeId);
    mRemovedActorRuntimeIds.erase(*runtimeId);
    const auto captureState = mState.load(std::memory_order_relaxed);
    if (captureState != State::Recording && captureState != State::Paused) return true;
    mCapturedActorRuntimeIds.emplace(*runtimeId);
    if (captureState == State::Recording) mPendingPackets.push_back(std::move(serialized));
    else mPausedPackets.push_back(std::move(serialized));
    return true;
}

void ReplayRecorder::rememberActorIdentityLocked(std::uint64_t uniqueId, std::uint64_t runtimeId) {
    if (const auto previous = mActorRuntimeToUniqueId.find(runtimeId); previous != mActorRuntimeToUniqueId.end()) {
        mActorUniqueToRuntimeId.erase(previous->second);
    }
    if (const auto previous = mActorUniqueToRuntimeId.find(uniqueId); previous != mActorUniqueToRuntimeId.end()) {
        if (previous->second != runtimeId) retireActorLocked(previous->second);
    }
    mActorUniqueToRuntimeId.insert_or_assign(uniqueId, runtimeId);
    mActorRuntimeToUniqueId.insert_or_assign(runtimeId, uniqueId);
}

void ReplayRecorder::retireActorLocked(std::uint64_t runtimeId) {
    if (const auto identity = mActorRuntimeToUniqueId.find(runtimeId); identity != mActorRuntimeToUniqueId.end()) {
        mActorUniqueToRuntimeId.erase(identity->second);
        mActorRuntimeToUniqueId.erase(identity);
    }
    mSpawnSnapshotPackets.erase(runtimeId);
    mAddPlayerSnapshotPackets.erase(runtimeId);
    mActorStateSnapshotPackets.erase(runtimeId);
    mCapturedActorRuntimeIds.erase(runtimeId);
    mCapturedArmorRuntimeIds.erase(runtimeId);
    mCapturedHeldRuntimeIds.erase(runtimeId);
    mLastEntityMovements.erase(runtimeId);
    const auto captureState = mState.load(std::memory_order_relaxed);
    if (captureState == State::Recording || captureState == State::Paused) mRemovedActorRuntimeIds.emplace(runtimeId);
}

void ReplayRecorder::observeLocalPlayerSwing() {
    const auto current = mState.load(std::memory_order_acquire);
    if (current != State::Recording && current != State::Paused) return;
    auto swing = createRecordedSwingPacket(ReplayRecordedPlayerRuntimeId);
    if (!swing) return;
    std::scoped_lock lock(mMutex);
    const auto captureState = mState.load(std::memory_order_relaxed);
    if (captureState == State::Recording) mPendingPackets.push_back(std::move(*swing));
    else if (captureState == State::Paused) mPausedPackets.push_back(std::move(*swing));
}

void ReplayRecorder::observeBlockChange(
    bedrocktools::sdk::BlockSource* source,
    const bedrocktools::sdk::BlockPos& position,
    int layer,
    const void* block,
    const void* previousBlock,
    std::uint32_t flags
) {
    const auto current = mState.load(std::memory_order_acquire);
    if ((current != State::Recording && current != State::Paused)
        || !source || source != mCurrentBlockSource.load(std::memory_order_acquire)
        || !block || block == previousBlock || layer < 0 || layer > 1) return;
    constexpr auto hashOffset = bedrocktools::sdk::offsets::ReplayContent::BlockNetworkHash;
    const auto hash = bedrocktools::sdk::field<std::uint32_t>(block, hashOffset);
    if (previousBlock && bedrocktools::sdk::field<std::uint32_t>(previousBlock, hashOffset) == hash) return;
    auto packet = native::createBlockUpdatePacket(position, hash, static_cast<std::uint32_t>(layer),
        static_cast<std::uint8_t>((flags & 8u) | 2u));
    std::string payload;
    if (!packet || !native::serialize(*packet, payload)) return;
    SerializedPacket serialized{packetValue(MinecraftPacketIds::UpdateBlock), std::move(payload)};
    std::scoped_lock lock(mMutex);
    if (source != mCurrentBlockSource.load(std::memory_order_relaxed)) return;
    const auto captureState = mState.load(std::memory_order_relaxed);
    if (captureState == State::Recording) mPendingPackets.push_back(std::move(serialized));
    else if (captureState == State::Paused) mPausedPackets.push_back(std::move(serialized));
}

bool ReplayRecorder::observeClientPacket(const bedrocktools::sdk::Packet& packet, bool preserveTimeline) {
    const auto current = mState.load(std::memory_order_acquire);

    const int rawId = native::packetId(packet);
    if (rawId < 0 || rawId >= static_cast<int>(MinecraftPacketIds::EndId)) return false;
    const auto id = static_cast<MinecraftPacketIds>(rawId);
    if (id == MinecraftPacketIds::FullChunkData) {
        const auto dimensionId = bedrocktools::sdk::field<std::int32_t>(
            &packet, bedrocktools::sdk::offsets::LevelChunkPacket::mDimensionId
        );
        const bool requestMode = bedrocktools::sdk::field<bool>(
            &packet, bedrocktools::sdk::offsets::LevelChunkPacket::mClientNeedsToRequestSubchunks
        );
        std::scoped_lock lock(mMutex);
        if (mChunkRequestModes.contains(dimensionId) || mChunkRequestModes.size() < 64) {
            mChunkRequestModes.insert_or_assign(dimensionId, requestMode);
        }
        return false;
    }
    if (id == MinecraftPacketIds::SubChunkPacket) return false;
    if (id == MinecraftPacketIds::AddActor || id == MinecraftPacketIds::AddItemActor) return false;
    const auto semantics = describePacketLifecycle(id);
    if (semantics.lifecycle == PacketLifecycle::Ignore) return false;
    const bool playerListSnapshot = id == MinecraftPacketIds::PlayerList;
    const bool addPlayerSnapshot = id == MinecraftPacketIds::AddPlayer;
    const bool playerSkinSnapshot = id == MinecraftPacketIds::PlayerSkin;
    const bool actorStateSnapshot = isActorStatePacket(id);

    if (current != State::Recording && current != State::Paused
        && !semantics.isConfiguration() && !playerListSnapshot && !addPlayerSnapshot
        && !playerSkinSnapshot && !actorStateSnapshot
        && id != MinecraftPacketIds::RemoveActor && id != MinecraftPacketIds::TakeItemActor) return false;

    std::string payload;
    if (!content::serializeReplayPacket(packet, payload)) return false;
    SerializedPacket serialized{rawId, std::move(payload)};
    if (isMovementPacket(id)) return false;

    std::scoped_lock lock(mMutex);
    const auto captureState = mState.load(std::memory_order_relaxed);

    if (id == MinecraftPacketIds::RemoveActor) {
        if (const auto encodedUniqueId = firstRuntimeId(serialized.payload)) {
            const auto uniqueId = (*encodedUniqueId >> 1) ^ (std::uint64_t{0} - (*encodedUniqueId & 1));
            const auto found = mActorUniqueToRuntimeId.find(uniqueId);
            if (found != mActorUniqueToRuntimeId.end()) {
                retireActorLocked(found->second);
            }
        }
    } else if (id == MinecraftPacketIds::TakeItemActor) {
        if (const auto runtimeId = firstRuntimeId(serialized.payload)) retireActorLocked(*runtimeId);
    }

    if (playerSkinSnapshot) {
        if (mPlayerSkinSnapshotPackets.empty() || mPlayerSkinSnapshotPackets.back().payload != serialized.payload) {
            mPlayerSkinSnapshotPackets.push_back(serialized);
            if (mPlayerSkinSnapshotPackets.size() > 64) mPlayerSkinSnapshotPackets.erase(mPlayerSkinSnapshotPackets.begin());
        }
        if (captureState != State::Recording && captureState != State::Paused) return false;
    }

    if (actorStateSnapshot) {
        if (const auto runtimeId = firstRuntimeId(serialized.payload); runtimeId && *runtimeId) {
            auto& statePackets = mActorStateSnapshotPackets[*runtimeId];
            const auto previous = std::find_if(statePackets.rbegin(), statePackets.rend(), [&serialized](const auto& entry) {
                return entry.id == serialized.id;
            });
            const bool unchanged = previous != statePackets.rend() && previous->payload == serialized.payload;
            if (!unchanged) {
                statePackets.push_back(serialized);
                if (statePackets.size() > 64) statePackets.erase(statePackets.begin());
            }
            if (unchanged && !preserveTimeline) return false;
            if (id == MinecraftPacketIds::MobArmorEquipment
                && (captureState == State::Recording || captureState == State::Paused)) {
                mCapturedArmorRuntimeIds.emplace(*runtimeId);
            }
        }
        if (captureState != State::Recording && captureState != State::Paused) return false;
    }

    if (playerListSnapshot) {
        if (!serialized.payload.empty() && static_cast<std::uint8_t>(serialized.payload.front()) == 0) {
            mPlayerListSnapshotPackets.push_back(serialized);
        }
        if (captureState != State::Recording && captureState != State::Paused) return false;
    }

    if (addPlayerSnapshot) {
        const auto runtimeId = native::addPlayerRuntimeId(packet);
        if (runtimeId) {
            mAddPlayerSnapshotPackets.insert_or_assign(runtimeId, serialized);
            mActorStateSnapshotPackets.erase(runtimeId);
            const auto uniqueId = bedrocktools::sdk::field<std::uint64_t>(&packet, bedrocktools::sdk::offsets::AddPlayerPacket::mUniqueId);
            rememberActorIdentityLocked(uniqueId, runtimeId);
            mRemovedActorRuntimeIds.erase(runtimeId);
            if (captureState == State::Recording || captureState == State::Paused) {
                mCapturedActorRuntimeIds.emplace(runtimeId);
                mCapturedArmorRuntimeIds.erase(runtimeId);
                mCapturedHeldRuntimeIds.erase(runtimeId);
            }
        }
        if (captureState != State::Recording && captureState != State::Paused) return false;
    }

    if (semantics.isConfiguration()) {
        updateConfigurationLocked(serialized, id);
        if (captureState == State::Recording) mPendingPackets.push_back(serialized);
        else if (captureState == State::Paused) mPausedPackets.push_back(serialized);
        return captureState == State::Recording || captureState == State::Paused;
    }

    if (captureState != State::Recording && captureState != State::Paused) return false;

    if (mSourceLocalUniqueId && mSourceLocalRuntimeId && mRecordedLocalRuntimeId
        && referencesRecordedPlayer(packet, id, *mSourceLocalUniqueId, *mSourceLocalRuntimeId)) {
        auto remapped = native::createPacket(rawId);
        if (remapped && native::deserialize(*remapped, serialized.payload)
            && native::remapRecordedPlayerReferences(
                *remapped,
                *mSourceLocalUniqueId,
                *mSourceLocalRuntimeId,
                ReplayRecordedPlayerUniqueId,
                *mRecordedLocalRuntimeId
            )) {
            std::string remappedPayload;
            if (native::serialize(*remapped, remappedPayload)) serialized.payload = std::move(remappedPayload);
        }
    }

    if (captureState == State::Recording) mPendingPackets.push_back(std::move(serialized));
    else if (captureState == State::Paused) mPausedPackets.push_back(std::move(serialized));
    return true;
}

bool ReplayRecorder::writePacketLocked(ReplayAction action, const SerializedPacket& packet) {
    return mWriter.writeAction(action, encodePacket(packet));
}

bool ReplayRecorder::beginRecordingLocked(bedrocktools::sdk::Player& player) {
    auto* dimension = player.dimension();
    auto* region = dimension ? dimension->blockSource() : nullptr;
    if (!region) {
        setStatusLocked("Unable to read the current dimension");
        return false;
    }

    const int dimensionId = region->dimensionId();
    const auto position = player.position();
    const auto rotation = player.rotation();
    SnapshotContext context{};
    context.dimensionId = dimensionId;
    context.x = position.x;
    context.y = position.y;
    context.z = position.z;
    context.pitch = rotation.x;
    context.yaw = rotation.y;

    auto* level = player.level();
    const auto getTimeAddress = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::Time);
    if (!level || !getTimeAddress) {
        setStatusLocked("Unable to capture the current world time; recording not started");
        return false;
    }
    using GetTimeFn = int (*)(const bedrocktools::sdk::Level*);
    context.worldTime = reinterpret_cast<GetTimeFn>(getTimeAddress)(level);
    if (auto* weather = dimension->weather()) {
        context.rainLevel = weather->rainLevel();
        context.rainTarget = weather->targetRainLevel();
        context.lightningLevel = weather->lightningLevel();
        context.lightningTarget = weather->targetLightningLevel();
    }

    std::vector<SerializedPacket> chunkPackets;
    std::vector<SerializedPacket> actorPackets;
    std::vector<SerializedPacket> freshPlayerSkinPackets;
    std::vector<SerializedPacket> recordedPlayerStatePackets;
    SerializedPacket freshPlayerListPacket;
    std::string recordedPlayerPayload;

    mCapturedChunkColumns.clear();
    mChunkCaptureCenter.reset();
    mChunkCaptureCursor = 0;
    if (!captureFreshLevelChunksLocked(player, ReplayWorldDimensionId, chunkPackets)) {
        setStatusLocked("Unable to capture loaded terrain; recording not started");
        return false;
    }
    if (!captureFreshActorsLocked(player, actorPackets, freshPlayerSkinPackets)) {
        setStatusLocked("Unable to capture loaded entities; recording not started");
        return false;
    }

    const auto sourceUniqueId = native::actorUniqueId(player);
    const auto sourceRuntimeId = native::actorRuntimeId(player);
    auto recordedPlayer = native::tryCreateAddActorPacket(player);
    const auto sourcePlayerUuid = recordedPlayer
        ? native::addPlayerUuid(*recordedPlayer)
        : std::optional<native::UuidBytes>{};
    if (!sourceUniqueId || !sourceRuntimeId || !recordedPlayer
        || !sourcePlayerUuid
        || native::packetId(*recordedPlayer) != static_cast<int>(MinecraftPacketIds::AddPlayer)
        || !native::rewriteAddPlayerIdentity(
            *recordedPlayer,
            ReplayRecordedPlayerUniqueId,
            ReplayRecordedPlayerRuntimeId
        )
        || !native::setRecordedPlayerGameType(player, *recordedPlayer)
        || !content::serializeReplayPacket(*recordedPlayer, recordedPlayerPayload)) {
        setStatusLocked("Unable to capture the separate recorded player; recording not started");
        return false;
    }
    mSourceLocalUniqueId = sourceUniqueId;
    mSourceLocalRuntimeId = sourceRuntimeId;
    mRecordedLocalRuntimeId = ReplayRecordedPlayerRuntimeId;
    if (auto playerList = native::createPlayerListAddPacket(
            *level,
            *sourcePlayerUuid,
            sourceUniqueId,
            native::replayPlayerUuid(),
            ReplayRecordedPlayerUniqueId
        )) {
        std::string payload;
        if (native::serialize(*playerList, payload)) {
            freshPlayerListPacket = {
                static_cast<int>(MinecraftPacketIds::PlayerList),
                std::move(payload)
            };
        }
    }
    if (freshPlayerListPacket.payload.empty()) {
        setStatusLocked("Unable to capture current player skins; recording not started");
        return false;
    }
    if (auto skin = native::createPlayerSkinPacket(player, native::replayPlayerUuid())) {
        std::string payload;
        if (native::serialize(*skin, payload)) {
            freshPlayerSkinPackets.push_back({static_cast<int>(MinecraftPacketIds::PlayerSkin), std::move(payload)});
        }
    }
    if (const auto state = mActorStateSnapshotPackets.find(sourceRuntimeId);
        state != mActorStateSnapshotPackets.end()) {
        for (const auto& serialized : state->second) {
            auto packet = native::createPacket(serialized.id);
            if (!packet || !native::deserialize(*packet, serialized.payload)
                || !native::remapRecordedPlayerReferences(
                    *packet,
                    sourceUniqueId,
                    sourceRuntimeId,
                    ReplayRecordedPlayerUniqueId,
                    ReplayRecordedPlayerRuntimeId
                )) continue;
            std::string payload;
            if (native::serialize(*packet, payload)) {
                recordedPlayerStatePackets.push_back({serialized.id, std::move(payload)});
            }
        }
    }
    if (auto armor = native::createArmorEquipmentPacket(player)) {
        if (native::remapRecordedPlayerReferences(
                *armor,
                sourceUniqueId,
                sourceRuntimeId,
                ReplayRecordedPlayerUniqueId,
                ReplayRecordedPlayerRuntimeId
            )) {
            std::string payload;
            if (native::serialize(*armor, payload)) {
                recordedPlayerStatePackets.push_back({
                    static_cast<int>(MinecraftPacketIds::MobArmorEquipment),
                    std::move(payload)
                });
            }
        }
    }
    for (const auto& held : createHeldStatePackets(player)) {
        auto packet = native::createPacket(held.id);
        if (!packet || !native::deserialize(*packet, held.payload)
            || !native::remapRecordedPlayerReferences(
                *packet, sourceUniqueId, sourceRuntimeId,
                ReplayRecordedPlayerUniqueId, ReplayRecordedPlayerRuntimeId
            )) continue;
        std::string payload;
        if (native::serialize(*packet, payload)) recordedPlayerStatePackets.push_back({held.id, std::move(payload)});
    }

    std::error_code ec;
    auto directory = replayDirectory();
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        setStatusLocked("Unable to create replay directory");
        return false;
    }

    auto name = replayFileName();
    mFinalPath = directory / name;
    for (int suffix = 2; std::filesystem::exists(mFinalPath, ec) && suffix < 1000; ++suffix) {
        mFinalPath = directory / (std::filesystem::path(name).stem().string() + " (" + std::to_string(suffix) + ").playback");
    }
    mTemporaryPath = mFinalPath;
    mTemporaryPath += ".tmp";
    mRecordingWorldName = mWorldName.empty() ? "Unknown world" : mWorldName;

    if (!mWriter.open(mTemporaryPath) || !mWriter.beginSnapshot()) {
        mWriter.abort();
        setStatusLocked("Unable to open replay archive");
        return false;
    }

    if (!mWriter.writeAction(ReplayAction::SnapshotContext, encodeSnapshotContext(context))) {
        mWriter.abort();
        setStatusLocked("Unable to write replay snapshot context");
        return false;
    }

    for (const auto& packet : mConfigurationPackets) {
        if (packet.payload.empty()) continue;
        if (!writePacketLocked(ReplayAction::ConfigurationPacket, packet)) {
            mWriter.abort();
            setStatusLocked("Unable to write replay configuration snapshot");
            return false;
        }
    }

    if (!mWriter.writeAction(ReplayAction::CreateLocalPlayer, recordedPlayerPayload)) {
        mWriter.abort();
        setStatusLocked("Unable to write the separate recorded-player snapshot");
        return false;
    }

    for (const auto& packet : chunkPackets) {
        if (!writePacketLocked(ReplayAction::GamePacket, packet)) {
            mWriter.abort();
            setStatusLocked("Unable to write the terrain snapshot");
            return false;
        }
    }

    if (!writePacketLocked(ReplayAction::GamePacket, freshPlayerListPacket)) {
        mWriter.abort();
        setStatusLocked("Unable to write the current player-list snapshot");
        return false;
    }

    for (const auto& packet : actorPackets) {
        if (!writePacketLocked(ReplayAction::GamePacket, packet)) {
            mWriter.abort();
            setStatusLocked("Unable to write the entity snapshot");
            return false;
        }
    }

    for (const auto& packet : recordedPlayerStatePackets) {
        if (!writePacketLocked(ReplayAction::GamePacket, packet)) {
            mWriter.abort();
            setStatusLocked("Unable to write the recorded-player state snapshot");
            return false;
        }
    }

    for (const auto& packet : mPlayerSkinSnapshotPackets) {
        if (!writePacketLocked(ReplayAction::GamePacket, packet)) {
            mWriter.abort();
            setStatusLocked("Unable to write the player-skin snapshot");
            return false;
        }
    }

    for (const auto& packet : freshPlayerSkinPackets) {
        if (!writePacketLocked(ReplayAction::GamePacket, packet)) {
            mWriter.abort();
            setStatusLocked("Unable to write the current player-skin snapshot");
            return false;
        }
    }

    if (!mWriter.endSnapshot()) {
        mWriter.abort();
        setStatusLocked("Unable to finish replay snapshot");
        return false;
    }

    mPendingPackets.clear();
    mPausedPackets.clear();
    mLastEntityMovements.clear();
    mLastObservedWorldTime = context.worldTime;
    mLastRainTarget = context.rainTarget;
    mLastLightningTarget = context.lightningTarget;
    mTicks = 0;
    setStatusLocked("Recording (terrain snapshot + camera)");
    return true;
}

bool ReplayRecorder::start() {
    auto* player = mCurrentPlayer.load(std::memory_order_acquire);
    if (!player) {
        std::scoped_lock lock(mMutex);
        setStatusLocked("Waiting for the local-player tick hook");
        return false;
    }

    std::scoped_lock lock(mMutex);
    const auto current = mState.load(std::memory_order_relaxed);
    if (current == State::Paused) {
        mPendingPackets.insert(mPendingPackets.end(), std::make_move_iterator(mPausedPackets.begin()), std::make_move_iterator(mPausedPackets.end()));
        mPausedPackets.clear();
        mState.store(State::Recording, std::memory_order_release);
        setStatusLocked("Recording (terrain snapshot + camera)");
        return true;
    }
    if (current != State::Idle) return false;
    mSourceLocalUniqueId.reset();
    mSourceLocalRuntimeId.reset();
    mRecordedLocalRuntimeId.reset();
    mState.store(State::Starting, std::memory_order_release);
    setStatusLocked("Recording start queued");
    return true;
}

bool ReplayRecorder::pause() {
    std::scoped_lock lock(mMutex);
    if (mState.load(std::memory_order_relaxed) != State::Recording) return false;
    mState.store(State::Paused, std::memory_order_release);
    setStatusLocked("Recording paused");
    return true;
}

bool ReplayRecorder::flushPendingLocked() {
    for (const auto& packet : mPendingPackets) {
        const auto lifecycle = describePacketLifecycle(static_cast<MinecraftPacketIds>(packet.id));
        const auto action = lifecycle.isConfiguration() ? ReplayAction::ConfigurationPacket : ReplayAction::GamePacket;
        if (!writePacketLocked(action, packet)) return false;
    }
    mPendingPackets.clear();
    return true;
}

void ReplayRecorder::tick(bedrocktools::sdk::Player* player) {
    if (!player) return;
    mCurrentPlayer.store(player, std::memory_order_release);
    const auto initialState = mState.load(std::memory_order_acquire);
    if (initialState == State::Starting || initialState == State::Recording || initialState == State::Paused) {
        auto* dimension = player->dimension();
        mCurrentBlockSource.store(dimension ? dimension->blockSource() : nullptr, std::memory_order_release);
        if (auto armor = native::createArmorEquipmentPacket(*player)) observeClientPacket(*armor, false);
    }
    if (mState.load(std::memory_order_acquire) == State::Starting) {
        std::scoped_lock lock(mMutex);
        if (mState.load(std::memory_order_relaxed) == State::Starting) {
            if (beginRecordingLocked(*player)) {
                mState.store(State::Recording, std::memory_order_release);
            } else {
                mState.store(State::Idle, std::memory_order_release);
            }
        }
        return;
    }
    if (mState.load(std::memory_order_acquire) != State::Recording) return;
    std::scoped_lock lock(mMutex);
    if (mState.load(std::memory_order_relaxed) != State::Recording) return;

    ReplayEnvironmentState environment{};
    auto* level = player->level();
    const auto getTimeAddress = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::Time);
    if (level && getTimeAddress) {
        using GetTimeFn = int (*)(const bedrocktools::sdk::Level*);
        const auto worldTime = reinterpret_cast<GetTimeFn>(getTimeAddress)(level);
        if (!mLastObservedWorldTime) {
            mLastObservedWorldTime = worldTime;
        } else {
            const auto delta = static_cast<std::int64_t>(worldTime)
                - static_cast<std::int64_t>(*mLastObservedWorldTime);
            constexpr std::int64_t NaturalTickTolerance = 40;
            if (delta < 0 || delta > NaturalTickTolerance) {
                environment.setTime = true;
                environment.worldTime = worldTime;
            }
            mLastObservedWorldTime = worldTime;
        }
    }
    if (auto* dimension = player->dimension()) {
        if (auto* weather = dimension->weather()) {
            const auto rainTarget = weather->targetRainLevel();
            const auto lightningTarget = weather->targetLightningLevel();
            constexpr float WeatherEpsilon = 0.0001f;
            const bool rainChanged = !mLastRainTarget || std::fabs(rainTarget - *mLastRainTarget) > WeatherEpsilon;
            const bool lightningChanged = !mLastLightningTarget
                || std::fabs(lightningTarget - *mLastLightningTarget) > WeatherEpsilon;
            if (rainChanged || lightningChanged) {
                environment.setWeather = true;
                environment.rainLevel = weather->rainLevel();
                environment.rainTarget = rainTarget;
                environment.lightningLevel = weather->lightningLevel();
                environment.lightningTarget = lightningTarget;
            }
            mLastRainTarget = rainTarget;
            mLastLightningTarget = lightningTarget;
        }
    }
    if ((environment.setTime || environment.setWeather)
        && !mWriter.writeAction(ReplayAction::EnvironmentState, encodeEnvironmentState(environment))) {
        mWriter.abort();
        mPendingPackets.clear();
        mPausedPackets.clear();
        mRecordedLocalRuntimeId.reset();
        mState.store(State::Idle, std::memory_order_release);
        setStatusLocked("Recording failed while writing environment state");
        return;
    }

    std::vector<SerializedPacket> newChunkPackets;
    std::vector<SerializedPacket> newActorPackets;
    std::vector<SerializedPacket> newActorStatePackets;
    if (!captureNewLevelChunksLocked(*player, ReplayWorldDimensionId, newChunkPackets)) {
        mWriter.abort();
        mPendingPackets.clear();
        mPausedPackets.clear();
        mCapturedChunkColumns.clear();
        mState.store(State::Idle, std::memory_order_release);
        setStatusLocked("Recording stopped: unable to capture newly loaded terrain");
        return;
    }
    level = player->level();
    const auto actors = level ? native::runtimeActors(*level) : std::vector<bedrocktools::sdk::Actor*>{};
    if (!captureNewActorsLocked(*player, actors, newActorPackets, newActorStatePackets)) {
        mWriter.abort();
        mPendingPackets.clear();
        mPausedPackets.clear();
        mCapturedActorRuntimeIds.clear();
        mCapturedArmorRuntimeIds.clear();
        mState.store(State::Idle, std::memory_order_release);
        setStatusLocked("Recording stopped: unable to capture newly loaded entities");
        return;
    }
    newChunkPackets.insert(
        newChunkPackets.end(),
        std::make_move_iterator(newActorPackets.begin()),
        std::make_move_iterator(newActorPackets.end())
    );
    if (!newChunkPackets.empty()) {
        mPendingPackets.insert(
            mPendingPackets.begin(),
            std::make_move_iterator(newChunkPackets.begin()),
            std::make_move_iterator(newChunkPackets.end())
        );
    }
    if (!newActorStatePackets.empty()) {
        mPendingPackets.insert(
            mPendingPackets.end(),
            std::make_move_iterator(newActorStatePackets.begin()),
            std::make_move_iterator(newActorStatePackets.end())
        );
    }
    if (!flushPendingLocked()) {
        mWriter.abort();
        mPendingPackets.clear();
        mPausedPackets.clear();
        mRecordedLocalRuntimeId.reset();
        mState.store(State::Idle, std::memory_order_release);
        setStatusLocked("Recording failed while writing replay data");
        return;
    }
    std::vector<ReplayMovementState> changedMovements;
    std::unordered_map<std::uint64_t, ReplayMovementState> currentMovements;
    currentMovements.reserve(actors.size() + 1);
    changedMovements.reserve(actors.size() + 1);
    const auto movementEquals = [](const ReplayMovementState& left, const ReplayMovementState& right) {
        return left.x == right.x && left.y == right.y && left.z == right.z
            && left.pitch == right.pitch && left.yaw == right.yaw
            && left.headYaw == right.headYaw && left.bodyYaw == right.bodyYaw
            && left.onGround == right.onGround && left.gliding == right.gliding;
    };
    const auto captureMovement = [&](bedrocktools::sdk::Actor& actor, std::uint64_t replayRuntimeId) {
        if (!replayRuntimeId) return;
        const auto position = actor.position();
        const auto rotation = actor.rotation();
        ReplayMovementState movement{};
        movement.actorId = static_cast<std::int64_t>(replayRuntimeId);
        movement.x = position.x;
        movement.y = position.y;
        movement.z = position.z;
        movement.pitch = rotation.x;
        movement.yaw = rotation.y;
        movement.headYaw = rotation.y;
        movement.bodyYaw = rotation.y;
        movement.gliding = native::actorIsGliding(actor);
        movement.glidingKnown = true;
        auto* context = reinterpret_cast<EntityContext*>(actor.entityContext());
        if (context) {
            if (auto* headRotation = context->tryGetComponent<ActorHeadRotationComponent>()) {
                movement.headYaw = headRotation->mYHeadRot;
            }
            if (auto* bodyRotation = context->tryGetComponent<MobBodyRotationComponent>()) {
                movement.bodyYaw = bodyRotation->mYBodyRot;
            }
        }
        const auto previous = mLastEntityMovements.find(replayRuntimeId);
        movement.onGround = previous != mLastEntityMovements.end()
            && std::fabs(previous->second.y - movement.y) < 0.001f;
        currentMovements.insert_or_assign(replayRuntimeId, movement);
        if (previous == mLastEntityMovements.end() || !movementEquals(previous->second, movement)) {
            changedMovements.push_back(movement);
        }
    };
    captureMovement(*player, ReplayRecordedPlayerRuntimeId);
    auto* playerDimension = player->dimension();
    if (level && player->level() == level) {
        for (auto* actor : actors) {
            if (!actor || actor == player || actor->dimension() != playerDimension) continue;
            const auto runtimeId = native::actorRuntimeId(*actor);
            if (!runtimeId || !mCapturedActorRuntimeIds.contains(runtimeId)) continue;
            if (mSourceLocalRuntimeId && runtimeId == *mSourceLocalRuntimeId) continue;
            if (mSourceLocalUniqueId && native::actorUniqueId(*actor) == *mSourceLocalUniqueId) continue;
            captureMovement(*actor, runtimeId);
        }
    }
    if (!changedMovements.empty()
        && !mWriter.writeAction(ReplayAction::MoveEntities, encodeMoveEntities(changedMovements))) {
        mWriter.abort();
        mRecordedLocalRuntimeId.reset();
        mState.store(State::Idle, std::memory_order_release);
        setStatusLocked("Recording failed while writing entity movements");
        return;
    }
    mLastEntityMovements = std::move(currentMovements);
    if (!mWriter.writeAction(ReplayAction::NextTick, {})) {
        mWriter.abort();
        mPendingPackets.clear();
        mPausedPackets.clear();
        mRecordedLocalRuntimeId.reset();
        mState.store(State::Idle, std::memory_order_release);
        setStatusLocked("Recording failed while writing replay data");
        return;
    }
    ++mTicks;
}

void ReplayRecorder::onClientUpdate() {
}

bool ReplayRecorder::stop() {
    std::scoped_lock lock(mMutex);
    const auto current = mState.load(std::memory_order_relaxed);
    if (current == State::Starting) {
        mState.store(State::Idle, std::memory_order_release);
        setStatusLocked("Recording start cancelled");
        return true;
    }
    if (current != State::Recording && current != State::Paused) return false;
    mState.store(State::Closing, std::memory_order_release);
    mCurrentBlockSource.store(nullptr, std::memory_order_release);

    if (current == State::Paused) {
        mPendingPackets.insert(mPendingPackets.end(), std::make_move_iterator(mPausedPackets.begin()), std::make_move_iterator(mPausedPackets.end()));
        mPausedPackets.clear();
    }

    if (!flushPendingLocked() || !mWriter.close()) {
        mWriter.abort();
        mRecordedLocalRuntimeId.reset();
        mState.store(State::Idle, std::memory_order_release);
        setStatusLocked("Unable to finalize replay archive");
        return false;
    }

    std::error_code ec;
    std::filesystem::rename(mTemporaryPath, mFinalPath, ec);
    if (ec) {
        std::filesystem::remove(mFinalPath, ec);
        ec.clear();
        std::filesystem::rename(mTemporaryPath, mFinalPath, ec);
    }
    if (ec) {
        mRecordedLocalRuntimeId.reset();
        mState.store(State::Idle, std::memory_order_release);
        setStatusLocked("Unable to move completed replay into the replay library");
        return false;
    }

    mLastReplayPath = mFinalPath;
    mTemporaryPath.clear();
    mFinalPath.clear();
    mPendingPackets.clear();
    mPausedPackets.clear();
    mLastEntityMovements.clear();
    mLastObservedWorldTime.reset();
    mLastRainTarget.reset();
    mLastLightningTarget.reset();
    mSourceLocalUniqueId.reset();
    mSourceLocalRuntimeId.reset();
    mRecordedLocalRuntimeId.reset();
    mCapturedChunkColumns.clear();
    mCapturedActorRuntimeIds.clear();
    mCapturedArmorRuntimeIds.clear();
    mCapturedHeldRuntimeIds.clear();
    mRemovedActorRuntimeIds.clear();
    mChunkCaptureCenter.reset();
    mChunkCaptureCursor = 0;
    mState.store(State::Idle, std::memory_order_release);
    setStatusLocked("Replay saved");
    return true;
}

void ReplayRecorder::setStorageDirectory(std::filesystem::path path) {
    std::scoped_lock lock(mMutex);
    mStorageDirectory = std::move(path);
}

void ReplayRecorder::setCurrentWorldName(std::string name) {
    if (name.empty() || name.size() > 256) return;
    std::scoped_lock lock(mMutex);
    mWorldName = std::move(name);
}

void ReplayRecorder::resetWorldCache() {
    mCurrentPlayer.store(nullptr, std::memory_order_release);
    mCurrentBlockSource.store(nullptr, std::memory_order_release);
    std::scoped_lock lock(mMutex);
    mConfigurationPackets.clear();
    mConfigurationLatest.clear();
    mPlayerListSnapshotPackets.clear();
    mAddPlayerSnapshotPackets.clear();
    mSpawnSnapshotPackets.clear();
    mActorStateSnapshotPackets.clear();
    mPlayerSkinSnapshotPackets.clear();
    mCapturedChunkColumns.clear();
    mCapturedActorRuntimeIds.clear();
    mCapturedArmorRuntimeIds.clear();
    mCapturedHeldRuntimeIds.clear();
    mActorUniqueToRuntimeId.clear();
    mActorRuntimeToUniqueId.clear();
    mRemovedActorRuntimeIds.clear();
    mChunkCaptureCenter.reset();
    mChunkCaptureCursor = 0;
    mChunkRequestModes.clear();
    if (mState.load(std::memory_order_relaxed) == State::Idle) {
        mSourceLocalUniqueId.reset();
        mSourceLocalRuntimeId.reset();
        mRecordedLocalRuntimeId.reset();
        mLastObservedWorldTime.reset();
        mLastRainTarget.reset();
        mLastLightningTarget.reset();
    }
}

ReplayRecorder::State ReplayRecorder::state() const noexcept { return mState.load(std::memory_order_acquire); }

std::string ReplayRecorder::status() const {
    std::scoped_lock lock(mMutex);
    return mStatus;
}

std::filesystem::path ReplayRecorder::lastReplayPath() const {
    std::scoped_lock lock(mMutex);
    return mLastReplayPath;
}

std::filesystem::path ReplayRecorder::activeReplayPath() const {
    std::scoped_lock lock(mMutex);
    return mFinalPath;
}

std::string ReplayRecorder::currentWorldName() const {
    std::scoped_lock lock(mMutex);
    return mState.load(std::memory_order_relaxed) == State::Idle ? mWorldName : mRecordingWorldName;
}

std::uint64_t ReplayRecorder::recordedTicks() const noexcept { return mTicks; }

bool ReplayRecorder::hasActivePlayer() const noexcept {
    return mCurrentPlayer.load(std::memory_order_acquire) != nullptr;
}

std::filesystem::path ReplayRecorder::libraryDirectory() const {
    std::scoped_lock lock(mMutex);
    return replayDirectory();
}

}

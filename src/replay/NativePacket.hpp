#pragma once

#include <bedrocktools/sdk/network/Packet.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bedrocktools::sdk { class Actor; class Level; class LevelChunk; }

namespace bedrocktools::replay::native {

using UuidBytes = std::array<std::uint8_t, 16>;

struct PortableChunkSnapshot {
    std::shared_ptr<bedrocktools::sdk::Packet> levelChunk;
    std::shared_ptr<bedrocktools::sdk::Packet> subChunk;
};

int packetId(const bedrocktools::sdk::Packet& packet);
std::string_view packetName(const bedrocktools::sdk::Packet& packet);
bool serialize(const bedrocktools::sdk::Packet& packet, std::string& output);
bool isValid(const bedrocktools::sdk::Packet& packet);
bool validateRoundTrip(const bedrocktools::sdk::Packet& packet);
std::optional<bedrocktools::sdk::ChunkPos> levelChunkPosition(const bedrocktools::sdk::Packet& packet);
std::optional<bedrocktools::sdk::ChunkPos> subChunkCenterColumn(const bedrocktools::sdk::Packet& packet);
std::optional<PortableChunkSnapshot> createPortableChunkSnapshot(
    bedrocktools::sdk::LevelChunk& chunk,
    bedrocktools::sdk::ChunkPos position,
    int dimensionId
);
std::optional<PortableChunkSnapshot> createRequestModeChunkSnapshot(
    bedrocktools::sdk::LevelChunk& chunk,
    bedrocktools::sdk::ChunkPos position,
    int dimensionId
);
std::shared_ptr<bedrocktools::sdk::Packet> createPacket(int packetId);
std::shared_ptr<bedrocktools::sdk::Packet> createBlockUpdatePacket(
    const bedrocktools::sdk::BlockPos& position,
    std::uint32_t blockHash,
    std::uint32_t layer,
    std::uint8_t flags
);
std::shared_ptr<bedrocktools::sdk::Packet> createSystemTextPacket(std::string_view message);
bool deserialize(bedrocktools::sdk::Packet& packet, std::string_view payload);
void* packetHandler(const bedrocktools::sdk::Packet& packet);
bool dispatch(std::shared_ptr<bedrocktools::sdk::Packet>& packet, void* networkIdentifier, void* networkCallback);
bool dispatchMovement(
    bedrocktools::sdk::Actor& actor,
    const bedrocktools::sdk::Vec3& position,
    const bedrocktools::sdk::Vec2& rotation,
    float headYaw,
    bool onGround,
    void* networkIdentifier,
    void* networkCallback
);
std::unique_ptr<bedrocktools::sdk::Packet> tryCreateAddActorPacket(bedrocktools::sdk::Actor& actor);
const UuidBytes& replayPlayerUuid();
std::optional<UuidBytes> addPlayerUuid(const bedrocktools::sdk::Packet& packet);
struct PlayerIdentity {
    UuidBytes uuid{};
    std::string name;
};
std::vector<PlayerIdentity> playerIdentities(const bedrocktools::sdk::Packet& packet);
std::uint64_t addPlayerRuntimeId(const bedrocktools::sdk::Packet& packet);
std::optional<std::uint64_t> spawnRuntimeId(const bedrocktools::sdk::Packet& packet);
std::uint64_t actorUniqueId(bedrocktools::sdk::Actor& actor);
std::uint64_t actorRuntimeId(bedrocktools::sdk::Actor& actor);
std::vector<bedrocktools::sdk::Actor*> runtimeActors(bedrocktools::sdk::Level& level);
bool actorIsPlayer(bedrocktools::sdk::Actor& actor);
bool actorIsGliding(bedrocktools::sdk::Actor& actor);
bool setActorGliding(bedrocktools::sdk::Actor& actor, bool gliding);
std::optional<std::int32_t> actorGameType(bedrocktools::sdk::Actor& actor);
std::shared_ptr<bedrocktools::sdk::Packet> createArmorEquipmentPacket(bedrocktools::sdk::Actor& actor);
std::shared_ptr<bedrocktools::sdk::Packet> createHeldEquipmentPacket(bedrocktools::sdk::Actor& actor, bool offhand);
std::shared_ptr<bedrocktools::sdk::Packet> createPlayerSkinPacket(
    bedrocktools::sdk::Actor& actor,
    const UuidBytes& uuid
);
std::shared_ptr<bedrocktools::sdk::Packet> createPlayerListAddPacket(
    bedrocktools::sdk::Level& level,
    const UuidBytes& sourceUuid,
    std::uint64_t sourceUniqueId,
    const UuidBytes& replayUuid,
    std::uint64_t replayUniqueId
);
bool setRecordedPlayerGameType(bedrocktools::sdk::Actor& actor, bedrocktools::sdk::Packet& packet);
bool normalizeAddPlayerGameType(bedrocktools::sdk::Packet& packet);
bool rewriteAddPlayerIdentity(
    bedrocktools::sdk::Packet& packet,
    std::uint64_t uniqueId,
    std::uint64_t runtimeId
);
bool remapRecordedPlayerReferences(
    bedrocktools::sdk::Packet& packet,
    std::uint64_t sourceUniqueId,
    std::uint64_t sourceRuntimeId,
    std::uint64_t targetUniqueId,
    std::uint64_t targetRuntimeId
);

}

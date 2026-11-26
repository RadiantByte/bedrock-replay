#include "NativePacket.hpp"
#include "ReplayContent.hpp"

#include "core/memory/Rtti.hpp"
#include "ReplayEntityComponents.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/network/MinecraftPacketIds.hpp>
#include <bedrocktools/sdk/offsets/Content.hpp>
#include <bedrocktools/sdk/input/MoveInput.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/world/Level.hpp>
#include <bedrocktools/sdk/world/ChunkSource.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <utility>
#include <vector>

struct ActorGameTypeComponent {
    std::int32_t mGameType;
};

static_assert(sizeof(ActorGameTypeComponent) == 0x4);

namespace bedrocktools::replay::native {
namespace {

struct ReadOnlyBinaryStreamLayout {
    void** vtable{};
    std::string ownedBuffer;
    std::string_view view;
    std::uint64_t readPointer{};
    bool overflow{};
    std::array<std::byte, 7> padding{};
};

struct BinaryStreamLayout {
    ReadOnlyBinaryStreamLayout base;
    std::string* buffer{};
};

struct alignas(8) ResultStorage {
    std::array<std::byte, 256> bytes{};
};

struct VarIntDataOutputLayout {
    void** vtable{};
    void* stream{};
};

struct SerializedSkinRefLayout {
    std::shared_ptr<void> skinImpl;
};

struct PlayerListEntryLayout {
    std::uint64_t uniqueId{};
    UuidBytes uuid{};
    std::string name;
    std::string xuid;
    std::string playFabId;
    std::string platformOnlineId;
    std::int32_t buildPlatform{-1};
    std::array<std::byte, 4> buildPlatformPadding{};
    SerializedSkinRefLayout skin;
    std::array<std::byte, 16> color{};
    bool isTeacher{};
    bool isHost{};
    bool isSubClient{};
    std::array<std::byte, 5> tailPadding{};
};

struct PlayerListAddEntryLayout {
    bool reserved{};
    std::array<std::byte, 7> reservedPadding{};
    UuidBytes uuid{};
    std::uint64_t uniqueId{};
    std::string name;
    std::string xuid;
    std::string playFabId;
    std::string platformOnlineId;
    std::int32_t buildPlatform{-1};
    std::array<std::byte, 4> buildPlatformPadding{};
    SerializedSkinRefLayout skin;
    bool isTeacher{};
    bool isHost{};
    bool isSubClient{};
    std::byte flagsPadding{};
    std::array<std::byte, 16> color{};
    std::array<std::byte, 4> colorPadding{};
    std::uint32_t variantIndex{bedrocktools::sdk::offsets::PlayerListAddEntry::AddVariantIndex};
};

enum class SaveUseCase : std::uint8_t {
    SaveToDisk = 0,
    SendOverNetwork = 1,
    Clone = 2,
    Move = 3,
};

struct SaveContextLayout {
    SaveUseCase saveUseCase{SaveUseCase::SendOverNetwork};
};

struct alignas(8) SubChunkPacketDataLayout {
    std::int8_t offsetX{};
    std::int8_t offsetY{};
    std::int8_t offsetZ{};
    std::uint8_t result{};
    std::array<std::byte, 4> padding{};
    std::string serializedSubChunk;
    bool serializedSubChunkPresent{true};
    std::array<std::byte, 7> optionalPadding{};
    std::array<
        std::byte,
        bedrocktools::sdk::offsets::SubChunkPacketData::Size
            - bedrocktools::sdk::offsets::SubChunkPacketData::mHeightmapData
    > remaining{};

    SubChunkPacketDataLayout(std::int8_t y, std::string payload)
    : offsetY(y),
      result(1),
      serializedSubChunk(std::move(payload)) {}
};

static_assert(sizeof(VarIntDataOutputLayout) == 0x10);
static_assert(sizeof(SaveContextLayout) == 0x1);
static_assert(sizeof(SerializedSkinRefLayout) == 0x10);
static_assert(offsetof(PlayerListEntryLayout, uniqueId) == bedrocktools::sdk::offsets::PlayerListEntry::mUniqueId);
static_assert(offsetof(PlayerListEntryLayout, uuid) == bedrocktools::sdk::offsets::PlayerListEntry::mUuid);
static_assert(offsetof(PlayerListEntryLayout, name) == bedrocktools::sdk::offsets::PlayerListEntry::mName);
static_assert(offsetof(PlayerListEntryLayout, xuid) == bedrocktools::sdk::offsets::PlayerListEntry::mXuid);
static_assert(offsetof(PlayerListEntryLayout, playFabId) == bedrocktools::sdk::offsets::PlayerListEntry::mPlayFabId);
static_assert(
    offsetof(PlayerListEntryLayout, platformOnlineId)
    == bedrocktools::sdk::offsets::PlayerListEntry::mPlatformOnlineId
);
static_assert(
    offsetof(PlayerListEntryLayout, buildPlatform)
    == bedrocktools::sdk::offsets::PlayerListEntry::mBuildPlatform
);
static_assert(offsetof(PlayerListEntryLayout, skin) == bedrocktools::sdk::offsets::PlayerListEntry::mSkin);
static_assert(offsetof(PlayerListEntryLayout, color) == bedrocktools::sdk::offsets::PlayerListEntry::mColor);
static_assert(
    offsetof(PlayerListEntryLayout, isTeacher)
    == bedrocktools::sdk::offsets::PlayerListEntry::mIsTeacher
);
static_assert(offsetof(PlayerListEntryLayout, isHost) == bedrocktools::sdk::offsets::PlayerListEntry::mIsHost);
static_assert(
    offsetof(PlayerListEntryLayout, isSubClient)
    == bedrocktools::sdk::offsets::PlayerListEntry::mIsSubClient
);
static_assert(sizeof(PlayerListEntryLayout) == bedrocktools::sdk::offsets::PlayerListEntry::Size);
static_assert(offsetof(PlayerListAddEntryLayout, uuid) == bedrocktools::sdk::offsets::PlayerListAddEntry::mUuid);
static_assert(offsetof(PlayerListAddEntryLayout, uniqueId) == bedrocktools::sdk::offsets::PlayerListAddEntry::mUniqueId);
static_assert(offsetof(PlayerListAddEntryLayout, name) == bedrocktools::sdk::offsets::PlayerListAddEntry::mName);
static_assert(offsetof(PlayerListAddEntryLayout, xuid) == bedrocktools::sdk::offsets::PlayerListAddEntry::mXuid);
static_assert(offsetof(PlayerListAddEntryLayout, playFabId) == bedrocktools::sdk::offsets::PlayerListAddEntry::mPlayFabId);
static_assert(
    offsetof(PlayerListAddEntryLayout, platformOnlineId)
    == bedrocktools::sdk::offsets::PlayerListAddEntry::mPlatformOnlineId
);
static_assert(
    offsetof(PlayerListAddEntryLayout, buildPlatform)
    == bedrocktools::sdk::offsets::PlayerListAddEntry::mBuildPlatform
);
static_assert(offsetof(PlayerListAddEntryLayout, skin) == bedrocktools::sdk::offsets::PlayerListAddEntry::mSkin);
static_assert(
    offsetof(PlayerListAddEntryLayout, isTeacher)
    == bedrocktools::sdk::offsets::PlayerListAddEntry::mIsTeacher
);
static_assert(offsetof(PlayerListAddEntryLayout, isHost) == bedrocktools::sdk::offsets::PlayerListAddEntry::mIsHost);
static_assert(
    offsetof(PlayerListAddEntryLayout, isSubClient)
    == bedrocktools::sdk::offsets::PlayerListAddEntry::mIsSubClient
);
static_assert(offsetof(PlayerListAddEntryLayout, color) == bedrocktools::sdk::offsets::PlayerListAddEntry::mColor);
static_assert(
    offsetof(PlayerListAddEntryLayout, variantIndex)
    == bedrocktools::sdk::offsets::PlayerListAddEntry::mVariantIndex
);
static_assert(sizeof(PlayerListAddEntryLayout) == bedrocktools::sdk::offsets::PlayerListAddEntry::Size);
static_assert(offsetof(SubChunkPacketDataLayout, result) == bedrocktools::sdk::offsets::SubChunkPacketData::mResult);
static_assert(
    offsetof(SubChunkPacketDataLayout, serializedSubChunk)
    == bedrocktools::sdk::offsets::SubChunkPacketData::mSerializedSubChunk
);
static_assert(
    offsetof(SubChunkPacketDataLayout, serializedSubChunkPresent)
    == bedrocktools::sdk::offsets::SubChunkPacketData::mSerializedSubChunkPresent
);
static_assert(
    offsetof(SubChunkPacketDataLayout, remaining)
    == bedrocktools::sdk::offsets::SubChunkPacketData::mHeightmapData
);
static_assert(sizeof(SubChunkPacketDataLayout) == bedrocktools::sdk::offsets::SubChunkPacketData::Size);

static_assert(sizeof(VarIntDataOutputLayout) == 0x10);
static_assert(sizeof(SaveContextLayout) == 0x1);

bool readSignedVarInt64(
    std::string_view data,
    std::size_t offset,
    std::size_t& end,
    std::int64_t& value
) {
    std::uint64_t encoded{};
    int shift = 0;
    for (int index = 0; index < 10 && offset < data.size(); ++index, shift += 7) {
        const auto byte = static_cast<std::uint8_t>(data[offset++]);
        if (shift == 63 && (byte & 0xFEu) != 0) return false;
        encoded |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) {
            value = static_cast<std::int64_t>((encoded >> 1) ^ (0u - (encoded & 1u)));
            end = offset;
            return true;
        }
    }
    return false;
}

bool readUnsignedVarInt64(
    std::string_view data,
    std::size_t offset,
    std::size_t& end,
    std::uint64_t& value
) {
    value = 0;
    int shift = 0;
    for (int index = 0; index < 10 && offset < data.size(); ++index, shift += 7) {
        const auto byte = static_cast<std::uint8_t>(data[offset++]);
        if (shift == 63 && (byte & 0xFEu) != 0) return false;
        value |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) {
            end = offset;
            return true;
        }
    }
    return false;
}

bool setupReadStream(ReadOnlyBinaryStreamLayout& stream, std::string_view payload) {
    stream.vtable = bedrocktools::core::rtti::vtable("20ReadOnlyBinaryStream");
    if (!stream.vtable) return false;
    stream.ownedBuffer.clear();
    stream.view = payload;
    stream.readPointer = 0;
    stream.overflow = false;
    return true;
}

bool setupWriteStream(BinaryStreamLayout& stream) {
    stream.base.vtable = bedrocktools::core::rtti::vtable("12BinaryStream");
    if (!stream.base.vtable) return false;
    stream.base.ownedBuffer.clear();
    stream.base.view = stream.base.ownedBuffer;
    stream.base.readPointer = 0;
    stream.base.overflow = false;
    stream.buffer = &stream.base.ownedBuffer;
    return true;
}

bool setupVarIntOutput(VarIntDataOutputLayout& output, BinaryStreamLayout& stream) {
    output.vtable = bedrocktools::core::rtti::vtable("16VarIntDataOutput");
    if (!output.vtable) return false;
    output.stream = &stream;
    return true;
}

}

int packetId(const bedrocktools::sdk::Packet& packet) {
    auto** table = *reinterpret_cast<void***>(const_cast<bedrocktools::sdk::Packet*>(&packet));
    if (!table || !table[2]) return -1;
    using Fn = int (*)(const bedrocktools::sdk::Packet*);
    return reinterpret_cast<Fn>(table[2])(&packet);
}

std::string_view packetName(const bedrocktools::sdk::Packet& packet) {
    auto** table = *reinterpret_cast<void***>(const_cast<bedrocktools::sdk::Packet*>(&packet));
    if (!table || !table[3]) return {};
    using Fn = std::string_view (*)(const bedrocktools::sdk::Packet*);
    return reinterpret_cast<Fn>(table[3])(&packet);
}

bool serialize(const bedrocktools::sdk::Packet& packet, std::string& output) {
    BinaryStreamLayout stream;
    if (!setupWriteStream(stream)) return false;
    auto** table = *reinterpret_cast<void***>(const_cast<bedrocktools::sdk::Packet*>(&packet));
    if (!table || !table[8]) return false;
    try {
        using Fn = void (*)(const bedrocktools::sdk::Packet*, void*);
        reinterpret_cast<Fn>(table[8])(&packet, &stream);
        output = stream.base.ownedBuffer;
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

bool isValid(const bedrocktools::sdk::Packet& packet) {
    auto** table = *reinterpret_cast<void***>(const_cast<bedrocktools::sdk::Packet*>(&packet));
    if (!table || !table[12]) return false;
    try {
        using Fn = bool (*)(const bedrocktools::sdk::Packet*);
        return reinterpret_cast<Fn>(table[12])(&packet);
    } catch (...) {
        return false;
    }
}

bool validateRoundTrip(const bedrocktools::sdk::Packet& packet) {
    const int id = packetId(packet);
    if (id < 0 || !isValid(packet)) return false;
    std::string payload;
    if (!serialize(packet, payload)) return false;
    auto decoded = createPacket(id);
    if (!decoded || !deserialize(*decoded, payload)) return false;
    return isValid(*decoded);
}

std::optional<bedrocktools::sdk::ChunkPos> levelChunkPosition(const bedrocktools::sdk::Packet& packet) {
    if (packetId(packet) != static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::FullChunkData)) return std::nullopt;
    bedrocktools::sdk::ChunkPos position{};
    std::memcpy(
        &position,
        reinterpret_cast<const std::byte*>(&packet) + bedrocktools::sdk::offsets::LevelChunkPacket::mPos,
        sizeof(position)
    );
    return position;
}

std::optional<bedrocktools::sdk::ChunkPos> subChunkCenterColumn(const bedrocktools::sdk::Packet& packet) {
    if (packetId(packet) != static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::SubChunkPacket)) return std::nullopt;
    bedrocktools::sdk::BlockPos center{};
    std::memcpy(
        &center,
        reinterpret_cast<const std::byte*>(&packet) + bedrocktools::sdk::offsets::SubChunkPacket::mCenterPos,
        sizeof(center)
    );
    return bedrocktools::sdk::ChunkPos{center.x, center.z};
}

std::optional<PortableChunkSnapshot> createPortableChunkSnapshot(
    bedrocktools::sdk::LevelChunk& chunk,
    bedrocktools::sdk::ChunkPos position,
    int dimensionId
) {
    auto levelPacket = createPacket(static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::FullChunkData));
    if (!levelPacket
        || packetId(*levelPacket) != static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::FullChunkData)) {
        return std::nullopt;
    }

    const auto subChunkAddress = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::SubChunkSerialize);
    const auto biomesAddress = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LevelChunkSerializeBiomes);
    const auto borderAddress = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LevelChunkSerializeBorderBlocks);
    const auto blockEntitiesAddress = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::LevelChunkSerializeBlockEntities
    );
    if (!subChunkAddress || !biomesAddress || !borderAddress || !blockEntitiesAddress) {
        return std::nullopt;
    }

    const auto chunkBase = reinterpret_cast<std::uintptr_t>(&chunk);
    auto* subChunksBegin = *reinterpret_cast<std::byte**>(
        chunkBase + bedrocktools::sdk::offsets::LevelChunk::mSubChunks
    );
    auto* subChunksEnd = *reinterpret_cast<std::byte**>(
        chunkBase + bedrocktools::sdk::offsets::LevelChunk::mSubChunks + sizeof(void*)
    );
    if (!subChunksBegin || !subChunksEnd || subChunksEnd < subChunksBegin) return std::nullopt;
    const auto subChunkBytes = static_cast<std::size_t>(subChunksEnd - subChunksBegin);
    if (subChunkBytes % bedrocktools::sdk::offsets::SubChunk::Size != 0) return std::nullopt;
    const auto subChunkCount = subChunkBytes / bedrocktools::sdk::offsets::SubChunk::Size;
    if (subChunkCount == 0 || subChunkCount > 64) return std::nullopt;
    for (std::size_t index = 0; index < subChunkCount; ++index) {
        const auto* subChunk = subChunksBegin + index * bedrocktools::sdk::offsets::SubChunk::Size;
        if (!*reinterpret_cast<void* const*>(subChunk + bedrocktools::sdk::offsets::SubChunk::mReadStorage)) {
            return std::nullopt;
        }
    }
    const auto paletteUsesHashes = content::blockPaletteUsesHashes();
    if (!paletteUsesHashes) return std::nullopt;

    BinaryStreamLayout stream;
    VarIntDataOutputLayout output;
    if (!setupWriteStream(stream) || !setupVarIntOutput(output, stream)) return std::nullopt;

    try {
        using SerializeSubChunkFn = void (*)(const void*, void*, bool);
        using SerializeBiomesFn = void (*)(const bedrocktools::sdk::LevelChunk*, void*);
        using SerializeBorderFn = bool (*)(const bedrocktools::sdk::LevelChunk*, void*);
        using SerializeBlockEntitiesFn = void (*)(
            const bedrocktools::sdk::LevelChunk*,
            void*,
            const SaveContextLayout*
        );
        const auto serializeSubChunk = reinterpret_cast<SerializeSubChunkFn>(subChunkAddress);
        for (std::size_t index = 0; index < subChunkCount; ++index) {
            serializeSubChunk(
                subChunksBegin + index * bedrocktools::sdk::offsets::SubChunk::Size,
                &output,
                *paletteUsesHashes
            );
        }
        reinterpret_cast<SerializeBiomesFn>(biomesAddress)(&chunk, &output);
        reinterpret_cast<SerializeBorderFn>(borderAddress)(&chunk, &output);
        const SaveContextLayout saveContext{};
        reinterpret_cast<SerializeBlockEntitiesFn>(blockEntitiesAddress)(&chunk, &output, &saveContext);
    } catch (...) {
        return std::nullopt;
    }

    auto* levelRaw = levelPacket.get();
    bedrocktools::sdk::field<bedrocktools::sdk::ChunkPos>(
        levelRaw,
        bedrocktools::sdk::offsets::LevelChunkPacket::mPos
    ) = position;
    bedrocktools::sdk::field<int>(
        levelRaw,
        bedrocktools::sdk::offsets::LevelChunkPacket::mDimensionId
    ) = dimensionId;
    bedrocktools::sdk::field<int>(
        levelRaw,
        bedrocktools::sdk::offsets::LevelChunkPacket::mSubChunksCount
    ) = static_cast<int>(subChunkCount);
    bedrocktools::sdk::field<int>(
        levelRaw,
        bedrocktools::sdk::offsets::LevelChunkPacket::mClientRequestSubChunkLimit
    ) = 0;
    bedrocktools::sdk::field<bool>(
        levelRaw,
        bedrocktools::sdk::offsets::LevelChunkPacket::mClientNeedsToRequestSubchunks
    ) = false;
    bedrocktools::sdk::field<bool>(
        levelRaw,
        bedrocktools::sdk::offsets::LevelChunkPacket::mCacheEnabled
    ) = false;
    bedrocktools::sdk::field<std::vector<std::uint64_t>>(
        levelRaw,
        bedrocktools::sdk::offsets::LevelChunkPacket::mCacheMetadata
    ).clear();
    bedrocktools::sdk::field<std::string>(
        levelRaw,
        bedrocktools::sdk::offsets::LevelChunkPacket::mSerializedChunk
    ) = std::move(stream.base.ownedBuffer);
    if (!validateRoundTrip(*levelPacket)) return std::nullopt;
    return PortableChunkSnapshot{std::move(levelPacket), {}};
}

std::optional<PortableChunkSnapshot> createRequestModeChunkSnapshot(
    bedrocktools::sdk::LevelChunk& chunk,
    bedrocktools::sdk::ChunkPos position,
    int dimensionId
) {
    auto levelPacket = createPacket(static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::FullChunkData));
    if (!levelPacket
        || packetId(*levelPacket) != static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::FullChunkData)) {
        return std::nullopt;
    }

    const auto subChunkAddress = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::SubChunkSerialize);
    const auto biomesAddress = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LevelChunkSerializeBiomes);
    const auto borderAddress = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LevelChunkSerializeBorderBlocks);
    const auto blockEntitiesAddress = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::LevelChunkSerializeBlockEntitiesForSubChunk
    );
    const auto heightmapAddress = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::LevelChunkPopulateHeightMapDataForSubChunkPacket
    );
    if (!subChunkAddress || !biomesAddress || !borderAddress || !blockEntitiesAddress || !heightmapAddress) {
        return std::nullopt;
    }

    const auto chunkBase = reinterpret_cast<std::uintptr_t>(&chunk);
    auto* subChunksBegin = *reinterpret_cast<std::byte**>(chunkBase + bedrocktools::sdk::offsets::LevelChunk::mSubChunks);
    auto* subChunksEnd = *reinterpret_cast<std::byte**>(chunkBase + bedrocktools::sdk::offsets::LevelChunk::mSubChunks + sizeof(void*));
    if (!subChunksBegin || !subChunksEnd || subChunksEnd < subChunksBegin) return std::nullopt;
    const auto subChunkBytes = static_cast<std::size_t>(subChunksEnd - subChunksBegin);
    if (subChunkBytes % bedrocktools::sdk::offsets::SubChunk::Size != 0) return std::nullopt;
    const auto subChunkCount = subChunkBytes / bedrocktools::sdk::offsets::SubChunk::Size;
    if (subChunkCount == 0 || subChunkCount > 64) return std::nullopt;

    BinaryStreamLayout levelStream;
    VarIntDataOutputLayout levelOutput;
    if (!setupWriteStream(levelStream) || !setupVarIntOutput(levelOutput, levelStream)) return std::nullopt;

    try {
        using SerializeBiomesFn = void (*)(const bedrocktools::sdk::LevelChunk*, void*);
        using SerializeBorderFn = bool (*)(const bedrocktools::sdk::LevelChunk*, void*);
        reinterpret_cast<SerializeBiomesFn>(biomesAddress)(&chunk, &levelOutput);
        reinterpret_cast<SerializeBorderFn>(borderAddress)(&chunk, &levelOutput);
    } catch (...) {
        return std::nullopt;
    }

    auto* levelRaw = levelPacket.get();
    bedrocktools::sdk::field<bedrocktools::sdk::ChunkPos>(
        levelRaw,
        bedrocktools::sdk::offsets::LevelChunkPacket::mPos
    ) = position;
    bedrocktools::sdk::field<int>(
        levelRaw,
        bedrocktools::sdk::offsets::LevelChunkPacket::mDimensionId
    ) = dimensionId;
    bedrocktools::sdk::field<int>(
        levelRaw,
        bedrocktools::sdk::offsets::LevelChunkPacket::mSubChunksCount
    ) = 0;
    bedrocktools::sdk::field<int>(
        levelRaw,
        bedrocktools::sdk::offsets::LevelChunkPacket::mClientRequestSubChunkLimit
    ) = -1;
    bedrocktools::sdk::field<bool>(
        levelRaw,
        bedrocktools::sdk::offsets::LevelChunkPacket::mClientNeedsToRequestSubchunks
    ) = true;
    bedrocktools::sdk::field<bool>(
        levelRaw,
        bedrocktools::sdk::offsets::LevelChunkPacket::mCacheEnabled
    ) = false;
    bedrocktools::sdk::field<std::vector<std::uint64_t>>(
        levelRaw,
        bedrocktools::sdk::offsets::LevelChunkPacket::mCacheMetadata
    ).clear();
    bedrocktools::sdk::field<std::string>(
        levelRaw,
        bedrocktools::sdk::offsets::LevelChunkPacket::mSerializedChunk
    ) = std::move(levelStream.base.ownedBuffer);
    if (!validateRoundTrip(*levelPacket)) return std::nullopt;

    struct SerializedSubChunk {
        std::int8_t absoluteY{};
        std::string payload;
    };
    std::vector<SerializedSubChunk> serializedSubChunks;
    serializedSubChunks.reserve(subChunkCount);

    using SerializeSubChunkFn = void (*)(const void*, void*, bool);
    using SerializeBlockEntitiesForSubChunkFn = void (*)(
        const bedrocktools::sdk::LevelChunk*,
        void*,
        const bedrocktools::sdk::BlockPos*,
        const SaveContextLayout*
    );
    using PopulateHeightMapDataForSubChunkPacketFn = void (*)(
        const bedrocktools::sdk::LevelChunk*,
        std::int16_t,
        SubChunkPacketDataLayout*
    );
    const auto serializeSubChunk = reinterpret_cast<SerializeSubChunkFn>(subChunkAddress);
    const auto serializeBlockEntities = reinterpret_cast<SerializeBlockEntitiesForSubChunkFn>(blockEntitiesAddress);
    const auto populateHeightMapData = reinterpret_cast<PopulateHeightMapDataForSubChunkPacketFn>(heightmapAddress);
    const SaveContextLayout saveContext{};
    for (std::size_t index = 0; index < subChunkCount; ++index) {
        const auto* subChunk = subChunksBegin + index * bedrocktools::sdk::offsets::SubChunk::Size;
        const auto* readStorage = *reinterpret_cast<void* const*>(
            subChunk + bedrocktools::sdk::offsets::SubChunk::mReadStorage
        );
        if (!readStorage) continue;

        std::int8_t absoluteY{};
        std::memcpy(
            &absoluteY,
            subChunk + bedrocktools::sdk::offsets::SubChunk::mAbsoluteIndex,
            sizeof(absoluteY)
        );
        BinaryStreamLayout subChunkStream;
        VarIntDataOutputLayout subChunkOutput;
        if (!setupWriteStream(subChunkStream) || !setupVarIntOutput(subChunkOutput, subChunkStream)) {
            return std::nullopt;
        }
        try {
            serializeSubChunk(subChunk, &subChunkOutput, false);
            const bedrocktools::sdk::BlockPos subChunkPosition{position.x, absoluteY, position.z};
            serializeBlockEntities(&chunk, &subChunkOutput, &subChunkPosition, &saveContext);
        } catch (...) {
            return std::nullopt;
        }
        if (subChunkStream.base.ownedBuffer.empty()) return std::nullopt;
        serializedSubChunks.push_back({absoluteY, std::move(subChunkStream.base.ownedBuffer)});
    }

    if (serializedSubChunks.empty()) return std::nullopt;
    std::sort(serializedSubChunks.begin(), serializedSubChunks.end(), [](const auto& left, const auto& right) {
        return left.absoluteY < right.absoluteY;
    });

    std::shared_ptr<bedrocktools::sdk::Packet> subChunkPacket;
    {
        const auto centerY = serializedSubChunks.front().absoluteY;
        for (const auto& entry : serializedSubChunks) {
            const int offset = static_cast<int>(entry.absoluteY) - static_cast<int>(centerY);
            if (offset > std::numeric_limits<std::int8_t>::max()) return std::nullopt;
        }

        subChunkPacket = createPacket(static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::SubChunkPacket));
        if (!subChunkPacket
            || packetId(*subChunkPacket) != static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::SubChunkPacket)) {
            return std::nullopt;
        }
        auto* subChunkRaw = subChunkPacket.get();
        bedrocktools::sdk::field<bool>(
            subChunkRaw,
            bedrocktools::sdk::offsets::SubChunkPacket::mCacheEnabled
        ) = false;
        bedrocktools::sdk::field<int>(
            subChunkRaw,
            bedrocktools::sdk::offsets::SubChunkPacket::mDimensionType
        ) = dimensionId;
        bedrocktools::sdk::field<bedrocktools::sdk::BlockPos>(
            subChunkRaw,
            bedrocktools::sdk::offsets::SubChunkPacket::mCenterPos
        ) = {position.x, centerY, position.z};
        auto& entries = bedrocktools::sdk::field<std::vector<SubChunkPacketDataLayout>>(
            subChunkRaw,
            bedrocktools::sdk::offsets::SubChunkPacket::mSubChunkData
        );
        entries.clear();
        entries.reserve(serializedSubChunks.size());
        for (auto& entry : serializedSubChunks) {
            const auto offset = static_cast<std::int8_t>(
                static_cast<int>(entry.absoluteY) - static_cast<int>(centerY)
            );
            entries.emplace_back(offset, std::move(entry.payload));
            try {
                populateHeightMapData(
                    &chunk,
                    static_cast<std::int16_t>(entry.absoluteY),
                    &entries.back()
                );
            } catch (...) {
                return std::nullopt;
            }
        }
        if (!validateRoundTrip(*subChunkPacket)) return std::nullopt;
    }

    return PortableChunkSnapshot{std::move(levelPacket), std::move(subChunkPacket)};
}

std::shared_ptr<bedrocktools::sdk::Packet> createPacket(int id) {
    const auto address = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MinecraftPacketsCreatePacket);
    if (!address) return {};
    using Fn = std::shared_ptr<bedrocktools::sdk::Packet> (*)(int);
    try {
        return reinterpret_cast<Fn>(address)(id);
    } catch (...) {
        return {};
    }
}

std::shared_ptr<bedrocktools::sdk::Packet> createBlockUpdatePacket(
    const bedrocktools::sdk::BlockPos& position,
    std::uint32_t blockHash,
    std::uint32_t layer,
    std::uint8_t flags
) {
    if (layer > 1) return {};
    auto packet = createPacket(static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::UpdateBlock));
    if (!packet || packetId(*packet) != static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::UpdateBlock)) return {};
    namespace layout = bedrocktools::sdk::offsets::ReplayContent;
    bedrocktools::sdk::field<bedrocktools::sdk::BlockPos>(packet.get(), layout::UpdateBlockPosition) = position;
    auto* payload = packet->payload();
    bedrocktools::sdk::field<std::uint32_t>(payload, layout::UpdateBlockRuntimeId) = blockHash;
    bedrocktools::sdk::field<std::uint32_t>(payload, layout::UpdateBlockLayer) = layer;
    bedrocktools::sdk::field<std::uint8_t>(payload, layout::UpdateBlockFlags) = flags;
    return packet;
}

std::shared_ptr<bedrocktools::sdk::Packet> createSystemTextPacket(std::string_view message) {
    auto packet = createPacket(static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::Text));
    if (!packet || packetId(*packet) != static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::Text)) return {};
    auto* payload = packet->payload();
    auto& variant = bedrocktools::sdk::field<std::uint32_t>(
        payload,
        bedrocktools::sdk::offsets::TextPacketPayload::mVariantIndex
    );
    if (variant != 0) return {};
    bedrocktools::sdk::field<std::uint8_t>(
        payload,
        bedrocktools::sdk::offsets::TextPacketPayload::MessageOnly::mType
    ) = 6;
    bedrocktools::sdk::field<std::string>(
        payload,
        bedrocktools::sdk::offsets::TextPacketPayload::MessageOnly::mMessage
    ).assign(message);
    return packet;
}

bool deserialize(bedrocktools::sdk::Packet& packet, std::string_view payload) {
    ReadOnlyBinaryStreamLayout stream;
    if (!setupReadStream(stream, payload)) return false;
    auto** table = *reinterpret_cast<void***>(&packet);
    if (!table || !table[10]) return false;
    try {
        using Fn = ResultStorage (*)(bedrocktools::sdk::Packet*, void*);
        [[maybe_unused]] auto result = reinterpret_cast<Fn>(table[10])(&packet, &stream);
        return !stream.overflow && stream.readPointer == stream.view.size();
    } catch (...) {
        return false;
    }
}

void* packetHandler(const bedrocktools::sdk::Packet& packet) {
    return *reinterpret_cast<void* const*>(reinterpret_cast<std::uintptr_t>(&packet) + bedrocktools::sdk::offsets::Packet::mHandler);
}

bool dispatch(std::shared_ptr<bedrocktools::sdk::Packet>& packet, void* networkIdentifier, void* networkCallback) {
    if (!packet || !networkIdentifier || !networkCallback) return false;
    void* dispatcher = packetHandler(*packet);
    if (!dispatcher) return false;
    auto** table = *reinterpret_cast<void***>(dispatcher);
    if (!table || !table[2]) return false;
    try {
        using Fn = void (*)(void*, void*, void*, std::shared_ptr<bedrocktools::sdk::Packet>&);
        reinterpret_cast<Fn>(table[2])(dispatcher, networkIdentifier, networkCallback, packet);
        return true;
    } catch (...) {
        return false;
    }
}

bool dispatchMovement(
    bedrocktools::sdk::Actor& actor,
    const bedrocktools::sdk::Vec3& position,
    const bedrocktools::sdk::Vec2& rotation,
    float headYaw,
    bool onGround,
    void* networkIdentifier,
    void* networkCallback
) {
    const bool player = actorIsPlayer(actor);
    const auto runtimeId = actorRuntimeId(actor);
    if (runtimeId == 0) return false;
    const int id = static_cast<int>(player
        ? bedrocktools::sdk::MinecraftPacketIds::MovePlayer
        : bedrocktools::sdk::MinecraftPacketIds::MoveAbsoluteActor);
    auto packet = createPacket(id);
    if (!packet || packetId(*packet) != id) return false;

    if (player) {
        bedrocktools::sdk::field<std::uint64_t>(
            packet.get(), bedrocktools::sdk::offsets::MovePlayerPacket::mPlayerId
        ) = runtimeId;
        bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(
            packet.get(), bedrocktools::sdk::offsets::MovePlayerPacket::mPos
        ) = position;
        bedrocktools::sdk::field<bedrocktools::sdk::Vec2>(
            packet.get(), bedrocktools::sdk::offsets::MovePlayerPacket::mRot
        ) = rotation;
        bedrocktools::sdk::field<float>(
            packet.get(), bedrocktools::sdk::offsets::MovePlayerPacket::mHeadYaw
        ) = headYaw;
        bedrocktools::sdk::field<std::uint8_t>(
            packet.get(), bedrocktools::sdk::offsets::MovePlayerPacket::mPositionMode
        ) = 2;
        bedrocktools::sdk::field<bool>(
            packet.get(), bedrocktools::sdk::offsets::MovePlayerPacket::mOnGround
        ) = onGround;
    } else {
        const auto quantize = [](float degrees) {
            return static_cast<std::int8_t>(std::floor(degrees * 256.0f / 360.0f));
        };
        bedrocktools::sdk::field<std::uint64_t>(
            packet.get(), bedrocktools::sdk::offsets::MoveActorAbsolutePacket::mRuntimeId
        ) = runtimeId;
        bedrocktools::sdk::field<std::uint8_t>(
            packet.get(), bedrocktools::sdk::offsets::MoveActorAbsolutePacket::mHeader
        ) = static_cast<std::uint8_t>((onGround ? 1 : 0) | 2);
        bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(
            packet.get(), bedrocktools::sdk::offsets::MoveActorAbsolutePacket::mPos
        ) = position;
        bedrocktools::sdk::field<std::int8_t>(
            packet.get(), bedrocktools::sdk::offsets::MoveActorAbsolutePacket::mRotX
        ) = quantize(rotation.x);
        bedrocktools::sdk::field<std::int8_t>(
            packet.get(), bedrocktools::sdk::offsets::MoveActorAbsolutePacket::mRotY
        ) = quantize(rotation.y);
        bedrocktools::sdk::field<std::int8_t>(
            packet.get(), bedrocktools::sdk::offsets::MoveActorAbsolutePacket::mHeadYaw
        ) = quantize(headYaw);
    }

    return dispatch(packet, networkIdentifier, networkCallback);
}

std::unique_ptr<bedrocktools::sdk::Packet> tryCreateAddActorPacket(bedrocktools::sdk::Actor& actor) {
    auto** table = *reinterpret_cast<void***>(&actor);
    if (!table || !table[24]) return {};
    using Fn = std::unique_ptr<bedrocktools::sdk::Packet>(*)(bedrocktools::sdk::Actor*);
    return reinterpret_cast<Fn>(table[24])(&actor);
}

const UuidBytes& replayPlayerUuid() {
    static constexpr UuidBytes value{
        0x42, 0x54, 0x52, 0x50, 0x4C, 0x41, 0x59, 0x42,
        0xAC, 0xED, 0x52, 0x45, 0x50, 0x4C, 0x41, 0x59
    };
    return value;
}

std::optional<UuidBytes> addPlayerUuid(const bedrocktools::sdk::Packet& packet) {
    if (packetId(packet) != static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::AddPlayer)) {
        return std::nullopt;
    }
    UuidBytes value{};
    std::memcpy(
        value.data(),
        reinterpret_cast<const std::byte*>(&packet) + bedrocktools::sdk::offsets::AddPlayerPacket::mUuid,
        value.size()
    );
    if (std::all_of(value.begin(), value.end(), [](std::uint8_t byte) { return byte == 0; })) {
        return std::nullopt;
    }
    return value;
}

std::uint64_t addPlayerRuntimeId(const bedrocktools::sdk::Packet& packet) {
    std::uint64_t value{};
    std::memcpy(&value, reinterpret_cast<const std::byte*>(&packet) + bedrocktools::sdk::offsets::AddPlayerPacket::mRuntimeId, sizeof(value));
    return value;
}

std::vector<PlayerIdentity> playerIdentities(const bedrocktools::sdk::Packet& packet) {
    std::vector<PlayerIdentity> result;
    const auto id = static_cast<bedrocktools::sdk::MinecraftPacketIds>(packetId(packet));
    if (id == bedrocktools::sdk::MinecraftPacketIds::AddPlayer) {
        const auto uuid = addPlayerUuid(packet);
        if (uuid) {
            const auto& name = bedrocktools::sdk::field<std::string>(
                &packet, bedrocktools::sdk::offsets::AddPlayerPacket::mName);
            result.push_back({*uuid, name.substr(0, 512)});
        }
    } else if (id == bedrocktools::sdk::MinecraftPacketIds::PlayerList) {
        const auto& entries = bedrocktools::sdk::field<std::vector<PlayerListAddEntryLayout>>(
            &packet, bedrocktools::sdk::offsets::PlayerListPacket::mEntries);
        const auto count = std::min<std::size_t>(entries.size(), 4096);
        result.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& entry = entries[i];
            if (entry.variantIndex != bedrocktools::sdk::offsets::PlayerListAddEntry::AddVariantIndex
                || std::all_of(entry.uuid.begin(), entry.uuid.end(), [](auto byte) { return byte == 0; })) continue;
            result.push_back({entry.uuid, entry.name.substr(0, 512)});
        }
    }
    return result;
}

std::optional<std::uint64_t> spawnRuntimeId(const bedrocktools::sdk::Packet& packet) {
    const auto id = static_cast<bedrocktools::sdk::MinecraftPacketIds>(packetId(packet));
    if (id != bedrocktools::sdk::MinecraftPacketIds::AddActor
        && id != bedrocktools::sdk::MinecraftPacketIds::AddItemActor) {
        return std::nullopt;
    }
    std::string payload;
    if (!serialize(packet, payload) || payload.empty()) return std::nullopt;
    std::size_t offset{};
    std::int64_t uniqueId{};
    if (!readSignedVarInt64(payload, 0, offset, uniqueId)) return std::nullopt;
    std::size_t end{};
    std::uint64_t runtimeId{};
    if (!readUnsignedVarInt64(payload, offset, end, runtimeId) || runtimeId == 0) return std::nullopt;
    return runtimeId;
}

std::uint64_t actorUniqueId(bedrocktools::sdk::Actor& actor) {
    const auto address = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetOrCreateUniqueID);
    if (!address) return 0;
    using Fn = const std::uint64_t* (*)(const bedrocktools::sdk::Actor*);
    try {
        const auto* value = reinterpret_cast<Fn>(address)(&actor);
        return value ? *value : 0;
    } catch (...) {
        return 0;
    }
}

std::uint64_t actorRuntimeId(bedrocktools::sdk::Actor& actor) {
    const auto address = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetRuntimeID);
    if (!address) return 0;
    using Fn = std::uint64_t (*)(const bedrocktools::sdk::Actor*);
    try {
        return reinterpret_cast<Fn>(address)(&actor);
    } catch (...) {
        return 0;
    }
}

std::vector<bedrocktools::sdk::Actor*> runtimeActors(bedrocktools::sdk::Level& level) {
    const auto address = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorManagerList);
    auto* manager = level.actorManager();
    if (!address || !manager) return {};
    using Fn = std::vector<void*> (*)(void*);
    try {
        auto raw = reinterpret_cast<Fn>(address)(manager);
        std::vector<bedrocktools::sdk::Actor*> actors;
        actors.reserve(raw.size());
        for (auto* actor : raw) if (actor) actors.push_back(static_cast<bedrocktools::sdk::Actor*>(actor));
        return actors;
    } catch (...) {
        return {};
    }
}

bool actorIsPlayer(bedrocktools::sdk::Actor& actor) {
    const auto address = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer);
    if (!address) return false;
    using Fn = bool (*)(bedrocktools::sdk::Actor*);
    try { return reinterpret_cast<Fn>(address)(&actor); } catch (...) { return false; }
}

bool actorIsGliding(bedrocktools::sdk::Actor& actor) {
    auto* context = reinterpret_cast<EntityContext*>(actor.entityContext());
    if (!context) return false;
    auto* flags = context->tryGetComponent<ActorDataFlagComponent>();
    return flags && (flags->mFlags[0] & (std::uint64_t{1} << 32)) != 0;
}

bool setActorGliding(bedrocktools::sdk::Actor& actor, bool gliding) {
    auto* context = reinterpret_cast<EntityContext*>(actor.entityContext());
    if (!context) return false;
    auto* flags = context->tryGetComponent<ActorDataFlagComponent>();
    auto* dirty = context->tryGetComponent<ActorDataDirtyFlagsComponent>();
    if (!flags || !dirty) return false;
    const auto mask = std::uint64_t{1} << 32;
    bool poseChanged = ((flags->mFlags[0] & mask) != 0) != gliding;
    if (poseChanged) {
        if (gliding) flags->mFlags[0] |= mask;
        else flags->mFlags[0] &= ~mask;
        dirty->mFlags[0] |= 1;
    }
    auto& registry = context->getRegistry();
    const bool horizontalPose = registry.all_of<IsHorizontalPoseFlagComponent>(context->mEntity);
    if (gliding && !horizontalPose) {
        registry.emplace<IsHorizontalPoseFlagComponent>(context->mEntity);
        poseChanged = true;
    } else if (!gliding && horizontalPose) {
        registry.remove<IsHorizontalPoseFlagComponent>(context->mEntity);
        poseChanged = true;
    }
    if (poseChanged && !registry.all_of<ShouldUpdateBoundingBoxRequestComponent>(context->mEntity)) {
        registry.emplace<ShouldUpdateBoundingBoxRequestComponent>(context->mEntity);
    }
    return true;
}

std::shared_ptr<bedrocktools::sdk::Packet> createHeldEquipmentPacket(bedrocktools::sdk::Actor& actor, bool offhand) {
    if (!actorIsPlayer(actor)) return {};
    auto packet = createPacket(static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::PlayerEquipment));
    auto** expectedTable = bedrocktools::core::rtti::vtable("18MobEquipmentPacket");
    auto** packetTable = packet ? *reinterpret_cast<void***>(packet.get()) : nullptr;
    constexpr std::uintptr_t GetIdOffset = 0xdc02558;
    constexpr std::uintptr_t WriteOffset = 0xdc02d0c;
    constexpr std::uintptr_t PayloadFromItemOffset = 0xdc06064;
    constexpr std::uintptr_t OffhandItemOffset = 0xfd9ccac;
    if (!packet || !expectedTable || packetTable != expectedTable) return {};
    Dl_info image{};
    if (!dladdr(expectedTable[2], &image) || !image.dli_fbase) return {};
    const auto imageBase = reinterpret_cast<std::uintptr_t>(image.dli_fbase);
    if (reinterpret_cast<std::uintptr_t>(expectedTable[2]) != imageBase + GetIdOffset
        || reinterpret_cast<std::uintptr_t>(expectedTable[8]) != imageBase + WriteOffset) return {};
    auto** actorTable = *reinterpret_cast<void***>(&actor);
    if (!actorTable || !actorTable[78]) return {};
    using ItemFn = const void*(*)(const bedrocktools::sdk::Actor*);
    const auto* item = offhand
        ? reinterpret_cast<ItemFn>(imageBase + OffhandItemOffset)(&actor)
        : reinterpret_cast<ItemFn>(actorTable[78])(&actor);
    const auto runtimeId = actorRuntimeId(actor);
    if (!item || !runtimeId) return {};
    int slot = offhand ? 1 : 0;
    if (!offhand) {
        auto* inventory = bedrocktools::sdk::field<void*>(&actor, bedrocktools::sdk::offsets::Player::mInventory);
        if (inventory) {
            const auto selected = bedrocktools::sdk::field<int>(inventory, bedrocktools::sdk::offsets::PlayerInventory::mSelectedSlot);
            if (selected >= 0 && selected < 9) slot = selected;
        }
    }
    auto* payload = packet->payload();
    auto* descriptor = static_cast<std::byte*>(payload) + bedrocktools::sdk::offsets::ReplayContent::EquipmentItemDescriptor;
    auto** descriptorTable = *reinterpret_cast<void***>(descriptor);
    if (!descriptorTable || !descriptorTable[0]) return {};
    using DestructorFn = void(*)(void*);
    using PayloadFromItemFn = void(*)(void*, std::uint64_t, const void*, int, int, std::uint8_t);
    reinterpret_cast<DestructorFn>(descriptorTable[0])(descriptor);
    reinterpret_cast<PayloadFromItemFn>(imageBase + PayloadFromItemOffset)(payload, runtimeId, item, slot, slot, offhand ? 119 : 0);
    return packet;
}

std::shared_ptr<bedrocktools::sdk::Packet> createArmorEquipmentPacket(bedrocktools::sdk::Actor& actor) {
    if (!actorIsPlayer(actor)) return {};
    auto packet = createPacket(static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::MobArmorEquipment));
    auto** expectedTable = bedrocktools::core::rtti::vtable("23MobArmorEquipmentPacket");
    auto** packetTable = packet ? *reinterpret_cast<void***>(packet.get()) : nullptr;
    constexpr std::uintptr_t GetIdOffset = 0xdbf78bc;
    constexpr std::uintptr_t WriteOffset = 0xdbf7f64;
    constexpr std::uintptr_t PayloadDestructorOffset = 0xdbf7500;
    constexpr std::uintptr_t PayloadFromActorOffset = 0xdbfa704;
    if (!packet || !expectedTable || packetTable != expectedTable) return {};
    Dl_info image{};
    if (!dladdr(expectedTable[2], &image) || !image.dli_fbase) return {};
    const auto imageBase = reinterpret_cast<std::uintptr_t>(image.dli_fbase);
    if (reinterpret_cast<std::uintptr_t>(expectedTable[2]) != imageBase + GetIdOffset
        || reinterpret_cast<std::uintptr_t>(expectedTable[8]) != imageBase + WriteOffset) return {};
    using PayloadDestructorFn = void(*)(void*);
    using PayloadFromActorFn = void(*)(void*, const bedrocktools::sdk::Actor*);
    auto* payload = packet->payload();
    reinterpret_cast<PayloadDestructorFn>(imageBase + PayloadDestructorOffset)(payload);
    reinterpret_cast<PayloadFromActorFn>(imageBase + PayloadFromActorOffset)(payload, &actor);
    return packet;
}

std::shared_ptr<bedrocktools::sdk::Packet> createPlayerSkinPacket(
    bedrocktools::sdk::Actor& actor,
    const UuidBytes& uuid
) {
    if (!actorIsPlayer(actor)
        || std::all_of(uuid.begin(), uuid.end(), [](std::uint8_t byte) { return byte == 0; })) {
        return {};
    }
    auto* source = static_cast<SerializedSkinRefLayout*>(static_cast<bedrocktools::sdk::Player&>(actor).skin());
    if (!source || !source->skinImpl) return {};
    auto packet = createPacket(static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::PlayerSkin));
    if (!packet || packetId(*packet) != static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::PlayerSkin)) {
        return {};
    }
    std::memcpy(
        reinterpret_cast<std::byte*>(packet.get()) + bedrocktools::sdk::offsets::PlayerSkinPacket::mUuid,
        uuid.data(),
        uuid.size()
    );
    bedrocktools::sdk::field<SerializedSkinRefLayout>(
        packet.get(),
        bedrocktools::sdk::offsets::PlayerSkinPacket::mSkin
    ).skinImpl = source->skinImpl;
    return validateRoundTrip(*packet) ? packet : std::shared_ptr<bedrocktools::sdk::Packet>{};
}

std::shared_ptr<bedrocktools::sdk::Packet> createPlayerListAddPacket(
    bedrocktools::sdk::Level& level,
    const UuidBytes& sourceUuid,
    std::uint64_t sourceUniqueId,
    const UuidBytes& replayUuid,
    std::uint64_t replayUniqueId
) {
    if (!sourceUniqueId || !replayUniqueId
        || std::all_of(sourceUuid.begin(), sourceUuid.end(), [](std::uint8_t byte) { return byte == 0; })
        || std::all_of(replayUuid.begin(), replayUuid.end(), [](std::uint8_t byte) { return byte == 0; })) {
        return {};
    }
    auto packet = createPacket(static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::PlayerList));
    if (!packet || packetId(*packet) != static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::PlayerList)) {
        return {};
    }
    auto& entries = bedrocktools::sdk::field<std::vector<PlayerListAddEntryLayout>>(
        packet.get(),
        bedrocktools::sdk::offsets::PlayerListPacket::mEntries
    );
    if (!entries.empty()) return {};
    auto** table = *reinterpret_cast<void***>(&level);
    if (!table || !table[bedrocktools::sdk::offsets::VTable::LevelGetPlayerList]) return {};
    using GetPlayerListFn = void* (*)(bedrocktools::sdk::Level*);
    auto* playerList = reinterpret_cast<GetPlayerListFn>(
        table[bedrocktools::sdk::offsets::VTable::LevelGetPlayerList]
    )(&level);
    if (!playerList) return {};
    bool localEntryFound = false;
    auto* node = bedrocktools::sdk::field<void*>(
        playerList,
        bedrocktools::sdk::offsets::PlayerListMap::mFirstNode
    );
    for (std::size_t index = 0; node && index < 1024; ++index) {
        auto* liveEntry = reinterpret_cast<const PlayerListEntryLayout*>(
            reinterpret_cast<std::uintptr_t>(node)
            + bedrocktools::sdk::offsets::PlayerListMap::mNodeEntry
        );
        node = *reinterpret_cast<void**>(node);
        if (!liveEntry->uniqueId || !liveEntry->skin.skinImpl
            || std::all_of(liveEntry->uuid.begin(), liveEntry->uuid.end(), [](std::uint8_t byte) { return byte == 0; })) {
            continue;
        }
        const bool localEntry = liveEntry->uuid == sourceUuid || liveEntry->uniqueId == sourceUniqueId;
        PlayerListAddEntryLayout entry;
        entry.uuid = localEntry ? replayUuid : liveEntry->uuid;
        entry.uniqueId = localEntry ? replayUniqueId : liveEntry->uniqueId;
        entry.name = liveEntry->name;
        entry.xuid = liveEntry->xuid;
        entry.playFabId = liveEntry->playFabId;
        entry.platformOnlineId = liveEntry->platformOnlineId;
        entry.buildPlatform = liveEntry->buildPlatform;
        entry.skin.skinImpl = liveEntry->skin.skinImpl;
        entry.isTeacher = liveEntry->isTeacher;
        entry.isHost = liveEntry->isHost;
        entry.isSubClient = liveEntry->isSubClient;
        entry.color = liveEntry->color;
        entries.push_back(std::move(entry));
        localEntryFound = localEntryFound || localEntry;
    }
    if (!localEntryFound || entries.empty()) return {};
    return validateRoundTrip(*packet) ? packet : std::shared_ptr<bedrocktools::sdk::Packet>{};
}

std::optional<std::int32_t> actorGameType(bedrocktools::sdk::Actor& actor) {
    auto* context = reinterpret_cast<EntityContext*>(actor.entityContext());
    if (!context) return std::nullopt;
    auto* component = context->tryGetComponent<ActorGameTypeComponent>();
    if (!component) return std::nullopt;
    switch (component->mGameType) {
    case 0:
    case 3:
        return 0;
    case 1:
    case 4:
        return 1;
    case 2:
        return 2;
    case 6:
        return 6;
    default:
        return std::nullopt;
    }
}

bool setRecordedPlayerGameType(bedrocktools::sdk::Actor& actor, bedrocktools::sdk::Packet& packet) {
    if (packetId(packet) != static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::AddPlayer)) return false;
    auto& gameType = bedrocktools::sdk::field<std::int32_t>(
        &packet,
        bedrocktools::sdk::offsets::AddPlayerPacket::mPlayerGameType
    );
    switch (gameType) {
    case 0:
    case 1:
    case 2:
    case 6:
        return true;
    case 3:
        gameType = 0;
        return true;
    case 4:
        gameType = 1;
        return true;
    default:
        break;
    }
    if (const auto liveGameType = actorGameType(actor)) {
        gameType = *liveGameType;
    } else {
        gameType = 0;
    }
    return true;
}

bool normalizeAddPlayerGameType(bedrocktools::sdk::Packet& packet) {
    if (packetId(packet) != static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::AddPlayer)) return true;
    auto& gameType = bedrocktools::sdk::field<std::int32_t>(
        &packet,
        bedrocktools::sdk::offsets::AddPlayerPacket::mPlayerGameType
    );
    if (gameType == -1 || gameType == 5) gameType = 2;
    return gameType == 0 || gameType == 1 || gameType == 2 || gameType == 6;
}

bool rewriteAddPlayerIdentity(
    bedrocktools::sdk::Packet& packet,
    std::uint64_t uniqueId,
    std::uint64_t runtimeId
) {
    if (packetId(packet) != static_cast<int>(bedrocktools::sdk::MinecraftPacketIds::AddPlayer)
        || uniqueId == 0 || runtimeId == 0) {
        return false;
    }

    auto& name = bedrocktools::sdk::field<std::string>(&packet, bedrocktools::sdk::offsets::AddPlayerPacket::mName);
    constexpr std::size_t LegacyNameTail = 15;
    if (name.size() > LegacyNameTail && name.size() <= 22) {
        const auto damagedSize = name.size() - LegacyNameTail;
        if (std::memcmp(name.data() + LegacyNameTail, &uniqueId, damagedSize) == 0) {
            name.resize(LegacyNameTail);
        }
    }
    auto* base = reinterpret_cast<std::byte*>(&packet);
    const auto& uuid = replayPlayerUuid();
    std::memcpy(base + bedrocktools::sdk::offsets::AddPlayerPacket::mUuid, uuid.data(), uuid.size());
    std::memcpy(base + bedrocktools::sdk::offsets::AddPlayerPacket::mUniqueId, &uniqueId, sizeof(uniqueId));
    std::memcpy(base + bedrocktools::sdk::offsets::AddPlayerPacket::mRuntimeId, &runtimeId, sizeof(runtimeId));
    bedrocktools::sdk::field<std::string>(&packet, bedrocktools::sdk::offsets::AddPlayerPacket::mPlatformChatId).clear();
    bedrocktools::sdk::field<std::string>(&packet, bedrocktools::sdk::offsets::AddPlayerPacket::mDeviceId).clear();
    return true;
}

bool remapRecordedPlayerReferences(
    bedrocktools::sdk::Packet& packet,
    std::uint64_t sourceUniqueId,
    std::uint64_t sourceRuntimeId,
    std::uint64_t targetUniqueId,
    std::uint64_t targetRuntimeId
) {
    if (!sourceRuntimeId || !targetRuntimeId) return false;
    auto* payload = static_cast<std::byte*>(packet.payload());
    const auto replace = [](std::byte* base, std::size_t offset, std::uint64_t source, std::uint64_t target) {
        if (!source || !target) return false;
        std::uint64_t value{};
        std::memcpy(&value, base + offset, sizeof(value));
        if (value != source) return false;
        std::memcpy(base + offset, &target, sizeof(target));
        return true;
    };

    const auto id = static_cast<bedrocktools::sdk::MinecraftPacketIds>(packetId(packet));
    switch (id) {
    case bedrocktools::sdk::MinecraftPacketIds::AddPlayer: {
        std::uint64_t value{};
        std::memcpy(
            &value,
            reinterpret_cast<std::byte*>(&packet) + bedrocktools::sdk::offsets::AddPlayerPacket::mRuntimeId,
            sizeof(value)
        );
        if (value != sourceRuntimeId) return false;
        return rewriteAddPlayerIdentity(packet, targetUniqueId, targetRuntimeId);
    }
    case bedrocktools::sdk::MinecraftPacketIds::MoveAbsoluteActor:
    case bedrocktools::sdk::MinecraftPacketIds::MovePlayer:
    case bedrocktools::sdk::MinecraftPacketIds::ActorEvent:
    case bedrocktools::sdk::MinecraftPacketIds::MobEffect:
    case bedrocktools::sdk::MinecraftPacketIds::UpdateAttributes:
    case bedrocktools::sdk::MinecraftPacketIds::PlayerEquipment:
    case bedrocktools::sdk::MinecraftPacketIds::MobArmorEquipment:
    case bedrocktools::sdk::MinecraftPacketIds::SetActorData:
    case bedrocktools::sdk::MinecraftPacketIds::SetActorMotion:
    case bedrocktools::sdk::MinecraftPacketIds::Animate:
        return replace(payload, 0, sourceRuntimeId, targetRuntimeId);
    case bedrocktools::sdk::MinecraftPacketIds::TakeItemActor: {
        bool changed = replace(payload, 0, sourceRuntimeId, targetRuntimeId);
        changed |= replace(payload, 8, sourceRuntimeId, targetRuntimeId);
        return changed;
    }
    case bedrocktools::sdk::MinecraftPacketIds::PlayerAction:
        return replace(payload, 0x20, sourceRuntimeId, targetRuntimeId);
    case bedrocktools::sdk::MinecraftPacketIds::RemoveActor:
        return replace(payload, 0, sourceUniqueId, targetUniqueId);
    case bedrocktools::sdk::MinecraftPacketIds::SetActorLink: {
        bool changed = replace(payload, 8, sourceUniqueId, targetUniqueId);
        changed |= replace(payload, 16, sourceUniqueId, targetUniqueId);
        return changed;
    }
    case bedrocktools::sdk::MinecraftPacketIds::UpdatePlayerGameType:
        return replace(payload, 8, sourceUniqueId, targetUniqueId);
    default:
        return false;
    }
}

}

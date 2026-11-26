#pragma once

#include <cstddef>
#include <cstdint>

namespace bedrocktools::sdk::offsets {


namespace NetworkStatistics {
inline constexpr std::size_t mType = 0x30;
}

namespace NetworkIdentifier {
inline constexpr std::size_t mType = 0xA8;
inline constexpr std::size_t Size = 0xB0;
}

namespace Packet {
inline constexpr std::size_t mHandler = 0x20;
inline constexpr std::size_t Size = 0x30;
}

namespace AddPlayerPacket {
inline constexpr std::size_t mUuid = 0x30;
inline constexpr std::size_t mName = 0x40;

inline constexpr std::size_t mUniqueId = 0x140;
inline constexpr std::size_t mRuntimeId = 0x58;
inline constexpr std::size_t mPlatformChatId = 0x60;
inline constexpr std::size_t mDeviceId = 0x1A8;
inline constexpr std::size_t mBuildPlatform = 0x1C0;
inline constexpr std::size_t mPlayerGameType = 0xF0;
inline constexpr std::size_t Size = 0x1D0;
}

namespace PlayerSkinPacket {
inline constexpr std::size_t mUuid = 0x30;
inline constexpr std::size_t mSkin = 0x40;
inline constexpr std::size_t Size = 0x88;
}

namespace PlayerListPacket {
inline constexpr std::size_t mEntries = 0x30;
inline constexpr std::size_t Size = 0x50;
}

namespace PlayerListMap {
inline constexpr std::size_t mFirstNode = 0x10;
inline constexpr std::size_t mNodeEntry = 0x20;
}

namespace PlayerListEntry {
inline constexpr std::size_t mUniqueId = 0x00;
inline constexpr std::size_t mUuid = 0x08;
inline constexpr std::size_t mName = 0x18;
inline constexpr std::size_t mXuid = 0x30;
inline constexpr std::size_t mPlayFabId = 0x48;
inline constexpr std::size_t mPlatformOnlineId = 0x60;
inline constexpr std::size_t mBuildPlatform = 0x78;
inline constexpr std::size_t mSkin = 0x80;
inline constexpr std::size_t mColor = 0x90;
inline constexpr std::size_t mIsTeacher = 0xA0;
inline constexpr std::size_t mIsHost = 0xA1;
inline constexpr std::size_t mIsSubClient = 0xA2;
inline constexpr std::size_t Size = 0xA8;
}

namespace PlayerListAddEntry {
inline constexpr std::size_t mUuid = 0x08;
inline constexpr std::size_t mUniqueId = 0x18;
inline constexpr std::size_t mName = 0x20;
inline constexpr std::size_t mXuid = 0x38;
inline constexpr std::size_t mPlayFabId = 0x50;
inline constexpr std::size_t mPlatformOnlineId = 0x68;
inline constexpr std::size_t mBuildPlatform = 0x80;
inline constexpr std::size_t mSkin = 0x88;
inline constexpr std::size_t mIsTeacher = 0x98;
inline constexpr std::size_t mIsHost = 0x99;
inline constexpr std::size_t mIsSubClient = 0x9A;
inline constexpr std::size_t mColor = 0x9C;
inline constexpr std::size_t mVariantIndex = 0xB0;
inline constexpr std::size_t Size = 0xB8;
inline constexpr std::uint32_t AddVariantIndex = 1;
}

namespace MovePlayerPacket {
inline constexpr std::size_t mPlayerId = 0x30;
inline constexpr std::size_t mPos = 0x38;
inline constexpr std::size_t mRot = 0x44;
inline constexpr std::size_t mHeadYaw = 0x4C;
inline constexpr std::size_t mPositionMode = 0x50;
inline constexpr std::size_t mOnGround = 0x51;
}

namespace MoveActorAbsolutePacket {
inline constexpr std::size_t mRuntimeId = 0x30;
inline constexpr std::size_t mHeader = 0x38;
inline constexpr std::size_t mPos = 0x3C;
inline constexpr std::size_t mRotX = 0x48;
inline constexpr std::size_t mRotY = 0x49;
inline constexpr std::size_t mHeadYaw = 0x4A;
}

namespace LevelChunkPacket {
inline constexpr std::size_t mPos = 0x30;
inline constexpr std::size_t mDimensionId = 0x38;
inline constexpr std::size_t mSubChunksCount = 0x3C;
inline constexpr std::size_t mClientRequestSubChunkLimit = 0x40;
inline constexpr std::size_t mClientNeedsToRequestSubchunks = 0x44;
inline constexpr std::size_t mCacheEnabled = 0x48;
inline constexpr std::size_t mCacheMetadata = 0x50;
inline constexpr std::size_t mSerializedChunk = 0x68;
inline constexpr std::size_t Size = 0x90;
}

namespace SubChunkPacket {
inline constexpr std::size_t mCacheEnabled = 0x30;
inline constexpr std::size_t mDimensionType = 0x34;
inline constexpr std::size_t mSubChunkData = 0x38;
inline constexpr std::size_t mCenterPos = 0x50;
inline constexpr std::size_t Size = 0x68;
}

namespace SubChunkPacketData {
inline constexpr std::size_t mResult = 0x03;
inline constexpr std::size_t mSerializedSubChunk = 0x08;
inline constexpr std::size_t mSerializedSubChunkPresent = 0x20;
inline constexpr std::size_t mHeightmapData = 0x28;
inline constexpr std::size_t Size = 0x240;
}



namespace TextPacketPayload {
inline constexpr std::size_t mVariantIndex = 0x90;
namespace AuthorAndMessage {
inline constexpr std::size_t mType = 0x58;
inline constexpr std::size_t mMessage = 0x78;
}
namespace MessageOnly {
inline constexpr std::size_t mType = 0x58;
inline constexpr std::size_t mMessage = 0x60;
}
}

namespace AvailableCommandsPacketPayload {
inline constexpr std::size_t mEnumValues = 0x00;
inline constexpr std::size_t mEnums = 0x30;
inline constexpr std::size_t mCommands = 0x78;
}

namespace CommandRequestPacketPayload {
inline constexpr std::size_t mCommand = 0;
}

namespace CommandOriginData {
inline constexpr std::size_t mType = 0;
}

namespace ChangeDimensionPacketPayload {
inline constexpr std::size_t mDimensionId = 0;
}

namespace SetTitlePacketPayload {
inline constexpr std::size_t mType = 0;
}

}

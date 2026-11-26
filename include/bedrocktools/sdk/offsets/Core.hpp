#pragma once

#include <cstddef>

namespace bedrocktools::sdk::offsets {

namespace VTable {
inline constexpr std::size_t ClientInstance_getRegion = 30;
inline constexpr std::size_t BlockSource_getDimensionId = 18;
inline constexpr std::size_t BlockSource_clip = 51;
inline constexpr std::size_t BlockSource_getChunkSource = 53;
inline constexpr std::size_t BlockSource_fireBlockChanged = 64;
inline constexpr std::size_t ChunkSource_getExistingChunk = 4;
inline constexpr std::size_t ClientInstanceGetMinecraftGame = 79;
inline constexpr std::size_t ClientInstanceGetLocalPlayer = 31;
inline constexpr std::size_t ClientInstanceRequestLeaveGameAsync = 14;
inline constexpr std::size_t LevelGetPlayerList = 320;
}

namespace ClientInstance {
inline constexpr std::size_t mLevelRenderer = 0x1B8;
}

}

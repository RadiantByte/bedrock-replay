#pragma once

#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <memory>

namespace bedrocktools::sdk {

class LevelChunk {};

class ChunkSource {
public:
    std::shared_ptr<LevelChunk> existingChunk(const ChunkPos& pos) {
        return virtualCall<std::shared_ptr<LevelChunk>>(this, offsets::VTable::ChunkSource_getExistingChunk, pos);
    }
};

}

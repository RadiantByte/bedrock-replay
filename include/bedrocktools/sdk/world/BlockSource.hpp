#pragma once

#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/world/ChunkSource.hpp>

namespace bedrocktools::sdk {

class BlockSource {
public:
    int dimensionId() {
        return virtualCall<int>(this, offsets::VTable::BlockSource_getDimensionId);
    }

    ChunkSource* chunkSource() {
        return virtualCall<ChunkSource*>(this, offsets::VTable::BlockSource_getChunkSource);
    }
};

}

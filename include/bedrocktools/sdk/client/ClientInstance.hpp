#pragma once

#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/render/LevelRenderer.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/world/BlockSource.hpp>

namespace bedrocktools::sdk {

class ClientInstance {
public:
    BlockSource* region() { return virtualCall<BlockSource*>(this, offsets::VTable::ClientInstance_getRegion); }
    LevelRenderer* levelRenderer() { return field<LevelRenderer*>(this, offsets::ClientInstance::mLevelRenderer); }

    Player* localPlayer() { return virtualCall<Player*>(this, offsets::VTable::ClientInstanceGetLocalPlayer); }

    void requestLeaveGameAsync() { virtualCall<void>(this, offsets::VTable::ClientInstanceRequestLeaveGameAsync); }

};

}

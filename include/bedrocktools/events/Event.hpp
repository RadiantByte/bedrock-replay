#pragma once

#include <cstdint>

namespace bedrocktools::events {

enum class EventType : std::uint32_t {
    Frame = 1,
    LocalPlayerTick,
    ClientInstanceUpdate,
    LocalPlayerPreTick,
    LevelState,
    PacketHandle
};

enum class EventPriority : std::int32_t {
    First = 200,
    Early = 100,
    Normal = 0,
    Late = -100,
    Last = -200
};


}

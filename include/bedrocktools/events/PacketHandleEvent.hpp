#pragma once

#include <bedrocktools/events/Event.hpp>
#include <bedrocktools/sdk/network/Packet.hpp>
#include <memory>

namespace bedrocktools::events {

enum class PacketHandleKind : std::uint8_t {
    ResourcePacksInfo,
    ResourcePackStack
};

struct PacketHandleEvent {
    static constexpr EventType type = EventType::PacketHandle;
    PacketHandleKind kind;
    void* networkIdentifier;
    void* networkCallback;
    std::shared_ptr<bedrocktools::sdk::Packet>* packet;
};

}

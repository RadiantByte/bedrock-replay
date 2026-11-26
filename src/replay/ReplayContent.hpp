#pragma once

#include "ReplayArchive.hpp"
#include <bedrocktools/sdk/network/Packet.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bedrocktools::replay::content {
bool install();
void uninstall();
bool prepare(const std::vector<SerializedPacket>& packets, std::string& error);
void release();
void flushRelease();
std::string failure();
void reportFailure(std::string_view message);
std::shared_ptr<bedrocktools::sdk::Packet> handshakePacket(int id);
std::optional<bool> blockPaletteUsesHashes();
bool serializeReplayPacket(const bedrocktools::sdk::Packet& packet, std::string& output);
bool prepareReplayPacket(bedrocktools::sdk::Packet& packet);
}

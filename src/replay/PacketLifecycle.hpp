#pragma once

#include <bedrocktools/sdk/network/MinecraftPacketIds.hpp>
#include <optional>

namespace bedrocktools::replay {

enum class PacketLifecycle { Timeline, Ignore, PreWorldHandshake, SnapshotLatest, SnapshotSequence };

struct PacketLifecycleSemantics {
    PacketLifecycle lifecycle{PacketLifecycle::Timeline};
    bool replayEverySnapshot{};
    bool startsConfigurationEpoch{};
    std::optional<bedrocktools::sdk::MinecraftPacketIds> supersedes;

    bool isConfiguration() const noexcept {
        return lifecycle == PacketLifecycle::PreWorldHandshake || lifecycle == PacketLifecycle::SnapshotLatest || lifecycle == PacketLifecycle::SnapshotSequence;
    }

    bool keepsSequence() const noexcept { return lifecycle == PacketLifecycle::SnapshotSequence; }
};

PacketLifecycleSemantics describePacketLifecycle(bedrocktools::sdk::MinecraftPacketIds id);

bool isRecordedContainerUiPacket(bedrocktools::sdk::MinecraftPacketIds id);

}

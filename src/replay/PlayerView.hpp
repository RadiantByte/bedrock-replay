#pragma once

#include <bedrocktools/sdk/Types.hpp>
#include <array>
#include <cstdint>
#include <string>

namespace bedrocktools::replay {

using PlayerViewId = std::array<std::uint8_t, 16>;

enum class PlayerPerspective { Free, FirstPerson, ThirdPersonBack, ThirdPersonFront };

struct RecordedPlayerView {
    PlayerViewId id{};
    std::string name;
    bool recordingPlayer{};
    bool available{};
};

struct PlayerViewPose {
    bedrocktools::sdk::Vec3 eye{};
    float yaw{};
    float pitch{};
    std::uint64_t runtimeId{};
};

}

#pragma once

#include <bedrocktools/events/Event.hpp>
#include <string>

namespace bedrocktools::events {

enum class LevelPhase : std::uint8_t {
    Initializing,
    Destroyed
};

struct LevelStateEvent {
    static constexpr EventType type = EventType::LevelState;
    LevelPhase phase;
    void* level;
    const std::string* levelName;
    void* levelSettings;
    const std::string* levelId;
};

}

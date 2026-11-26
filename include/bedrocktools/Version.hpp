#pragma once

#include <cstdint>
#include <string_view>

namespace bedrocktools {

inline constexpr std::string_view Name = "BedrockReplay";
inline constexpr std::string_view Author = "MrPokeG";
inline constexpr std::string_view Description = "A standalone replay recording, playback, and export mod for Minecraft Bedrock.";
inline constexpr std::string_view Version = "0.1.4";
inline constexpr std::uint32_t SdkVersion = 1;
inline constexpr std::string_view RuntimeLibrary = "libBedrockReplay.so";

}

#pragma once

#include <array>
#include <cstddef>
#include <mutex>
#include <string_view>

namespace bedrocktools::replay {

class ReplayWorldSettings {
public:
    static ReplayWorldSettings& get();
    ~ReplayWorldSettings();

    bool capture(const void* levelSettings, std::string_view levelId = {});
    bool ensureAvailable();
    bool setDefaultSpawn(float x, float y, float z);
    void setInitialTime(int time);
    bool prepareFresh();
    bool available() const;
    const void* data() const;
    void clear();

private:
    ReplayWorldSettings() = default;
    void configureReplayWorldLocked();
    bool destroyLocked();

    alignas(16) std::array<std::byte, 0x460> mStorage{};
    mutable std::mutex mMutex;
    std::array<int, 3> mDefaultSpawn{};
    int mInitialTime{};
    bool mHasDefaultSpawn{};
    bool mConstructed{};
};

}

#include "ReplayWorldSettings.hpp"

#include <bedrocktools/memory/Signatures.hpp>

#include <cmath>
#include <cstring>
#include <limits>

namespace bedrocktools::replay {
namespace {

constexpr std::size_t GameTypeOffset = 0x8;
constexpr std::size_t ForceGameTypeOffset = 0x14;
constexpr std::size_t GeneratorTypeOffset = 0x18;
constexpr std::size_t AchievementsDisabledOffset = 0x68;
constexpr std::size_t TimeOffset = 0x74;
constexpr std::size_t CheatsEnabledOffset = 0x96;
constexpr std::size_t CommandsEnabledOffset = 0x98;
constexpr std::size_t DefaultSpawnOffset = 0xD0;
constexpr int SpectatorGameType = 6;
constexpr int VoidGeneratorType = 5;
constexpr std::string_view ReplayLevelPrefix = "__playback_replay_world__";

}

ReplayWorldSettings& ReplayWorldSettings::get() {
    static ReplayWorldSettings settings;
    return settings;
}

ReplayWorldSettings::~ReplayWorldSettings() {
    std::scoped_lock lock(mMutex);
    destroyLocked();
}

void ReplayWorldSettings::configureReplayWorldLocked() {
    *reinterpret_cast<int*>(mStorage.data() + GameTypeOffset) = SpectatorGameType;
    *reinterpret_cast<bool*>(mStorage.data() + ForceGameTypeOffset) = true;
    *reinterpret_cast<int*>(mStorage.data() + GeneratorTypeOffset) = VoidGeneratorType;
    *reinterpret_cast<bool*>(mStorage.data() + AchievementsDisabledOffset) = true;
    *reinterpret_cast<int*>(mStorage.data() + TimeOffset) = mInitialTime;
    *reinterpret_cast<bool*>(mStorage.data() + CheatsEnabledOffset) = true;
    *reinterpret_cast<bool*>(mStorage.data() + CommandsEnabledOffset) = true;
    if (mHasDefaultSpawn) {
        std::memcpy(mStorage.data() + DefaultSpawnOffset, mDefaultSpawn.data(), sizeof(mDefaultSpawn));
    }
}

bool ReplayWorldSettings::destroyLocked() {
    if (!mConstructed) return true;
    const auto address = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LevelSettingsDtor);
    if (!address) return false;
    using Fn = void(*)(void*);
    reinterpret_cast<Fn>(address)(mStorage.data());
    mStorage.fill(std::byte{});
    mConstructed = false;
    return true;
}

bool ReplayWorldSettings::capture(const void* levelSettings, std::string_view levelId) {
    if (!levelSettings || levelId.starts_with(ReplayLevelPrefix)) return false;
    std::scoped_lock lock(mMutex);
    if (mConstructed && !destroyLocked()) return false;
    const auto address = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LevelSettingsCopyCtor);
    if (!address) return false;
    using Fn = void*(*)(void*, const void*);
    reinterpret_cast<Fn>(address)(mStorage.data(), levelSettings);
    configureReplayWorldLocked();
    mConstructed = true;
    return true;
}

bool ReplayWorldSettings::ensureAvailable() {
    std::scoped_lock lock(mMutex);
    if (mConstructed) return true;
    const auto address = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LevelSettingsDefaultCtor);
    if (!address) return false;
    using Fn = void*(*)(void*);
    reinterpret_cast<Fn>(address)(mStorage.data());
    configureReplayWorldLocked();
    mConstructed = true;
    return true;
}

bool ReplayWorldSettings::setDefaultSpawn(float x, float y, float z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    constexpr auto minimum = static_cast<double>(std::numeric_limits<int>::min());
    constexpr auto maximum = static_cast<double>(std::numeric_limits<int>::max());
    const double flooredX = std::floor(static_cast<double>(x));
    const double flooredY = std::floor(static_cast<double>(y));
    const double flooredZ = std::floor(static_cast<double>(z));
    if (flooredX < minimum || flooredX > maximum
        || flooredY < minimum || flooredY > maximum
        || flooredZ < minimum || flooredZ > maximum) {
        return false;
    }

    std::scoped_lock lock(mMutex);
    mDefaultSpawn = {
        static_cast<int>(flooredX),
        static_cast<int>(flooredY),
        static_cast<int>(flooredZ)
    };
    mHasDefaultSpawn = true;
    if (mConstructed) configureReplayWorldLocked();
    return true;
}

void ReplayWorldSettings::setInitialTime(int time) {
    std::scoped_lock lock(mMutex);
    mInitialTime = time;
    if (mConstructed) configureReplayWorldLocked();
}

bool ReplayWorldSettings::prepareFresh() {
    std::scoped_lock lock(mMutex);
    if (mConstructed && !destroyLocked()) return false;
    const auto address = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LevelSettingsDefaultCtor);
    if (!address) return false;
    using Fn = void*(*)(void*);
    reinterpret_cast<Fn>(address)(mStorage.data());
    configureReplayWorldLocked();
    mConstructed = true;
    return true;
}

bool ReplayWorldSettings::available() const {
    std::scoped_lock lock(mMutex);
    return mConstructed;
}

const void* ReplayWorldSettings::data() const {
    std::scoped_lock lock(mMutex);
    return mConstructed ? mStorage.data() : nullptr;
}

void ReplayWorldSettings::clear() {
    std::scoped_lock lock(mMutex);
    destroyLocked();
}

}

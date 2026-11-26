#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <span>
#include <vector>

namespace bedrocktools::replay {

inline constexpr std::int32_t ReplayMagic = 0x4C4C5042;
inline constexpr std::int32_t ReplaySnapshotFormatVersion = 3;

struct SerializedPacket {
    std::int32_t id{};
    std::string payload;
};

struct SnapshotContext {
    std::int32_t dimensionId{};
    std::int32_t dimensionMinHeight{};
    std::int32_t dimensionMaxHeight{};
    float x{};
    float y{};
    float z{};
    float yaw{};
    float pitch{};
    std::int32_t worldTime{};
    float rainLevel{};
    float rainTarget{};
    float lightningLevel{};
    float lightningTarget{};
};

struct ReplayEnvironmentState {
    bool setTime{};
    bool setWeather{};
    std::int32_t worldTime{};
    float rainLevel{};
    float rainTarget{};
    float lightningLevel{};
    float lightningTarget{};
};

struct ReplayMovementState {
    std::int64_t actorId{};
    float x{};
    float y{};
    float z{};
    float pitch{};
    float yaw{};
    float headYaw{};
    float bodyYaw{};
    bool onGround{};
    bool gliding{};
    bool glidingKnown{};
};

enum class ReplayAction : std::int32_t {
    NextTick = 0,
    SnapshotContext = 1,
    CreateLocalPlayer = 2,
    LevelChunkCached = 3,
    SubChunkCached = 4,
    ConfigurationPacket = 5,
    GamePacket = 6,
    MoveEntities = 7,
    EnvironmentState = 8,
    Count = 9,
};

std::string_view actionName(ReplayAction action);
std::string encodePacket(const SerializedPacket& packet);
bool decodePacket(std::string_view data, SerializedPacket& packet);
std::string encodeSnapshotContext(const SnapshotContext& context);
bool decodeSnapshotContext(std::string_view data, SnapshotContext& context);
std::string encodeMoveEntities(std::span<const ReplayMovementState> movements);
bool decodeMoveEntities(std::string_view data, std::vector<ReplayMovementState>& movements);
std::string encodeEnvironmentState(const ReplayEnvironmentState& state);
bool decodeEnvironmentState(std::string_view data, ReplayEnvironmentState& state);

class ReplayArchiveWriter {
public:
    bool open(const std::filesystem::path& path);
    bool beginSnapshot();
    bool writeAction(ReplayAction action, std::string_view payload);
    bool endSnapshot();
    bool close();
    void abort();
    const std::filesystem::path& path() const noexcept { return mPath; }

private:
    bool writeHeader();
    bool writeRaw(std::string_view data);
    bool writeVarInt(std::int32_t value);
    bool writeUnsignedInt(std::uint32_t value);
    bool writeString(std::string_view value);

    std::filesystem::path mPath;
    void* mFile{};
    std::uint64_t mSnapshotSizePosition{};
    std::uint64_t mSnapshotDataPosition{};
    bool mSnapshotOpen{};
    bool mOpen{};
};

struct ReplayActionRecord {
    ReplayAction action{};
    std::string payload;
    bool snapshot{};
    std::uint32_t tick{};
};

struct ReplayArchiveData {
    SnapshotContext context{};
    std::string localPlayerPayload;
    std::vector<SerializedPacket> configurationPackets;
    std::vector<SerializedPacket> snapshotPackets;
    std::vector<ReplayActionRecord> timeline;
    std::uint32_t durationTicks{};
};

bool readReplayArchive(const std::filesystem::path& path, ReplayArchiveData& output, std::string* error = nullptr);

}

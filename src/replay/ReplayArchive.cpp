#include "ReplayArchive.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace bedrocktools::replay {
namespace {

constexpr std::array<std::string_view, static_cast<std::size_t>(ReplayAction::Count)> ActionNames{
    "next_tick",
    "snapshot_context",
    "create_local_player",
    "level_chunk_cached",
    "sub_chunk_cached",
    "configuration_packet",
    "game_packet",
    "move_entities",
    "environment_state",
};
constexpr std::uint64_t MaxReplaySize = 2ull * 1024ull * 1024ull * 1024ull;

void appendUnsignedVarInt(std::string& out, std::uint32_t value) {
    while (value >= 0x80u) {
        out.push_back(static_cast<char>((value & 0x7fu) | 0x80u));
        value >>= 7u;
    }
    out.push_back(static_cast<char>(value));
}

void appendVarInt(std::string& out, std::int32_t value) {
    const auto encoded = (static_cast<std::uint32_t>(value) << 1u)
        ^ static_cast<std::uint32_t>(value >> 31);
    appendUnsignedVarInt(out, encoded);
}

void appendUnsignedVarInt64(std::string& out, std::uint64_t value) {
    while (value >= 0x80u) {
        out.push_back(static_cast<char>((value & 0x7fu) | 0x80u));
        value >>= 7u;
    }
    out.push_back(static_cast<char>(value));
}

void appendVarInt64(std::string& out, std::int64_t value) {
    const auto encoded = (static_cast<std::uint64_t>(value) << 1u)
        ^ static_cast<std::uint64_t>(value >> 63);
    appendUnsignedVarInt64(out, encoded);
}

void appendU32(std::string& out, std::uint32_t value) {
    out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void appendFloat(std::string& out, float value) {
    out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void appendString(std::string& out, std::string_view value) {
    appendUnsignedVarInt(out, static_cast<std::uint32_t>(value.size()));
    out.append(value);
}

struct Decoder {
    std::string_view data;
    std::size_t pos{};

    bool unsignedVarInt(std::uint32_t& value) {
        value = 0;
        for (int shift = 0; shift <= 28; shift += 7) {
            if (pos >= data.size()) return false;
            const auto byte = static_cast<std::uint8_t>(data[pos++]);
            value |= static_cast<std::uint32_t>(byte & 0x7fu) << shift;
            if ((byte & 0x80u) == 0) return true;
        }
        return false;
    }

    bool varInt(std::int32_t& value) {
        std::uint32_t raw{};
        if (!unsignedVarInt(raw)) return false;
        value = static_cast<std::int32_t>((raw >> 1u) ^ static_cast<std::uint32_t>(-static_cast<std::int32_t>(raw & 1u)));
        return true;
    }

    bool unsignedVarInt64(std::uint64_t& value) {
        value = 0;
        for (int shift = 0; shift <= 63; shift += 7) {
            if (pos >= data.size()) return false;
            const auto byte = static_cast<std::uint8_t>(data[pos++]);
            if (shift == 63 && (byte & 0xfeu) != 0) return false;
            value |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
            if ((byte & 0x80u) == 0) return true;
        }
        return false;
    }

    bool varInt64(std::int64_t& value) {
        std::uint64_t raw{};
        if (!unsignedVarInt64(raw)) return false;
        value = static_cast<std::int64_t>((raw >> 1u) ^ static_cast<std::uint64_t>(-static_cast<std::int64_t>(raw & 1u)));
        return true;
    }

    bool u32(std::uint32_t& value) {
        if (pos > data.size() || sizeof(value) > data.size() - pos) return false;
        std::memcpy(&value, data.data() + pos, sizeof(value));
        pos += sizeof(value);
        return true;
    }

    bool f32(float& value) {
        if (pos > data.size() || sizeof(value) > data.size() - pos) return false;
        std::memcpy(&value, data.data() + pos, sizeof(value));
        pos += sizeof(value);
        return true;
    }

    bool string(std::string& value) {
        std::uint32_t size{};
        if (!unsignedVarInt(size) || pos > data.size() || size > data.size() - pos) return false;
        value.assign(data.data() + pos, size);
        pos += size;
        return true;
    }

    bool bytes(std::size_t size, std::string_view& value) {
        if (pos > data.size() || size > data.size() - pos) return false;
        value = data.substr(pos, size);
        pos += size;
        return true;
    }
};

ReplayAction actionForName(std::string_view name) {
    for (std::size_t i = 0; i < ActionNames.size(); ++i) {
        if (ActionNames[i] == name) return static_cast<ReplayAction>(i);
    }
    return ReplayAction::Count;
}

}

std::string_view actionName(ReplayAction action) {
    const auto index = static_cast<std::size_t>(action);
    return index < ActionNames.size() ? ActionNames[index] : std::string_view{};
}

std::string encodePacket(const SerializedPacket& packet) {
    std::string out;
    out.reserve(packet.payload.size() + 8);
    appendVarInt(out, packet.id);
    out.append(packet.payload);
    return out;
}

bool decodePacket(std::string_view data, SerializedPacket& packet) {
    Decoder reader{data};
    std::int32_t id{};
    if (!reader.varInt(id)) return false;
    packet.id = id;
    packet.payload.assign(data.data() + reader.pos, data.size() - reader.pos);
    return true;
}

std::string encodeSnapshotContext(const SnapshotContext& context) {
    std::string out;
    out.reserve(64);
    appendVarInt(out, ReplaySnapshotFormatVersion);
    appendVarInt(out, context.dimensionId);
    appendVarInt(out, context.dimensionMinHeight);
    appendVarInt(out, context.dimensionMaxHeight);
    appendFloat(out, context.x);
    appendFloat(out, context.y);
    appendFloat(out, context.z);
    appendFloat(out, context.yaw);
    appendFloat(out, context.pitch);
    appendVarInt(out, context.worldTime);
    appendFloat(out, context.rainLevel);
    appendFloat(out, context.rainTarget);
    appendFloat(out, context.lightningLevel);
    appendFloat(out, context.lightningTarget);
    return out;
}

bool decodeSnapshotContext(std::string_view data, SnapshotContext& context) {
    Decoder reader{data};
    std::int32_t version{};
    if (!reader.varInt(version) || version < 1 || version > ReplaySnapshotFormatVersion) return false;
    if (!reader.varInt(context.dimensionId)
        || !reader.varInt(context.dimensionMinHeight)
        || !reader.varInt(context.dimensionMaxHeight)
        || !reader.f32(context.x)
        || !reader.f32(context.y)
        || !reader.f32(context.z)
        || !reader.f32(context.yaw)
        || !reader.f32(context.pitch)) return false;
    if (version >= 2 && !reader.varInt(context.worldTime)) return false;
    if (version >= 3 && (!reader.f32(context.rainLevel)
        || !reader.f32(context.rainTarget)
        || !reader.f32(context.lightningLevel)
        || !reader.f32(context.lightningTarget))) return false;
    return reader.pos == data.size();
}

std::string encodeMoveEntities(std::span<const ReplayMovementState> movements) {
    std::string out;
    out.reserve(8 + movements.size() * 41);
    appendVarInt(out, 1);
    appendVarInt(out, static_cast<std::int32_t>(movements.size()));
    for (const auto& movement : movements) {
        appendVarInt64(out, movement.actorId);
        appendFloat(out, movement.x);
        appendFloat(out, movement.y);
        appendFloat(out, movement.z);
        appendFloat(out, movement.pitch);
        appendFloat(out, movement.yaw);
        appendFloat(out, movement.headYaw);
        appendFloat(out, movement.bodyYaw);
        out.push_back(movement.onGround ? '\1' : '\0');
        out.push_back(movement.gliding ? '\1' : '\0');
    }
    return out;
}

bool decodeMoveEntities(std::string_view data, std::vector<ReplayMovementState>& movements) {
    Decoder reader{data};
    std::int32_t marker{};
    std::int32_t count{};
    if (!reader.varInt(marker) || (marker != 0 && marker != 1)
        || !reader.varInt(count) || count < 0 || count > 65536) return false;
    std::vector<ReplayMovementState> parsed;
    parsed.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        ReplayMovementState movement;
        if (!reader.varInt64(movement.actorId)
            || !reader.f32(movement.x)
            || !reader.f32(movement.y)
            || !reader.f32(movement.z)
            || !reader.f32(movement.pitch)
            || !reader.f32(movement.yaw)
            || !reader.f32(movement.headYaw)
            || !reader.f32(movement.bodyYaw)
            || reader.pos >= data.size()) return false;
        movement.onGround = data[reader.pos++] != 0;
        if (marker >= 1) {
            if (reader.pos >= data.size()) return false;
            movement.gliding = data[reader.pos++] != 0;
            movement.glidingKnown = true;
        }
        parsed.push_back(movement);
    }
    if (reader.pos != data.size()) return false;
    movements = std::move(parsed);
    return true;
}


std::string encodeEnvironmentState(const ReplayEnvironmentState& state) {
    std::string out;
    out.reserve(24);
    std::uint8_t flags{};
    if (state.setTime) flags |= 0x1;
    if (state.setWeather) flags |= 0x2;
    out.push_back(static_cast<char>(flags));
    if (state.setTime) appendVarInt(out, state.worldTime);
    if (state.setWeather) {
        appendFloat(out, state.rainLevel);
        appendFloat(out, state.rainTarget);
        appendFloat(out, state.lightningLevel);
        appendFloat(out, state.lightningTarget);
    }
    return out;
}

bool decodeEnvironmentState(std::string_view data, ReplayEnvironmentState& state) {
    Decoder reader{data};
    if (reader.pos >= reader.data.size()) return false;
    const auto flags = static_cast<std::uint8_t>(reader.data[reader.pos++]);
    if ((flags & ~0x3u) != 0 || flags == 0) return false;
    ReplayEnvironmentState parsed{};
    parsed.setTime = (flags & 0x1u) != 0;
    parsed.setWeather = (flags & 0x2u) != 0;
    if (parsed.setTime && !reader.varInt(parsed.worldTime)) return false;
    if (parsed.setWeather && (!reader.f32(parsed.rainLevel)
        || !reader.f32(parsed.rainTarget)
        || !reader.f32(parsed.lightningLevel)
        || !reader.f32(parsed.lightningTarget))) return false;
    if (reader.pos != reader.data.size()) return false;
    state = parsed;
    return true;
}

bool ReplayArchiveWriter::open(const std::filesystem::path& path) {
    abort();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;
    auto* file = std::fopen(path.c_str(), "w+b");
    if (!file) return false;
    mFile = file;
    mPath = path;
    mOpen = true;
    if (!writeHeader()) {
        abort();
        return false;
    }
    return true;
}

bool ReplayArchiveWriter::writeRaw(std::string_view data) {
    if (!mOpen || !mFile) return false;
    if (data.empty()) return true;
    return std::fwrite(data.data(), 1, data.size(), static_cast<FILE*>(mFile)) == data.size();
}

bool ReplayArchiveWriter::writeVarInt(std::int32_t value) {
    std::string encoded;
    appendVarInt(encoded, value);
    return writeRaw(encoded);
}

bool ReplayArchiveWriter::writeUnsignedInt(std::uint32_t value) {
    return writeRaw({reinterpret_cast<const char*>(&value), sizeof(value)});
}

bool ReplayArchiveWriter::writeString(std::string_view value) {
    std::string encoded;
    appendString(encoded, value);
    return writeRaw(encoded);
}

bool ReplayArchiveWriter::writeHeader() {
    if (!writeVarInt(ReplayMagic)) return false;
    if (!writeVarInt(static_cast<std::int32_t>(ActionNames.size()))) return false;
    for (auto name : ActionNames) if (!writeString(name)) return false;
    return true;
}

bool ReplayArchiveWriter::beginSnapshot() {
    if (!mOpen || mSnapshotOpen || !mFile) return false;
    const auto position = std::ftell(static_cast<FILE*>(mFile));
    if (position < 0) return false;
    mSnapshotSizePosition = static_cast<std::uint64_t>(position);
    if (!writeUnsignedInt(0)) return false;
    const auto dataPosition = std::ftell(static_cast<FILE*>(mFile));
    if (dataPosition < 0) return false;
    mSnapshotDataPosition = static_cast<std::uint64_t>(dataPosition);
    mSnapshotOpen = true;
    return true;
}

bool ReplayArchiveWriter::writeAction(ReplayAction action, std::string_view payload) {
    if (!mOpen || static_cast<std::size_t>(action) >= ActionNames.size()) return false;
    return writeVarInt(static_cast<std::int32_t>(action))
        && writeUnsignedInt(static_cast<std::uint32_t>(payload.size()))
        && writeRaw(payload);
}

bool ReplayArchiveWriter::endSnapshot() {
    if (!mSnapshotOpen || !mFile) return false;
    const auto end = std::ftell(static_cast<FILE*>(mFile));
    if (end < 0 || static_cast<std::uint64_t>(end) < mSnapshotDataPosition) return false;
    const auto snapshotSize64 = static_cast<std::uint64_t>(end) - mSnapshotDataPosition;
    if (snapshotSize64 > std::numeric_limits<std::uint32_t>::max()) return false;
    if (std::fseek(static_cast<FILE*>(mFile), static_cast<long>(mSnapshotSizePosition), SEEK_SET) != 0) return false;
    const auto size = static_cast<std::uint32_t>(snapshotSize64);
    if (std::fwrite(&size, 1, sizeof(size), static_cast<FILE*>(mFile)) != sizeof(size)) return false;
    if (std::fseek(static_cast<FILE*>(mFile), end, SEEK_SET) != 0) return false;
    mSnapshotOpen = false;
    return true;
}

bool ReplayArchiveWriter::close() {
    if (!mOpen || !mFile) return false;
    if (mSnapshotOpen && !endSnapshot()) return false;
    const bool ok = std::fflush(static_cast<FILE*>(mFile)) == 0
        && ::fsync(::fileno(static_cast<FILE*>(mFile))) == 0;
    const bool closed = std::fclose(static_cast<FILE*>(mFile)) == 0;
    mFile = nullptr;
    mOpen = false;
    return ok && closed;
}

void ReplayArchiveWriter::abort() {
    if (mFile) std::fclose(static_cast<FILE*>(mFile));
    mFile = nullptr;
    mSnapshotOpen = false;
    mOpen = false;
    if (!mPath.empty()) {
        std::error_code ec;
        std::filesystem::remove(mPath, ec);
    }
    mPath.clear();
}

bool readReplayArchive(const std::filesystem::path& path, ReplayArchiveData& output, std::string* error) {
    auto fail = [&](std::string message) {
        output = {};
        if (error) *error = std::move(message);
        return false;
    };
    output = {};
    if (error) error->clear();
    try {
        std::error_code ec;
        const auto fileSize = std::filesystem::file_size(path, ec);
        if (ec || fileSize == 0 || fileSize > MaxReplaySize) return fail("invalid replay size");
        std::ifstream input(path, std::ios::binary);
        if (!input) return fail("unable to open replay");

        std::uint64_t position = 0;
        std::uint64_t limit = fileSize;
        auto readBytes = [&](char* destination, std::size_t size) {
            if (position > limit || size > limit - position) return false;
            if (size && !input.read(destination, static_cast<std::streamsize>(size))) return false;
            position += size;
            return true;
        };
        auto readUnsignedVarInt = [&](std::uint32_t& value) {
            value = 0;
            for (int shift = 0; shift <= 28; shift += 7) {
                char raw{};
                if (!readBytes(&raw, 1)) return false;
                const auto byte = static_cast<std::uint8_t>(raw);
                if (shift == 28 && (byte & 0xf0u) != 0) return false;
                value |= static_cast<std::uint32_t>(byte & 0x7fu) << shift;
                if ((byte & 0x80u) == 0) return true;
            }
            return false;
        };
        auto readVarInt = [&](std::int32_t& value) {
            std::uint32_t raw{};
            if (!readUnsignedVarInt(raw)) return false;
            value = static_cast<std::int32_t>((raw >> 1u)
                ^ static_cast<std::uint32_t>(-static_cast<std::int32_t>(raw & 1u)));
            return true;
        };
        auto readU32 = [&](std::uint32_t& value) {
            return readBytes(reinterpret_cast<char*>(&value), sizeof(value));
        };
        auto skipBytes = [&](std::uint32_t size) {
            if (position > limit || size > limit - position) return false;
            input.seekg(static_cast<std::streamoff>(size), std::ios::cur);
            if (!input) return false;
            position += size;
            return true;
        };

        std::int32_t magic{};
        if (!readVarInt(magic) || magic != ReplayMagic) return fail("invalid replay magic");
        std::int32_t actionCount{};
        if (!readVarInt(actionCount) || actionCount <= 0 || actionCount > 128) return fail("invalid action count");
        std::vector<ReplayAction> actionMap;
        actionMap.reserve(static_cast<std::size_t>(actionCount));
        for (int i = 0; i < actionCount; ++i) {
            std::uint32_t size{};
            if (!readUnsignedVarInt(size) || size > 128) return fail("invalid action table");
            std::string name(size, '\0');
            if (!readBytes(name.data(), name.size())) return fail("invalid action table");
            const auto action = actionForName(name);
            if (action == ReplayAction::Count) return fail("unsupported action: " + name);
            actionMap.push_back(action);
        }
        std::uint32_t snapshotSize{};
        if (!readU32(snapshotSize) || snapshotSize > fileSize - position) return fail("invalid snapshot size");
        const auto snapshotEnd = position + snapshotSize;
        std::uint32_t tick{};
        while (position < fileSize) {
            const bool snapshot = position < snapshotEnd;
            limit = snapshot ? snapshotEnd : fileSize;
            std::int32_t actionId{};
            std::uint32_t payloadSize{};
            if (!readVarInt(actionId) || !readU32(payloadSize)) return fail("invalid replay action");
            if (actionId < 0 || static_cast<std::size_t>(actionId) >= actionMap.size()) return fail("invalid action id");
            if (position > limit || payloadSize > limit - position) return fail("action beyond replay boundary");
            const auto action = actionMap[static_cast<std::size_t>(actionId)];
            if (action == ReplayAction::NextTick) {
                if (!skipBytes(payloadSize)) return fail("invalid tick action");
                if (!snapshot) {
                    if (tick == std::numeric_limits<std::uint32_t>::max()) return fail("replay duration overflow");
                    ++tick;
                }
                continue;
            }
            if (snapshot && (action == ReplayAction::LevelChunkCached || action == ReplayAction::SubChunkCached
                    || action == ReplayAction::MoveEntities || action == ReplayAction::EnvironmentState)) {
                if (!skipBytes(payloadSize)) return fail("invalid snapshot action");
                continue;
            }
            std::string payload(payloadSize, '\0');
            if (!readBytes(payload.data(), payload.size())) return fail("unable to read replay action");
            if (action == ReplayAction::SnapshotContext) {
                if (!decodeSnapshotContext(payload, output.context)) return fail("invalid snapshot context");
            } else if (action == ReplayAction::CreateLocalPlayer) {
                if (!snapshot || !output.localPlayerPayload.empty()) return fail("invalid local player snapshot");
                output.localPlayerPayload = std::move(payload);
            } else if (action == ReplayAction::ConfigurationPacket || action == ReplayAction::GamePacket) {
                Decoder packetReader{payload};
                std::int32_t packetId{};
                if (!packetReader.varInt(packetId)) {
                    return fail("invalid packet payload");
                }
                if (snapshot) {
                    payload.erase(0, packetReader.pos);
                    auto& packets = action == ReplayAction::ConfigurationPacket
                        ? output.configurationPackets : output.snapshotPackets;
                    packets.push_back({packetId, std::move(payload)});
                } else {
                    output.timeline.push_back({action, std::move(payload), false, tick});
                }
            } else if (!snapshot) {
                output.timeline.push_back({action, std::move(payload), false, tick});
            }
        }
        output.durationTicks = tick;
        return true;
    } catch (const std::bad_alloc&) {
        return fail("not enough memory to open this replay");
    } catch (const std::length_error&) {
        return fail("replay action is too large");
    }
}

}

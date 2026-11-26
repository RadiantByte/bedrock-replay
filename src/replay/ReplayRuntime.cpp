#include "ReplayRuntime.hpp"

#include "CinematicCamera.hpp"
#include "NativePacket.hpp"
#include "ReplayContent.hpp"
#include "ReplayRecorder.hpp"
#include "ReplayPlayer.hpp"
#include "ReplayExporter.hpp"
#include "ReplayWorldLauncher.hpp"
#include "ReplayWorldSettings.hpp"
#include "ReplayUi.hpp"
#include "ReplayVisuals.hpp"
#include "core/memory/Hooks.hpp"
#include "core/memory/Rtti.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/events/LocalPlayerPreTickEvent.hpp>
#include <bedrocktools/events/FrameEvent.hpp>
#include <bedrocktools/events/ClientInstanceUpdateEvent.hpp>
#include <bedrocktools/events/LevelStateEvent.hpp>
#include <bedrocktools/events/PacketHandleEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/offsets/Network.hpp>
#include <bedrocktools/sdk/offsets/Core.hpp>
#include <bedrocktools/sdk/network/MinecraftPacketIds.hpp>
#include <bedrocktools/sdk/world/BlockSource.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bedrocktools::replay {
namespace {

using bedrocktools::sdk::MinecraftPacketIds;
using PacketStatsFn = void(*)(void*, const void*, const bedrocktools::sdk::Packet*, std::uint32_t);
using SendToServerFn = void(*)(void*, bedrocktools::sdk::Packet*);
using ScreenHandleFn = std::uint32_t(*)(void*, void*);
using PacketDispatchFn = void(*)(void*, void*, void*, std::shared_ptr<bedrocktools::sdk::Packet>&);
using BlockChangedFn = void(*)(bedrocktools::sdk::BlockSource*, const bedrocktools::sdk::BlockPos*, int,
    const void*, const void*, int, bool, const void*, const void*);

PacketStatsFn packetReceivedOriginal{};
PacketStatsFn packetSentOriginal{};
SendToServerFn sendToServerOriginal{};
ScreenHandleFn screenHandleOriginal{};
PacketDispatchFn availableCommandsDispatchOriginal{};
BlockChangedFn blockChangedOriginal{};
bedrocktools::hooks::Handle packetReceivedHook{};
bedrocktools::hooks::Handle packetSentHook{};
bedrocktools::hooks::Handle sendToServerHook{};
bedrocktools::hooks::Handle screenHandleHook{};
bedrocktools::hooks::Handle availableCommandsDispatchHook{};
bedrocktools::hooks::Handle blockChangedHook{};
std::atomic_bool active{false};
thread_local const bedrocktools::sdk::Packet* directLocalEquipmentPacket{};
thread_local bool decodedBlockUpdateCaptured{};
std::mutex clientNetworkContextMutex;
alignas(8) std::array<std::byte, bedrocktools::sdk::offsets::NetworkIdentifier::Size> clientNetworkIdentifierStorage{};
void* clientNetworkCallback{};
bool clientNetworkContextReady{};

constexpr std::size_t DecodedConfigurationPacketCount = 8;
constexpr auto DecodedPacketIds = std::to_array<MinecraftPacketIds>({
    MinecraftPacketIds::PlayerList,
    MinecraftPacketIds::PlayerSkin,
    MinecraftPacketIds::AvailableActorIDList,
    MinecraftPacketIds::SyncActorProperty,
    MinecraftPacketIds::ItemRegistryPacket,
    MinecraftPacketIds::BiomeDefinitionList,
    MinecraftPacketIds::CreativeContent,
    MinecraftPacketIds::VoxelShapesPacket,
    MinecraftPacketIds::AddPlayer,
    MinecraftPacketIds::UpdateBlock,
    MinecraftPacketIds::UpdateBlockSynced,
    MinecraftPacketIds::UpdateSubChunkBlocks,
    MinecraftPacketIds::PlayerEquipment,
    MinecraftPacketIds::MobArmorEquipment,
    MinecraftPacketIds::TakeItemActor,
    MinecraftPacketIds::RemoveActor,
    MinecraftPacketIds::Animate,
    MinecraftPacketIds::ActorEvent,
    MinecraftPacketIds::SetActorData,
    MinecraftPacketIds::SetActorMotion,
    MinecraftPacketIds::UpdateAttributes,
    MinecraftPacketIds::MobEffect,
    MinecraftPacketIds::SetActorLink,
    MinecraftPacketIds::AddActor,
    MinecraftPacketIds::AddItemActor,
    MinecraftPacketIds::FullChunkData
});
std::array<PacketDispatchFn, DecodedPacketIds.size()> decodedDispatchOriginals{};
std::array<bedrocktools::hooks::Handle, DecodedPacketIds.size()> decodedDispatchHooks{};
std::array<void*, DecodedPacketIds.size()> decodedDispatchTargets{};

struct AvailableEnumData {
    std::string name;
    std::vector<std::uint32_t> values;
};

struct AvailableParamData {
    std::string name;
    std::uint32_t parseSymbol{};
    bool optional{};
    std::uint8_t paramOptions{};
    std::uint16_t padding{};
};

struct AvailableOverloadData {
    std::vector<AvailableParamData> params;
    bool isChaining{};
    std::uint8_t padding[7]{};
};

struct AvailableCommandData {
    std::string name;
    std::string description;
    std::uint16_t flags{};
    std::uint8_t permission{};
    std::uint8_t padding0[5]{};
    std::vector<AvailableOverloadData> overloads;
    std::vector<std::uint32_t> chainedSubcommandIndexes;
    std::int32_t aliasEnum{-1};
    std::uint8_t padding1[4]{};
};

#if defined(__ANDROID__)
static_assert(sizeof(std::string) == 0x18);
static_assert(sizeof(std::vector<std::uint32_t>) == 0x18);
static_assert(sizeof(AvailableEnumData) == 0x30);
static_assert(offsetof(AvailableParamData, parseSymbol) == 0x18);
static_assert(offsetof(AvailableParamData, paramOptions) == 0x1D);
static_assert(sizeof(AvailableParamData) == 0x20);
static_assert(offsetof(AvailableOverloadData, isChaining) == 0x18);
static_assert(sizeof(AvailableOverloadData) == 0x20);
static_assert(offsetof(AvailableCommandData, flags) == 0x30);
static_assert(offsetof(AvailableCommandData, permission) == 0x32);
static_assert(offsetof(AvailableCommandData, overloads) == 0x38);
static_assert(offsetof(AvailableCommandData, chainedSubcommandIndexes) == 0x50);
static_assert(offsetof(AvailableCommandData, aliasEnum) == 0x68);
static_assert(sizeof(AvailableCommandData) == 0x70);
#endif

bool shouldObserveClientTraffic() {
    const auto state = ReplayPlayer::get().state();
    return state == ReplayPlayer::State::Idle;
}

class BlockUpdateCaptureScope {
public:
    explicit BlockUpdateCaptureScope(bool captured) : mPrevious(decodedBlockUpdateCaptured) {
        decodedBlockUpdateCaptured = mPrevious || captured;
    }
    ~BlockUpdateCaptureScope() { decodedBlockUpdateCaptured = mPrevious; }

private:
    bool mPrevious;
};

void blockChangedDetour(
    bedrocktools::sdk::BlockSource* source,
    const bedrocktools::sdk::BlockPos* position,
    int layer,
    const void* block,
    const void* previousBlock,
    int flags,
    bool updateNeighbors,
    const void* syncMessage,
    const void* actorData
) {
    if (active.load(std::memory_order_acquire) && !decodedBlockUpdateCaptured && position
        && shouldObserveClientTraffic()) {
        ReplayRecorder::get().observeBlockChange(source, *position, layer, block, previousBlock,
            static_cast<std::uint32_t>(flags));
    }
    if (blockChangedOriginal) {
        blockChangedOriginal(source, position, layer, block, previousBlock, flags,
            updateNeighbors, syncMessage, actorData);
    }
}

bool installBlockChangedHook() {
    auto** table = bedrocktools::core::rtti::vtable("11BlockSource");
    if (!table) return false;
    auto* target = table[bedrocktools::sdk::offsets::VTable::BlockSource_fireBlockChanged];
    Dl_info image{};
    if (!target || !dladdr(target, &image) || !image.dli_fbase) return false;
    const auto imageBase = reinterpret_cast<std::uintptr_t>(image.dli_fbase);
    constexpr std::uintptr_t BlockChangedOffset = 0x1037b010;
    constexpr std::uintptr_t DimensionIdOffset = 0x103710f4;
    if (reinterpret_cast<std::uintptr_t>(target) != imageBase + BlockChangedOffset
        || reinterpret_cast<std::uintptr_t>(table[bedrocktools::sdk::offsets::VTable::BlockSource_getDimensionId])
            != imageBase + DimensionIdOffset) return false;
    blockChangedHook = bedrocktools::hooks::install(target, reinterpret_cast<void*>(blockChangedDetour),
        reinterpret_cast<void**>(&blockChangedOriginal));
    return blockChangedHook != nullptr;
}

bool usesDecodedCapture(const bedrocktools::sdk::Packet& packet) {
    const auto id = static_cast<MinecraftPacketIds>(native::packetId(packet));
    if (id == MinecraftPacketIds::StartGame
        || id == MinecraftPacketIds::ResourcePacksInfo
        || id == MinecraftPacketIds::ResourcePackStack
        || id == MinecraftPacketIds::AvailableCommands) {
        return true;
    }
    return std::find(DecodedPacketIds.begin(), DecodedPacketIds.end(), id)
        != DecodedPacketIds.end();
}

int trackerType(const void* statistics) {
    if (!statistics) return -1;
    const auto base = reinterpret_cast<std::uintptr_t>(statistics);
    return *reinterpret_cast<const int*>(base + bedrocktools::sdk::offsets::NetworkStatistics::mType);
}

bool isLocalPlayerEquipmentPacket(const bedrocktools::sdk::Packet& packet) {
    const auto id = static_cast<MinecraftPacketIds>(native::packetId(packet));
    return id == MinecraftPacketIds::PlayerEquipment || id == MinecraftPacketIds::MobArmorEquipment;
}

void packetReceivedDetour(
    void* self,
    const void* source,
    const bedrocktools::sdk::Packet* packet,
    std::uint32_t size
) {
    if (active.load(std::memory_order_acquire) && packet && shouldObserveClientTraffic()
        && trackerType(self) == 0 && !usesDecodedCapture(*packet)) {
        ReplayRecorder::get().observeClientPacket(*packet);
    }
    if (packetReceivedOriginal) packetReceivedOriginal(self, source, packet, size);
}

void packetSentDetour(
    void* self,
    const void* target,
    const bedrocktools::sdk::Packet* packet,
    std::uint32_t size
) {
    if (active.load(std::memory_order_acquire) && packet && shouldObserveClientTraffic() && trackerType(self) == 1
        && packet != directLocalEquipmentPacket
        && (!usesDecodedCapture(*packet) || (!sendToServerHook && isLocalPlayerEquipmentPacket(*packet)))) {
        ReplayRecorder::get().observeClientPacket(*packet);
    }
    if (packetSentOriginal) packetSentOriginal(self, target, packet, size);
}

constexpr std::uint32_t CommandSymbolValid = 0x100000u;
constexpr std::uint32_t CommandSymbolEnum = 0x200000u;
constexpr std::uint16_t CommandFlagNotCheat = 1u << 7u;
constexpr std::uint8_t CommandParamOptionEnumAutocompleteExpansion = 1u;
constexpr std::string_view ReplayActionEnumName = "BedrockReplayAction";

void injectReplayCommand(bedrocktools::sdk::Packet& packet) {
    if (native::packetId(packet) != static_cast<int>(MinecraftPacketIds::AvailableCommands)) return;

    const auto payload = reinterpret_cast<std::uintptr_t>(packet.payload());
    auto& enumValues = *reinterpret_cast<std::vector<std::string>*>(
        payload + bedrocktools::sdk::offsets::AvailableCommandsPacketPayload::mEnumValues
    );
    auto& enums = *reinterpret_cast<std::vector<AvailableEnumData>*>(
        payload + bedrocktools::sdk::offsets::AvailableCommandsPacketPayload::mEnums
    );
    auto& commands = *reinterpret_cast<std::vector<AvailableCommandData>*>(
        payload + bedrocktools::sdk::offsets::AvailableCommandsPacketPayload::mCommands
    );

    if (std::any_of(enums.begin(), enums.end(), [](const auto& value) {
        return value.name == ReplayActionEnumName;
    })) return;

    AvailableEnumData actionEnum;
    actionEnum.name = ReplayActionEnumName;
    actionEnum.values.reserve(3);
    for (std::string_view action : {"start", "pause", "stop"}) {
        actionEnum.values.push_back(static_cast<std::uint32_t>(enumValues.size()));
        enumValues.emplace_back(action);
    }
    const auto enumIndex = static_cast<std::uint32_t>(enums.size());
    enums.emplace_back(std::move(actionEnum));

    AvailableParamData actionParam;
    actionParam.name = "action";
    actionParam.parseSymbol = CommandSymbolValid | CommandSymbolEnum | enumIndex;
    actionParam.paramOptions = CommandParamOptionEnumAutocompleteExpansion;

    AvailableOverloadData overload;
    overload.params.emplace_back(std::move(actionParam));

    auto command = std::find_if(commands.begin(), commands.end(), [](const auto& value) {
        return value.name == "record";
    });
    if (command == commands.end()) {
        AvailableCommandData record;
        record.name = "record";
        record.description = "Replay recording controls";
        record.flags = CommandFlagNotCheat;
        record.permission = 0;
        record.overloads.emplace_back(std::move(overload));
        commands.emplace_back(std::move(record));
    } else {
        command->flags |= CommandFlagNotCheat;
        command->permission = 0;
        command->overloads.emplace_back(std::move(overload));
    }
}

void rememberClientNetworkContext(void* networkIdentifier, void* networkCallback) {
    if (!networkIdentifier || !networkCallback) return;
    std::scoped_lock lock(clientNetworkContextMutex);
    std::memcpy(clientNetworkIdentifierStorage.data(), networkIdentifier, clientNetworkIdentifierStorage.size());
    clientNetworkCallback = networkCallback;
    clientNetworkContextReady = true;
}

void clearClientNetworkContext() {
    std::scoped_lock lock(clientNetworkContextMutex);
    clientNetworkCallback = nullptr;
    clientNetworkContextReady = false;
}

void displayReplayMessage(const std::string& message) {
    alignas(8) std::array<std::byte, bedrocktools::sdk::offsets::NetworkIdentifier::Size> networkIdentifier{};
    void* networkCallback{};
    {
        std::scoped_lock lock(clientNetworkContextMutex);
        if (!clientNetworkContextReady) return;
        networkIdentifier = clientNetworkIdentifierStorage;
        networkCallback = clientNetworkCallback;
    }
    auto packet = native::createSystemTextPacket(message);
    if (packet) native::dispatch(packet, networkIdentifier.data(), networkCallback);
}

void availableCommandsDispatchDetour(
    void* dispatcher,
    void* networkIdentifier,
    void* networkCallback,
    std::shared_ptr<bedrocktools::sdk::Packet>& packet
) {
    rememberClientNetworkContext(networkIdentifier, networkCallback);
    if (active.load(std::memory_order_acquire) && packet && shouldObserveClientTraffic()) {
        ReplayRecorder::get().observeClientPacket(*packet);
    }
    if (active.load(std::memory_order_acquire) && packet) injectReplayCommand(*packet);
    if (availableCommandsDispatchOriginal) {
        availableCommandsDispatchOriginal(dispatcher, networkIdentifier, networkCallback, packet);
    }
}

bool installAvailableCommandsDispatchHook() {
    if (availableCommandsDispatchHook) return true;
    auto packet = native::createPacket(static_cast<int>(MinecraftPacketIds::AvailableCommands));
    if (!packet) return false;
    void* dispatcher = native::packetHandler(*packet);
    if (!dispatcher) return false;
    auto** table = *reinterpret_cast<void***>(dispatcher);
    if (!table || !table[2]) return false;
    availableCommandsDispatchHook = bedrocktools::hooks::install(
        table[2],
        reinterpret_cast<void*>(availableCommandsDispatchDetour),
        reinterpret_cast<void**>(&availableCommandsDispatchOriginal)
    );
    return availableCommandsDispatchHook != nullptr;
}

template <std::size_t Index>
void decodedDispatchDetour(
    void* dispatcher,
    void* networkIdentifier,
    void* networkCallback,
    std::shared_ptr<bedrocktools::sdk::Packet>& packet
) {
    if constexpr (Index < DecodedConfigurationPacketCount) rememberClientNetworkContext(networkIdentifier, networkCallback);
    const bool observe = active.load(std::memory_order_acquire) && packet && shouldObserveClientTraffic();
    const auto id = packet ? static_cast<MinecraftPacketIds>(native::packetId(*packet)) : MinecraftPacketIds::EndId;
    const bool spawn = id == MinecraftPacketIds::AddActor || id == MinecraftPacketIds::AddItemActor;
    bool capturedSpawn = false;
    bool capturedPacket = false;
    if (observe) {
        if (spawn) capturedSpawn = ReplayRecorder::get().observeSpawnedActor(*packet);
        else capturedPacket = ReplayRecorder::get().observeClientPacket(*packet);
    }
    if (active.load(std::memory_order_acquire) && packet) {
        if (id == MinecraftPacketIds::ItemRegistryPacket || id == MinecraftPacketIds::BiomeDefinitionList
            || id == MinecraftPacketIds::CreativeContent || id == MinecraftPacketIds::VoxelShapesPacket) {
            ReplayPlayer::get().onConfigurationPacket(packet);
        }
    }
    const BlockUpdateCaptureScope blockCapture(capturedPacket
        && (id == MinecraftPacketIds::UpdateBlock || id == MinecraftPacketIds::UpdateBlockSynced
            || id == MinecraftPacketIds::UpdateSubChunkBlocks));
    if (decodedDispatchOriginals[Index]) {
        decodedDispatchOriginals[Index](dispatcher, networkIdentifier, networkCallback, packet);
    }
    if (observe && spawn && !capturedSpawn && packet) ReplayRecorder::get().observeSpawnedActor(*packet);
}

template <std::size_t Index>
bool installDecodedDispatchHook() {
    auto packet = native::createPacket(static_cast<int>(DecodedPacketIds[Index]));
    if (!packet) return false;
    void* dispatcher = native::packetHandler(*packet);
    if (!dispatcher) return false;
    auto** table = *reinterpret_cast<void***>(dispatcher);
    if (!table || !table[2]) return false;
    void* target = table[2];
    decodedDispatchTargets[Index] = target;
    for (std::size_t previous = 0; previous < Index; ++previous) {
        if (decodedDispatchTargets[previous] == target) return true;
    }
    decodedDispatchHooks[Index] = bedrocktools::hooks::install(
        target,
        reinterpret_cast<void*>(decodedDispatchDetour<Index>),
        reinterpret_cast<void**>(&decodedDispatchOriginals[Index])
    );
    return decodedDispatchHooks[Index] != nullptr;
}

template <std::size_t... Indexes>
bool installDecodedDispatchHooks(std::index_sequence<Indexes...>) {
    return (installDecodedDispatchHook<Indexes>() && ...);
}

std::string normalizedCommand(const bedrocktools::sdk::Packet& packet) {
    const auto base = reinterpret_cast<std::uintptr_t>(packet.payload());
    const auto& command = *reinterpret_cast<const std::string*>(base + bedrocktools::sdk::offsets::CommandRequestPacketPayload::mCommand);
    std::size_t begin = 0;
    while (begin < command.size() && std::isspace(static_cast<unsigned char>(command[begin]))) ++begin;
    std::size_t end = command.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(command[end - 1]))) --end;
    std::string result = command.substr(begin, end - begin);
    for (char& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

bool handleReplayCommand(const bedrocktools::sdk::Packet& packet) {
    if (native::packetId(packet) != static_cast<int>(MinecraftPacketIds::CommandRequest)) return false;
    const auto command = normalizedCommand(packet);
    auto& recorder = ReplayRecorder::get();

    if (command == "/record start" || command == "/playback start"
        || command == "/record resume" || command == "/playback resume") {
        const auto previous = recorder.state();
        const bool result = recorder.start();
        if (result) {
            displayReplayMessage(previous == ReplayRecorder::State::Paused ? "Recording resumed." : "Recording started.");
        } else {
            displayReplayMessage("Recording is already active or cannot be started yet.");
        }
        return true;
    }
    if (command == "/record pause" || command == "/playback pause") {
        const bool result = recorder.pause();
        displayReplayMessage(result ? "Recording paused." : "Recording is not currently running.");
        return true;
    }
    if (command == "/record stop" || command == "/playback stop"
        || command == "/record end" || command == "/playback end") {
        const bool result = recorder.stop();
        displayReplayMessage(result ? recorder.status() : "There is no active recording to stop.");
        return true;
    }
    if (command == "/record" || command.starts_with("/record ")
        || command == "/playback" || command.starts_with("/playback ")) {
        displayReplayMessage("Usage: /record <start|pause|stop>");
        return true;
    }
    return false;
}

bool suppressReplayWorldRequest(const bedrocktools::sdk::Packet& packet) {
    const auto state = ReplayPlayer::get().state();
    if (state != ReplayPlayer::State::LoadingSnapshot && state != ReplayPlayer::State::ResettingSnapshot
        && state != ReplayPlayer::State::Playing
        && state != ReplayPlayer::State::Paused && state != ReplayPlayer::State::Finished) {
        return false;
    }
    return native::packetId(packet) == static_cast<int>(MinecraftPacketIds::SubChunkRequestPacket);
}

void sendToServerDetour(void* sender, bedrocktools::sdk::Packet* packet) {
    const bool captureEquipment = active.load(std::memory_order_acquire) && packet
        && shouldObserveClientTraffic() && isLocalPlayerEquipmentPacket(*packet);
    if (captureEquipment) {
        directLocalEquipmentPacket = packet;
        ReplayRecorder::get().observeClientPacket(*packet);
    }
    if (active.load(std::memory_order_acquire) && packet) {
        if (native::packetId(*packet) == static_cast<int>(MinecraftPacketIds::CommandRequest)) {
            const auto command = normalizedCommand(*packet);
            const auto replayState = ReplayPlayer::get().state();
            if ((replayState == ReplayPlayer::State::Playing || replayState == ReplayPlayer::State::Paused
                    || replayState == ReplayPlayer::State::Finished)
                && (command == "/time" || command.starts_with("/time "))) {
                ReplayPlayer::get().requestManualTimeOverride();
            }
        }
        if (handleReplayCommand(*packet) || suppressReplayWorldRequest(*packet)) {
            if (captureEquipment) directLocalEquipmentPacket = nullptr;
            return;
        }
    }
    if (sendToServerOriginal) sendToServerOriginal(sender, packet);
    if (captureEquipment) directLocalEquipmentPacket = nullptr;
}

constexpr std::uint32_t stringHash(std::string_view value) {
    std::uint32_t hash = 2166136261u;
    for (const char character : value) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 16777619u;
    }
    return hash;
}

constexpr std::uint32_t ReplayOpenLibraryId = stringHash("button.replay_open_library");
constexpr std::uint32_t ConsumeAndRefreshFocus = (1u << 5u) | (1u << 3u);

std::uint32_t screenHandleDetour(void* controller, void* screenEvent) {
    if (active.load(std::memory_order_acquire) && screenEvent) {
        const auto base = reinterpret_cast<std::uintptr_t>(screenEvent);
        const auto type = *reinterpret_cast<const std::int32_t*>(base);
        if (type == 1) {
            const auto id = *reinterpret_cast<const std::uint32_t*>(base + 0x8);
            const auto state = *reinterpret_cast<const std::int8_t*>(base + 0x11);
            if (id == ReplayOpenLibraryId && state == 1) {

                const auto result = screenHandleOriginal ? screenHandleOriginal(controller, screenEvent) : 0;
                ReplayUi::get().requestBrowserOpen();
                return result | ConsumeAndRefreshFocus;
            }
        }
    }
    return screenHandleOriginal ? screenHandleOriginal(controller, screenEvent) : 0;
}

bool installHook(bedrocktools::memory::SignatureId id, void* detour, void** original, bedrocktools::hooks::Handle& handle) {
    const auto address = bedrocktools::memory::resolve(id);
    if (!address) return false;

    handle = id == bedrocktools::memory::SignatureId::ScreenControllerHandleEvent
        ? bedrocktools::hooks::installShared(reinterpret_cast<void*>(address), detour, original)
        : bedrocktools::hooks::install(reinterpret_cast<void*>(address), detour, original);
    return handle != nullptr;
}

}

ReplayRuntime& ReplayRuntime::get() {
    static ReplayRuntime runtime;
    return runtime;
}

bool ReplayRuntime::install() {
    if (mInstalled) return true;
    const bool hooksReady = installHook(bedrocktools::memory::SignatureId::ScreenControllerHandleEvent, reinterpret_cast<void*>(screenHandleDetour), reinterpret_cast<void**>(&screenHandleOriginal), screenHandleHook)
        && installHook(bedrocktools::memory::SignatureId::NetworkStatisticsPacketReceivedFrom, reinterpret_cast<void*>(packetReceivedDetour), reinterpret_cast<void**>(&packetReceivedOriginal), packetReceivedHook)
        && installHook(bedrocktools::memory::SignatureId::NetworkStatisticsPacketSentTo, reinterpret_cast<void*>(packetSentDetour), reinterpret_cast<void**>(&packetSentOriginal), packetSentHook)
        && installAvailableCommandsDispatchHook()
        && content::install()
        && installDecodedDispatchHooks(std::make_index_sequence<DecodedPacketIds.size()>{})
        && installBlockChangedHook();
    if (!hooksReady) {
        uninstall();
        return false;
    }
    installHook(bedrocktools::memory::SignatureId::LoopbackPacketSenderSendToServer, reinterpret_cast<void*>(sendToServerDetour), reinterpret_cast<void**>(&sendToServerOriginal), sendToServerHook);
    if (!ReplayVisuals::get().install()) {
        uninstall();
        return false;
    }
    ReplayUi::get().installInput();
    bedrocktools::events::bus().subscribe<bedrocktools::events::FrameEvent>([](auto&) {
        CinematicCamera::get().beginRenderFrame();
        ReplayExporter::get().frame();
        if (active.load(std::memory_order_acquire)) {
            auto& ui = ReplayUi::get();
            ui.applyAdvancedLookInput();
            ui.frame();
        }
    }, bedrocktools::events::EventPriority::First);
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerPreTickEvent>([](auto& event) {
        if (active.load(std::memory_order_acquire)) ReplayUi::get().applyAdvancedCameraInput(event.player);
    });
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) {
        if (!active.load(std::memory_order_acquire)) return;
        auto& recorder = ReplayRecorder::get();
        const auto before = recorder.state();
        recorder.tick(event.player);
        if (before == ReplayRecorder::State::Starting && recorder.state() == ReplayRecorder::State::Idle) {
            displayReplayMessage(recorder.status());
        }
        ReplayPlayer::get().tick(event.player);
        ReplayPlayer::get().refreshPlayerViews(event.player);
        CinematicCamera::get().onPlayerTick(event.player);
    });
    bedrocktools::events::bus().subscribe<bedrocktools::events::ClientInstanceUpdateEvent>([](auto& event) {
        ReplayWorldLauncher::get().setClientInstance(event.clientInstance);
        content::flushRelease();
        if (!active.load(std::memory_order_acquire)) return;
        ReplayWorldLauncher::get().observeWorldStorage();
        ReplayRecorder::get().onClientUpdate();
        auto& replayPlayer = ReplayPlayer::get();
        replayPlayer.onClientUpdate();
        if (replayPlayer.state() == ReplayPlayer::State::Idle) {
            ReplayWorldLauncher::get().cleanupExitedWorlds();
        }
        if (ReplayExporter::get().consumeReturnToBrowser() && ReplayPlayer::get().state() == ReplayPlayer::State::Idle) ReplayUi::get().openBrowser();
    });
    bedrocktools::events::bus().subscribe<bedrocktools::events::LevelStateEvent>([](auto& event) {
        if (!active.load(std::memory_order_acquire)) return;
        ReplayPlayer::get().onLevelState(event);
        if (event.phase == bedrocktools::events::LevelPhase::Initializing && event.levelName) {
            ReplayRecorder::get().setCurrentWorldName(*event.levelName);
        }
        if (event.phase == bedrocktools::events::LevelPhase::Destroyed) {
            ReplayUi::get().clearAdvancedLookPlayer();
            clearClientNetworkContext();
            ReplayRecorder::get().stop();
            ReplayRecorder::get().resetWorldCache();
            return;
        }

    });
    bedrocktools::events::bus().subscribe<bedrocktools::events::PacketHandleEvent>([](auto& event) {
        if (!active.load(std::memory_order_acquire)) return;
        if (shouldObserveClientTraffic() && event.packet && *event.packet) {
            ReplayRecorder::get().observeClientPacket(**event.packet);
        }
        ReplayPlayer::get().onPacketHandle(event);
    }, bedrocktools::events::EventPriority::First);
    mInstalled = true;
    active.store(true, std::memory_order_release);
    return true;
}

void ReplayRuntime::uninstall() {
    active.store(false, std::memory_order_release);
    CinematicCamera::get().endSession();
    ReplayVisuals::get().uninstall();
    content::uninstall();
    if (blockChangedHook) bedrocktools::hooks::remove(blockChangedHook);
    for (auto& handle : decodedDispatchHooks) {
        if (handle) bedrocktools::hooks::remove(handle);
        handle = nullptr;
    }
    if (availableCommandsDispatchHook) bedrocktools::hooks::remove(availableCommandsDispatchHook);
    if (sendToServerHook) bedrocktools::hooks::remove(sendToServerHook);
    if (packetSentHook) bedrocktools::hooks::remove(packetSentHook);
    if (packetReceivedHook) bedrocktools::hooks::remove(packetReceivedHook);
    if (screenHandleHook) bedrocktools::hooks::remove(screenHandleHook);
    availableCommandsDispatchHook = nullptr;
    blockChangedHook = nullptr;
    sendToServerHook = nullptr;
    packetSentHook = nullptr;
    packetReceivedHook = nullptr;
    screenHandleHook = nullptr;
    availableCommandsDispatchOriginal = nullptr;
    blockChangedOriginal = nullptr;
    sendToServerOriginal = nullptr;
    packetSentOriginal = nullptr;
    packetReceivedOriginal = nullptr;
    screenHandleOriginal = nullptr;
    decodedDispatchOriginals.fill(nullptr);
    decodedDispatchTargets.fill(nullptr);
    directLocalEquipmentPacket = nullptr;
    decodedBlockUpdateCaptured = false;
    clearClientNetworkContext();
    mInstalled = false;
}

void ReplayRuntime::setEnabled(bool enabled) {
    active.store(enabled && mInstalled, std::memory_order_release);
    if (!enabled) {
        ReplayRecorder::get().stop();
        ReplayUi::get().closeBrowser();
        ReplayExporter::get().cancel();
        ReplayPlayer::get().stop();
        content::release();
        ReplayWorldLauncher::get().setClientInstance(nullptr);
        clearClientNetworkContext();
    }
}

bool ReplayRuntime::enabled() const noexcept { return active.load(std::memory_order_acquire); }

}

#include "PacketLifecycle.hpp"

#include <array>

namespace bedrocktools::replay {
namespace {

using bedrocktools::sdk::MinecraftPacketIds;

struct Entry {
    MinecraftPacketIds id;
    PacketLifecycleSemantics semantics;
};

constexpr auto entries = std::to_array<Entry>({
    {MinecraftPacketIds::MoveAbsoluteActor, {PacketLifecycle::Ignore}},
    {MinecraftPacketIds::MovePlayer, {PacketLifecycle::Ignore}},
    {MinecraftPacketIds::NetworkChunkPublisherUpdate, {PacketLifecycle::Ignore}},
    {MinecraftPacketIds::ChunkRadiusUpdated, {PacketLifecycle::Ignore}},
    {MinecraftPacketIds::ResourcePacksInfo, {PacketLifecycle::PreWorldHandshake, false, true}},
    {MinecraftPacketIds::ResourcePackStack, {PacketLifecycle::PreWorldHandshake}},
    {MinecraftPacketIds::StartGame, {PacketLifecycle::PreWorldHandshake}},
    {MinecraftPacketIds::AvailableActorIDList, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::BiomeDefinitionList, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::CreativeContent, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::AvailableCommands, {PacketLifecycle::SnapshotLatest, true, false, MinecraftPacketIds::UpdateSoftEnum}},
    {MinecraftPacketIds::ItemRegistryPacket, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::SyncActorProperty, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::DimensionDataPacket, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::FeatureRegistryPacket, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::CameraPresets, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::CompressedBiomeDefinitionListDeprecated, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::TrimData, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::VoxelShapesPacket, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::CameraSpline, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::CameraAimAssistActorPriority, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::CraftingData, {PacketLifecycle::SnapshotSequence}},
    {MinecraftPacketIds::UpdateSoftEnum, {PacketLifecycle::SnapshotSequence}},
    {MinecraftPacketIds::CameraAimAssistPresets, {PacketLifecycle::SnapshotSequence}},
});

}

PacketLifecycleSemantics describePacketLifecycle(MinecraftPacketIds id) {
    if (isRecordedContainerUiPacket(id)) return {PacketLifecycle::Ignore};
    for (const auto& entry : entries) if (entry.id == id) return entry.semantics;
    return {};
}

bool isRecordedContainerUiPacket(MinecraftPacketIds id) {
    switch (id) {
    case MinecraftPacketIds::ContainerOpen:
    case MinecraftPacketIds::ContainerClose:
    case MinecraftPacketIds::InventoryContent:
    case MinecraftPacketIds::InventorySlot:
    case MinecraftPacketIds::ContainerSetData:
    case MinecraftPacketIds::GuiDataPickItem:
    case MinecraftPacketIds::UpdateTrade:
    case MinecraftPacketIds::UpdateEquip:
    case MinecraftPacketIds::ItemStackResponse:
        return true;
    default:
        return false;
    }
}

}

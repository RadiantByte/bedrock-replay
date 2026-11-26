#pragma once

#include <cstddef>

namespace bedrocktools::sdk::offsets {
namespace ReplayContent {
inline constexpr std::size_t StartGameBlockProperties = 0x590;
inline constexpr std::size_t ServerBlockPropertySize = 0x38;
inline constexpr std::size_t ServerBlockPropertyTag = 0x18;
inline constexpr std::size_t DefinitionFirstNode = 0x38;
inline constexpr std::size_t DefinitionNodeValue = 0x28;
inline constexpr std::size_t DefinitionName = 0x50;
inline constexpr std::size_t DefinitionDescriptions = 0x158;
inline constexpr std::size_t TypeName = 0xD0;
inline constexpr std::size_t TypeStorage = 0x28;
inline constexpr std::size_t TypePermutations = 0x1E8;
inline constexpr std::size_t BlockStorage = 0x8;
inline constexpr std::size_t BlockNetworkHash = 0x118;
inline constexpr std::size_t BlockNetworkRuntimeId = 0x11C;
inline constexpr std::size_t StorageModifiable = 0x48;
inline constexpr std::size_t StorageReplaceable = 0x49;
inline constexpr std::size_t CompoundBeginNode = 0x8;
inline constexpr std::size_t CompoundEndNode = 0x10;
inline constexpr std::size_t CompoundNodeTag = 0x38;
inline constexpr std::size_t CompoundNodeVariant = 0x60;
inline constexpr std::size_t StringTagValue = 0x8;
inline constexpr std::size_t ClientGeometrySlot = 91;
inline constexpr std::size_t ClientRepositorySlot = 94;
inline constexpr std::size_t ClientResourcesSlot = 95;
inline constexpr std::size_t RepositoryAddCacheSlot = 18;
inline constexpr std::size_t RepositoryRemoveCacheSlot = 24;
inline constexpr std::size_t ResourcesLoadSlot = 2;
inline constexpr std::size_t LoadedResourceSize = 0xB8;
inline constexpr std::size_t DescriptionNameSlot = 2;
inline constexpr std::size_t DescriptionInitializeSlot = 3;
inline constexpr std::size_t LevelPaletteSlot = 87;
inline constexpr std::size_t LevelGlobalPaletteSlot = 88;
inline constexpr std::size_t PaletteBegin = 0x48;
inline constexpr std::size_t PaletteEnd = 0x50;
inline constexpr std::size_t PaletteHashRoot = 0x70;
inline constexpr std::size_t PaletteHashNodeKey = 0x20;
inline constexpr std::size_t PaletteHashNodeIndex = 0x28;
inline constexpr std::size_t UpdateBlockPosition = 0x2C;
inline constexpr std::size_t UpdateBlockLayer = 0x8;
inline constexpr std::size_t UpdateBlockFlags = 0xC;
inline constexpr std::size_t UpdateBlockRuntimeId = 0x10;
inline constexpr std::size_t UpdateBlockSyncedRuntimeId = 0x14;
inline constexpr std::size_t EquipmentItemDescriptor = 0x8;
inline constexpr std::size_t AddPlayerItemDescriptor = 0x70;
inline constexpr std::size_t AddItemActorItemDescriptor = 0x28;
inline constexpr std::size_t ItemDescriptorBlockRuntimeId = 0x30;
inline constexpr std::size_t SubChunkChangesExtra = 0x18;
inline constexpr std::size_t SubChunkChangeSize = 0x28;
inline constexpr std::size_t SubChunkChangeRuntimeId = 0xC;
inline constexpr std::size_t LegacyMaterialSize = 0x60;
inline constexpr std::size_t ResourceLocationSize = 0x30;
inline constexpr std::size_t MinEngineVersionSize = 0x20;
}
}

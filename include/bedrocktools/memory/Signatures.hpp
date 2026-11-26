#pragma once

#include <bedrocktools/Export.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bedrocktools::memory {

enum class SignatureId : std::uint16_t {
    VersionString,
    NormalTick,
    Time,
    SetTime,
    LevelInit,
    LevelDtor,
    ActorManagerList,
    GetFov,
    ClientInstanceUpdate,
    ClientInstanceGetMinecraftGame,
    RenderLevel,
    LocalPlayerApplyTurnDelta,
    LocalPlayerSwing,
    ActorIsPlayer,
    ActorGetOrCreateUniqueID,
    ActorGetRuntimeID,
    LoopbackPacketSenderSendToServer,
    MinecraftPacketsCreatePacket,
    RenderChunkCoordinatorSetAllDirty,
    ResourcePacksInfoPacketHandle,
    ResourcePackStackPacketHandle,
    ItemInHandRendererRenderFirstPerson,
    NetworkStatisticsPacketSentTo,
    NetworkStatisticsPacketReceivedFrom,
    MinecraftGameCanStartLocalServer,
    MinecraftGameStartLocalServerAsync,
    LevelSettingsDefaultCtor,
    LevelSettingsCopyCtor,
    LevelSettingsDtor,
    SubChunkSerialize,
    LevelChunkSerializeBiomes,
    LevelChunkSerializeBorderBlocks,
    LevelChunkSerializeBlockEntities,
    LevelChunkSerializeBlockEntitiesForSubChunk,
    LevelChunkPopulateHeightMapDataForSubChunkPacket,
    ScreenControllerHandleEvent,
    LevelRendererPlayerSetupCamera,
    FrustumCullerSetFromMatrix,
    LevelRendererCameraPreRenderUpdate,
    LevelRendererPlayerUpdateViewArea,
    ClientInstanceGetRenderCameraComponent,
    ReplayLevelLoadBlockDefinitions,
    BlockDefinitionGroupRegisterBlocks,
    BlockDefinitionGroupDigestServerBlockProperties,
    BlockTypeRegistryFinalizeForRendering,
    GeometryGroupLoadModelPackFromStringSync,
    ReplayPacksInfoCollectKeys,
    ReplayResourceLocationCtor,
    ReplayMinEngineVersionFromString,
    ReplayInternedStringDtor,
    ReplayBlockFinalizeComponentStorage,
    ReplayLegacyBlockMaterialCtor,
    ReplayBlockCacheComponentData,
    ReplayActorRenderDispatcherRender,
    ReplayLoadAllPackResources,
    ReplayLoadedResourceDtor,
    Count
};

inline constexpr std::size_t SignatureCount = static_cast<std::size_t>(SignatureId::Count);


BEDROCKTOOLS_API bool resolveAll(std::string_view libraryName = "libminecraftpe.so");
BEDROCKTOOLS_API std::uintptr_t resolve(SignatureId id);
BEDROCKTOOLS_API void clear();

}

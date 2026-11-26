#pragma once

#include <bedrocktools/memory/Signatures.hpp>
#include <array>
#include <string_view>

namespace bedrockreplay::scanner {

using bedrocktools::memory::SignatureId;

struct SignaturePattern {
    SignatureId id;
    bool required;
    std::string_view bytes;
};

inline constexpr std::array<SignaturePattern, bedrocktools::memory::SignatureCount> SignaturePatterns{{
    {SignatureId::VersionString, false, "FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 F3 03 08 AA ? ? ? ? E1 03 00 AA ? ? ? 90"},
    {SignatureId::NormalTick, true, "? ? ? FC ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D1 54 D0 3B D5 F3 03 00 AA ? ? ? F9 ? ? ? F8 ? ? ? 39"},
    {SignatureId::Time, true, "? ? ? D1 ? ? ? A9 ? ? ? F9 ? ? ? A9 ? ? ? 91 55 D0 3B D5 ? ? ? F9 ? ? ? F8 ? ? ? F9 ? ? ? F9 00 01 3F D6 ? ? ? F9"},
    {SignatureId::SetTime, true, "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? 91 54 D0 3B D5 F3 03 01 2A ? ? ? F9 ? ? ? F8 ? ? ? F9 ? ? ? F9 00 01 3F D6"},
    {SignatureId::LevelInit, true, "FF 03 07 D1 FD 7B 16 A9 FC 6F 17 A9 FA 67 18 A9 F8 5F 19 A9 F6 57 1A A9 F4 4F 1B A9 FD 83 05 91 E5 87 01 A9"},
    {SignatureId::LevelDtor, true, "FF 43 02 D1 FD 7B 05 A9 F8 5F 06 A9 F6 57 07 A9 F4 4F 08 A9 FD 43 01 91 56 D0 3B D5 ? ? ? 90 29 ? ? 91"},
    {SignatureId::ActorManagerList, true, "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? F9 ? ? ? A9 ? ? ? F9 ? ? ? B4 F4 03 00 AA"},
    {SignatureId::GetFov, true, "? ? ? FC ? ? ? 6D ? ? ? A9 ? ? ? F9 ? ? ? A9 ? ? ? 91 08 40 20 1E F4 03 01 2A"},
    {SignatureId::ClientInstanceUpdate, true, "E8 0F 19 FC FD 7B 01 A9 FC 6F 02 A9 FA 67 03 A9 F8 5F 04 A9 F6 57 05 A9 F4 4F 06 A9 FD 43 00 91 FF 03 13 D1 59 D0 3B D5 DA 1B 98 52"},
    {SignatureId::ClientInstanceGetMinecraftGame, true, "? ? ? F9 C0 03 5F D6 ? ? ? D1 ? ? ? A9 ? ? ? F9 ? ? ? A9 ? ? ? 91 55 D0 3B D5 F3 03 02 AA F4 03 01 AA"},
    {SignatureId::RenderLevel, true, "FF 83 07 D1 EE AB 00 FD ED 33 16 6D EB 2B 17 6D E9 23 18 6D"},
    {SignatureId::LocalPlayerApplyTurnDelta, true, "? ? ? D1 ? ? ? FD ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 56 D0 3B D5 F3 03 00 AA F4 03 01 AA ? ? ? F9 ? ? ? F8 ? ? ? F9 ? ? ? F9"},
    {SignatureId::LocalPlayerSwing, false, "FF C3 02 D1 FD 7B 07 A9 F7 43 00 F9 F6 57 09 A9 F4 4F 0A A9 FD C3 01 91 57 D0 3B D5 F5 03 02 2A F6 03 01 2A"},
    {SignatureId::ActorIsPlayer, true, "? ? ? F9 ? ? ? 52 ? ? ? 72 ? ? ? A9 08 01 0B CB ? ? ? D3 ? ? ? 51 0C 01 0A 8A ? ? ? B9 ? ? ? 8B ? ? ? F9 ? ? ? F9 ? ? ? B1 ? ? ? 54 ? ? ? 8B ? ? ? B9 BF 01 0A 6B ? ? ? 54 ? ? ? F9 7F 01 09 EB ? ? ? 54 ? ? ? F9 ? ? ? B4 ? ? ? A9 ? ? ? D3 6B 01 09 CB ? ? ? EB ? ? ? 54 ? ? ? F8 ? ? ? B4 ? ? ? 92 ? ? ? 12 ? ? ? 92 ? ? ? B8 28 01 08 4A ? ? ? 12 1F 01 09 6B E0 27 9F 1A C0 03 5F D6 E0 03 1F 2A C0 03 5F D6 ? ? ? D1 ? ? ? FD"},
    {SignatureId::ActorGetOrCreateUniqueID, true, "? ? ? A9 ? ? ? F9 FD 03 00 91 ? ? ? F9 ? ? ? 52 ? ? ? 72 ? ? ? B9 ? ? ? F9 08 01 0B 4B ? ? ? 53 ? ? ? 51 0C 01 0A 8A ? ? ? B9 ? ? ? 8B ? ? ? F9 ? ? ? F9 ? ? ? B1 ? ? ? 54 ? ? ? 8B ? ? ? B9 BF 01 0A 6B ? ? ? 54 ? ? ? F9 7F 01 09 EB ? ? ? 54 ? ? ? F9 ? ? ? B4 ? ? ? A9 ? ? ? D3 8C 01 0A CB ? ? ? EB ? ? ? 54 ? ? ? F8 ? ? ? B4 ? ? ? 92 ? ? ? 12 ? ? ? 92 ? ? ? B8 ? ? ? 32 48 01 08 4A 1F 01 0B 6B ? ? ? 54 ? ? ? D3"},
    {SignatureId::ActorGetRuntimeID, true, "? ? ? A9 FD 03 00 91 ? ? ? F9 ? ? ? 52 ? ? ? 72 ? ? ? A9 08 01 0B CB ? ? ? D3 ? ? ? 51 0C 01 0A 8A ? ? ? B9 ? ? ? 8B ? ? ? F9 ? ? ? F9 ? ? ? B1 ? ? ? 54 ? ? ? 8B ? ? ? B9 BF 01 0A 6B ? ? ? 54 ? ? ? F9 7F 01 09 EB ? ? ? 54 ? ? ? F9 ? ? ? B4 ? ? ? A9 ? ? ? D3 8C 01 0A CB ? ? ? EB ? ? ? 54 ? ? ? F8 ? ? ? B4 ? ? ? 92 ? ? ? 12 ? ? ? 92 ? ? ? B8 ? ? ? 32 48 01 08 4A 1F 01 0B 6B ? ? ? 54 ? ? ? D3 ? ? ? F9 ? ? ? 92 ? ? ? F8 ? ? ? B4 ? ? ? 92 ? ? ? 92 ? ? ? F8 ? ? ? A8 C0 03 5F D6 ? ? ? 96 ? ? ? A9 FD 03 00 91 ? ? ? 94"},
    {SignatureId::LoopbackPacketSenderSendToServer, true, "FD 7B BD A9 FC 57 01 A9 F4 4F 02 A9 FD 03 00 91 FF C3 08 D1 54 D0 3B D5 88 16 40 F9"},
    {SignatureId::MinecraftPacketsCreatePacket, true, "1F A4 05 71 08 01 00 54 E9 03 00 2A ? ? ? B0 4A ? ? 91"},
    {SignatureId::RenderChunkCoordinatorSetAllDirty, true, "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 48 D0 3B D5 F4 03 02 2A F3 03 00 AA"},
    {SignatureId::ResourcePacksInfoPacketHandle, true, "48 00 40 F9 E0 03 02 AA 62 00 40 F9 03 75 40 F9 60 00 1F D6"},
    {SignatureId::ResourcePackStackPacketHandle, true, "48 00 40 F9 E0 03 02 AA 62 00 40 F9 03 79 40 F9 60 00 1F D6"},
    {SignatureId::ItemInHandRendererRenderFirstPerson, true, "FF C3 07 D1 EF 3B 15 6D ED 33 16 6D EB 2B 17 6D E9 23 18 6D FD 7B 19 A9 FC 6F 1A A9 FA 67 1B A9 F8 5F 1C A9 F6 57 1D A9 F4 4F 1E A9 FD 43 06 91 5C D0 3B D5 F4 03 00 AA"},
    {SignatureId::NetworkStatisticsPacketSentTo, true, "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 56 D0 3B D5 F5 03 00 AA E0 03 02 AA ? ? ? F9 F3 03 03 2A F4 03 01 AA ? ? ? F8 ? ? ? F9 ? ? ? F9 00 01 3F D6 E1 03 00 2A"},
    {SignatureId::NetworkStatisticsPacketReceivedFrom, true, "? ? ? D1 ? ? ? A9 ? ? ? F9 ? ? ? A9 ? ? ? A9 ? ? ? 91 57 D0 3B D5 F4 03 00 AA E0 03 02 AA ? ? ? F9 F5 03 03 2A F6 03 02 AA F3 03 01 AA ? ? ? F8 ? ? ? F9 ? ? ? F9"},
    {SignatureId::MinecraftGameCanStartLocalServer, true, "? ? ? 91 ? ? ? 08 ? ? ? 37 ? ? ? 91 ? ? ? 88 1F 01 00 71 E0 17 9F 1A C0 03 5F D6 E0 03 1F 2A C0 03 5F D6 ? ? ? 91"},
    {SignatureId::MinecraftGameStartLocalServerAsync, true, "FD 7B BA A9 FC 6F 01 A9 FA 67 02 A9 F8 5F 03 A9 F6 57 04 A9 F4 4F 05 A9 FD 03 00 91 FF C3 13 D1 5C D0 3B D5 F3 03 08 AA F8 03 06 2A"},
    {SignatureId::LevelSettingsDefaultCtor, true, "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 56 D0 3B D5 F3 03 00 AA E1 03 1F 2A ? ? ? F9 ? ? ? F8 ? ? ? 12 ? ? ? B9 ? ? ? 52 ? ? ? B9 ? ? ? 52 ? ? ? F9 ? ? ? 39 ? ? ? 39 ? ? ? B9"},
    {SignatureId::LevelSettingsCopyCtor, true, "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 48 D0 3B D5 F6 03 01 AA F3 03 00 AA ? ? ? F9 ? ? ? F9 ? ? ? F8 ? ? ? 3C ? ? ? 3C"},
    {SignatureId::LevelSettingsDtor, true, "? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 F3 03 00 AA ? ? ? F9 ? ? ? B4 ? ? ? F9 ? ? ? 95 ? ? ? 91 ? ? ? 97 ? ? ? 39 ? ? ? 34 ? ? ? 91 ? ? ? 97 ? ? ? 39"},
    {SignatureId::SubChunkSerialize, true, "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 57 D0 3B D5 F3 03 01 AA F6 03 00 AA ? ? ? F9 E0 03 01 AA F5 03 02 2A"},
    {SignatureId::LevelChunkSerializeBiomes, true, "? ? ? D1 ? ? ? A9 ? ? ? F9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 54 D0 3B D5 F3 03 01 AA ? ? ? F9 ? ? ? F8 ? ? ? 52"},
    {SignatureId::LevelChunkSerializeBorderBlocks, true, "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 48 D0 3B D5 F3 03 01 AA F9 03 1F AA ? ? ? F9"},
    {SignatureId::LevelChunkSerializeBlockEntities, true, "? ? ? D1 ? ? ? A9 ? ? ? F9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 57 D0 3B D5 F3 03 00 AA F5 03 02 AA ? ? ? F9 F4 03 01 AA ? ? ? F8 ? ? ? F9 ? ? ? F9 ? ? ? F9 ? ? ? 91"},
    {SignatureId::LevelChunkSerializeBlockEntitiesForSubChunk, true, "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 59 D0 3B D5 F5 03 00 AA F3 03 03 AA ? ? ? F9 F4 03 02 AA F6 03 01 AA"},
    {SignatureId::LevelChunkPopulateHeightMapDataForSubChunkPacket, true, "FD 7B BD A9 F5 0B 00 F9 F4 4F 02 A9 FD 03 00 91 F4 03 00 AA 00 18 40 F9 35 3C 40 93"},
    {SignatureId::ScreenControllerHandleEvent, true, "? ? ? A9 ? ? ? F9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? F9 ? ? ? F9 F3 03 00 AA F5 03 01 AA F4 03 1F 2A 1F 01 16 EB ? ? ? 54 ? ? ? 91 ? ? ? F8 E1 03 15 AA"},
    {SignatureId::LevelRendererPlayerSetupCamera, true, "FF 03 04 D1 EE 3B 00 FD ED 33 08 6D EB 2B 09 6D E9 23 0A 6D FD 7B 0B A9 FA 67 0C A9 F8 5F 0D A9 F6 57 0E A9 F4 4F 0F A9 FD C3 02 91 59 D0 3B D5 F3 03 00 AA"},
    {SignatureId::FrustumCullerSetFromMatrix, true, "FF 83 02 D1 EF 3B 06 6D ED 33 07 6D EB 2B 08 6D E9 23 09 6D 20 0C 40 BD 21 00 40 BD 28 00 80 52 00 38 21 1E 00 00 00 BD 04 08 20 1E 21 1C 40 BD"},
    {SignatureId::LevelRendererCameraPreRenderUpdate, true, "FF 43 04 D1 EA 53 00 FD E9 A3 0A 6D FD FB 0B A9 FC 67 00 F9 FA 67 0D A9 F8 5F 0E A9 F6 57 0F A9 F4 4F 10 A9 FD E3 02 91 5A D0 3B D5 F3 03 00 AA F5 03 02 AA 48 17 40 F9"},
    {SignatureId::LevelRendererPlayerUpdateViewArea, true, "EB 2B B8 6D E9 23 01 6D FD 7B 02 A9 FC 6F 03 A9 FA 67 04 A9 F8 5F 05 A9 F6 57 06 A9 F4 4F 07 A9 FD 83 00 91 FF 03 08 D1"},
    {SignatureId::ClientInstanceGetRenderCameraComponent, true, "FF 83 01 D1 FD 7B 03 A9 F6 57 04 A9 F4 4F 05 A9 FD C3 00 91 56 D0 3B D5 C8 16 40 F9 A8 83 1F F8 08 00 40 F9 09 95 42 F9"},
    {SignatureId::ReplayLevelLoadBlockDefinitions, true, "FF 03 02 D1 FD 7B 03 A9 FA 67 04 A9 F8 5F 05 A9 F6 57 06 A9 F4 4F 07 A9 FD C3 00 91 57 D0 3B D5 E8 16 40 F9 A8 83 1F F8 08 7C 40 F9"},
    {SignatureId::BlockDefinitionGroupRegisterBlocks, true, "FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 13 1C 40 F9 D3 00 00 B5 F3 0B 40 F9 FD 7B C2 A8 C0 03 5F D6"},
    {SignatureId::BlockDefinitionGroupDigestServerBlockProperties, true, "FD 7B BA A9 FC 6F 01 A9 FA 67 02 A9 F8 5F 03 A9 F6 57 04 A9 F4 4F 05 A9 FD 03 00 91 FF C3 0E D1 E0 3F 00 F9"},
    {SignatureId::BlockTypeRegistryFinalizeForRendering, true, "FF 03 02 D1 FD 7B 03 A9 FA 67 04 A9 F8 5F 05 A9 F6 57 06 A9 F4 4F 07 A9 FD C3 00 91 58 D0 3B D5 F5 03 00 AA F9 23 00 91"},
    {SignatureId::GeometryGroupLoadModelPackFromStringSync, true, "FF C3 03 D1 FD 7B 0A A9 FA 67 0B A9 F8 5F 0C A9 F6 57 0D A9 F4 4F 0E A9 FD 83 02 91 5A D0 3B D5 F5 03 00 AA 00 10 80 52 48 17 40 F9 F7 03 03 AA"},
    {SignatureId::ReplayPacksInfoCollectKeys, true, "FD 7B BD A9 F5 0B 00 F9 F4 4F 02 A9 FD 03 00 91 F3 03 08 AA 00 E4 00 6F 09 F0 A7 52 08 D4 43 A9"},
    {SignatureId::ReplayResourceLocationCtor, true, "FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 28 24 40 A9 F3 03 00 AA 02 84 00 B8 E1 03 08 AA E2 03 09 AA"},
    {SignatureId::ReplayMinEngineVersionFromString, true, "FF 83 01 D1 FD 7B 02 A9 F7 1B 00 F9 F6 57 04 A9 F4 4F 05 A9 FD 83 00 91 56 D0 3B D5 F3 03 08 AA C8 16 40 F9 A8 83 1F F8 FF 7F 00 A9 FF 0B 00 F9"},
    {SignatureId::ReplayInternedStringDtor, true, "FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 08 18 40 39 F3 03 00 AA C8 00 38 36 ? ? ? ? 68 02 40 F9"},
    {SignatureId::ReplayBlockFinalizeComponentStorage, true, "FF C3 00 D1 FD 7B 01 A9 F4 4F 02 A9 FD 43 00 91 54 D0 3B D5 F3 03 00 AA E0 1F 00 91 88 16 40 F9"},
    {SignatureId::ReplayLegacyBlockMaterialCtor, true, "FF 83 03 D1 E8 3B 00 FD FD 7B 08 A9 FC 6F 09 A9 FA 67 0A A9 F8 5F 0B A9 F6 57 0C A9 F4 4F 0D A9 FD 03 02 91 48 D0 3B D5 08 40 20 1E 00 E4 00 6F"},
    {SignatureId::ReplayBlockCacheComponentData, true, "FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 F3 03 00 AA 00 34 40 F9 E1 03 13 AA"},
    {SignatureId::ReplayActorRenderDispatcherRender, true, "FF 83 03 D1 E8 3B 00 FD FD 7B 08 A9 FC 6F 09 A9 FA 67 0A A9 F8 5F 0B A9 F6 57 0C A9 F4 4F 0D A9 FD 03 02 91 5B D0 3B D5"},
    {SignatureId::ReplayLoadAllPackResources, true, "FD 7B BF A9 FD 03 00 91 09 28 40 F9 22 A9 40 A9 49 01 02 CB"},
    {SignatureId::ReplayLoadedResourceDtor, true, "FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 08 80 42 39 F3 03 00 AA C8 01 00 37 68 C2 41 39 08 02 00 37 68 42 41 39 48 02 00 37 60 02 01 91 B9 D8 37 95"},
}};

}

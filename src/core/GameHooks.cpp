#include "GameHooks.hpp"
#include <bedrocktools/Version.hpp>

#include "core/memory/Hooks.hpp"
#include "replay/ReplayRecorder.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <EGL/egl.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace bedrocktools::core::gamehooks {
namespace {
using namespace bedrocktools::events;
using bedrocktools::memory::SignatureId;

using VersionStringFn = std::string(*)(void*);
using NormalTickFn = void(*)(void*);
using PlayerSwingFn = bool(*)(void*, std::uint8_t, std::uint8_t);
using ClientInstanceUpdateFn = bool(*)(void*, bool, bool);
using EglSwapBuffersFn = EGLBoolean(*)(EGLDisplay, EGLSurface);
using LevelInitializeFn = bool(*)(void*, const std::string*, void*, void*, const std::string*, void*, void*, void*);
using LevelDtorFn = void(*)(void*);
using PacketHandleFn = void(*)(void*, void*, void*, std::shared_ptr<bedrocktools::sdk::Packet>&);

VersionStringFn versionOriginal = nullptr;
NormalTickFn tickOriginal = nullptr;
PlayerSwingFn playerSwingOriginal = nullptr;
ClientInstanceUpdateFn clientUpdateOriginal = nullptr;
EglSwapBuffersFn swapBuffersOriginal = nullptr;
LevelInitializeFn levelInitializeOriginal = nullptr;
LevelDtorFn levelDtorOriginal = nullptr;
PacketHandleFn resourcePacksInfoHandleOriginal = nullptr;
PacketHandleFn resourcePackStackHandleOriginal = nullptr;
std::array<bedrocktools::hooks::Handle, 9> handles{};
std::size_t handleCount = 0;
std::mutex installMutex;
bool installed = false;

template <class Function>
bool hookSignature(SignatureId id, void* detour, Function** original) {
    const auto address = bedrocktools::memory::resolve(id);
    if (!address) return false;
    bedrocktools::hooks::Handle handle = bedrocktools::hooks::install(reinterpret_cast<void*>(address), detour, reinterpret_cast<void**>(original));
    if (!handle) return false;
    if (handleCount < handles.size()) handles[handleCount++] = handle;
    return true;
}

std::string versionDetour(void* self) {
    std::string version = versionOriginal ? versionOriginal(self) : std::string{};
    return std::string("\xC2\xA7" "b") + std::string(bedrocktools::Name) + " v" + std::string(bedrocktools::Version) + " " + "\xC2\xA7" "fby " + "\xC2\xA7" "e" + std::string(bedrocktools::Author) + " " + "\xC2\xA7" "f- " + "\xC2\xA7" "r" + version;
}

void tickDetour(void* actor) {
    auto* player = reinterpret_cast<bedrocktools::sdk::Player*>(actor);
    LocalPlayerPreTickEvent preEvent{player};
    bus().publish(preEvent);
    if (tickOriginal) tickOriginal(actor);
    LocalPlayerTickEvent event{player};
    bus().publish(event);
}

bool playerSwingDetour(void* player, std::uint8_t source, std::uint8_t hand) {
    const bool result = playerSwingOriginal ? playerSwingOriginal(player, source, hand) : false;
    if (result) bedrocktools::replay::ReplayRecorder::get().observeLocalPlayerSwing();
    return result;
}

bool clientUpdateDetour(void* clientInstance, bool value, bool updateSimulation) {
    const bool result = clientUpdateOriginal && clientUpdateOriginal(clientInstance, value, updateSimulation);
    ClientInstanceUpdateEvent event{reinterpret_cast<bedrocktools::sdk::ClientInstance*>(clientInstance)};
    bus().publish(event);
    return result;
}

bool levelInitializeDetour(void* level, const std::string* levelName, void* levelSettings, void* experiments, const std::string* levelId, void* biomeOptLo, void* biomeOptHi, void* initializationContext) {
    const bool initialized = levelInitializeOriginal
        ? levelInitializeOriginal(level, levelName, levelSettings, experiments, levelId, biomeOptLo, biomeOptHi, initializationContext)
        : false;
    if (!initialized) return false;
    LevelStateEvent event{LevelPhase::Initializing, level, levelName, levelSettings, levelId};
    bus().publish(event);
    return true;
}

void levelDtorDetour(void* level) {
    LevelStateEvent event{LevelPhase::Destroyed, level, nullptr, nullptr, nullptr};
    bus().publish(event);
    if (levelDtorOriginal) levelDtorOriginal(level);
}

void resourcePacksInfoHandleDetour(void* dispatcher, void* networkIdentifier, void* networkCallback, std::shared_ptr<bedrocktools::sdk::Packet>& packet) {
    PacketHandleEvent event{PacketHandleKind::ResourcePacksInfo, networkIdentifier, networkCallback, &packet};
    bus().publish(event);
    if (resourcePacksInfoHandleOriginal) resourcePacksInfoHandleOriginal(dispatcher, networkIdentifier, networkCallback, packet);
}

void resourcePackStackHandleDetour(void* dispatcher, void* networkIdentifier, void* networkCallback, std::shared_ptr<bedrocktools::sdk::Packet>& packet) {
    PacketHandleEvent event{PacketHandleKind::ResourcePackStack, networkIdentifier, networkCallback, &packet};
    bus().publish(event);
    if (resourcePackStackHandleOriginal) resourcePackStackHandleOriginal(dispatcher, networkIdentifier, networkCallback, packet);
}

EGLBoolean swapBuffersDetour(EGLDisplay display, EGLSurface surface) {
    static thread_local bool publishingFrame = false;
    if (!publishingFrame && eglGetCurrentContext() != EGL_NO_CONTEXT) {
        publishingFrame = true;
        FrameEvent event;
        bus().publish(event);
        publishingFrame = false;
    }
    return swapBuffersOriginal ? swapBuffersOriginal(display, surface) : EGL_FALSE;
}

bool hookEgl() {
    auto libraryName = "libEGL.so";
    auto symbolName = "eglSwapBuffers";
    auto egl = bedrocktools::hooks::openLibrary(libraryName);
    if (!egl) return false;
    const auto address = bedrocktools::hooks::symbol(egl, symbolName);
    if (!address) {
        bedrocktools::hooks::closeLibrary(egl);
        return false;
    }
    auto handle = bedrocktools::hooks::install(reinterpret_cast<void*>(address), reinterpret_cast<void*>(swapBuffersDetour), reinterpret_cast<void**>(&swapBuffersOriginal));
    bedrocktools::hooks::closeLibrary(egl);
    if (!handle) return false;
    if (handleCount < handles.size()) handles[handleCount++] = handle;
    return true;
}
}

bool install() {
    std::lock_guard lock(installMutex);
    if (installed) return true;
    hookSignature(SignatureId::VersionString, reinterpret_cast<void*>(versionDetour), &versionOriginal);
    hookSignature(SignatureId::LocalPlayerSwing, reinterpret_cast<void*>(playerSwingDetour), &playerSwingOriginal);
    const bool ok = hookSignature(SignatureId::NormalTick, reinterpret_cast<void*>(tickDetour), &tickOriginal)
        && hookSignature(SignatureId::ClientInstanceUpdate, reinterpret_cast<void*>(clientUpdateDetour), &clientUpdateOriginal)
        && hookSignature(SignatureId::LevelInit, reinterpret_cast<void*>(levelInitializeDetour), &levelInitializeOriginal)
        && hookSignature(SignatureId::LevelDtor, reinterpret_cast<void*>(levelDtorDetour), &levelDtorOriginal)
        && hookSignature(SignatureId::ResourcePacksInfoPacketHandle, reinterpret_cast<void*>(resourcePacksInfoHandleDetour), &resourcePacksInfoHandleOriginal)
        && hookSignature(SignatureId::ResourcePackStackPacketHandle, reinterpret_cast<void*>(resourcePackStackHandleDetour), &resourcePackStackHandleOriginal)
        && hookEgl();
    installed = ok;
    return installed;
}

void uninstall() {
    std::lock_guard lock(installMutex);
    for (std::size_t i = 0; i < handleCount; ++i) {
        if (handles[i]) bedrocktools::hooks::remove(handles[i]);
        handles[i] = nullptr;
    }
    handleCount = 0;
    installed = false;
}


}

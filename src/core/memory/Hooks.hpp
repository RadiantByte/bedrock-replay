#pragma once

#include "api/memory/android/Gloss.h"

#include <cstdint>
#include <dlfcn.h>

namespace pl { namespace memory { enum class HookPriority : int; } }

namespace bedrocktools::hooks {

using LibraryHandle = void*;

struct State {
    void* target;
    void* detour;
    GHook handle;
    bool (*sharedUnhook)(void*, void*){};
    LibraryHandle sharedLibrary{};
};

using Handle = State*;

inline LibraryHandle openLibrary(const char* libraryName) {
    if (!libraryName) return nullptr;
    void* handle = dlopen(libraryName, RTLD_NOW | RTLD_NOLOAD);
    return handle ? handle : dlopen(libraryName, RTLD_NOW);
}

inline int closeLibrary(LibraryHandle handle) {
    return handle ? dlclose(handle) : 0;
}

inline std::uintptr_t symbol(LibraryHandle handle, const char* name) {
    return handle && name ? reinterpret_cast<std::uintptr_t>(dlsym(handle, name)) : 0;
}

inline Handle install(void* target, void* detour, void** original) {
    if (!target || !detour || !original) return nullptr;
    GlossSetLog(false);
    GHook handle = GlossHook(target, detour, original);
    if (!handle || !*original) {
        if (handle) GlossHookDelete(handle);
        return nullptr;
    }
    return new State{target, detour, handle};
}

inline Handle installShared(void* target, void* detour, void** original) {
    if (!target || !detour || !original) return nullptr;
    auto libraryName = "libpreloader.so";
    auto library = dlopen(libraryName, RTLD_NOW | RTLD_NOLOAD);
    if (!library) return nullptr;
    auto hookName = "_ZN2pl6memory4hookEPvS1_PS1_NS0_12HookPriorityE";
    auto unhookName = "_ZN2pl6memory6unhookEPvS1_";
    using SharedInstall = int (*)(void*, void*, void**, pl::memory::HookPriority);
    auto installFn = reinterpret_cast<SharedInstall>(dlsym(library, hookName));
    auto removeFn = reinterpret_cast<bool (*)(void*, void*)>(dlsym(library, unhookName));
    if (!installFn || !removeFn) {
        dlclose(library);
        return nullptr;
    }

    auto* state = new State{target, detour, nullptr, removeFn, library};
    const bool installed = installFn(target, detour, original,
        static_cast<pl::memory::HookPriority>(200)) == 0;
    if (!installed || !*original) {
        if (installed) removeFn(target, detour);
        dlclose(library);
        delete state;
        return nullptr;
    }
    return state;
}

inline void remove(Handle hook) {
    if (!hook) return;
    if (hook->sharedUnhook) hook->sharedUnhook(hook->target, hook->detour);
    else if (hook->handle) GlossHookDelete(hook->handle);
    if (hook->sharedLibrary) dlclose(hook->sharedLibrary);
    delete hook;
}

}

#include "Runtime.hpp"
#include "GameHooks.hpp"
#include "core/memory/Hooks.hpp"
#include "replay/ReplayRuntime.hpp"
#include "replay/ReplayRecorder.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <mutex>

namespace bedrocktools::core {
namespace {
std::atomic_bool enabled = false;
std::atomic_bool resolved = false;
std::atomic_bool installed = false;
std::mutex resolveMutex;
std::mutex installMutex;
thread_local bool resolvingFromDlopen = false;
void* (*dlopenOriginal)(const char*, int) = nullptr;
bedrocktools::hooks::Handle dlopenHook = nullptr;

class ResolveGuard {
public:
    ResolveGuard() : mPrevious(resolvingFromDlopen) { resolvingFromDlopen = true; }
    ~ResolveGuard() { resolvingFromDlopen = mPrevious; }
private:
    bool mPrevious;
};

void* dlopenDetour(const char* filename, int flags) {
    void* handle = dlopenOriginal ? dlopenOriginal(filename, flags) : nullptr;
    if (handle && filename && !resolvingFromDlopen) {
        auto minecraftName = "libminecraftpe.so";
        if (std::strstr(filename, minecraftName)) Runtime::get().minecraftLoaded();
    }
    return handle;
}
}

Runtime& Runtime::get() {
    static Runtime runtime;
    return runtime;
}

const std::filesystem::path& Runtime::resourceDirectory() const noexcept {
    return mResourceDirectory;
}

const std::filesystem::path& Runtime::configDirectory() const noexcept {
    return mConfigDirectory;
}

bool Runtime::resolveSignatures() {
    std::lock_guard lock(resolveMutex);
    if (resolved.load(std::memory_order_acquire)) return true;
    ResolveGuard guard;
    const bool ok = bedrocktools::memory::resolveAll({});
    resolved.store(ok, std::memory_order_release);
    return ok;
}

bool Runtime::install() {
    std::lock_guard lock(installMutex);
    if (installed.load(std::memory_order_acquire)) return true;
    if (!resolved.load(std::memory_order_acquire) && !resolveSignatures()) return false;
    if (!gamehooks::install()) return false;
    if (!replay::ReplayRuntime::get().install()) {
        gamehooks::uninstall();
        return false;
    }
    installed.store(true, std::memory_order_release);
    return true;
}

void Runtime::minecraftLoaded() {
    if (!resolveSignatures()) return;
    if (enabled.load(std::memory_order_acquire)) install();
}

bool Runtime::load(JavaVM* vm, const PLModInfo* info) {
    if (!vm || !info || info->size < sizeof(PLModInfo) || !info->mod_root_path) return false;
    const std::filesystem::path root(info->mod_root_path);
    mResourceDirectory = root / "resources";
    mConfigDirectory = root / "config";
    std::error_code error;
    std::filesystem::create_directories(mConfigDirectory, error);
    replay::ReplayRecorder::get().setStorageDirectory(mConfigDirectory);
    GlossSetLog(false);
    GlossInit(false);
    auto minecraftName = "libminecraftpe.so";
    void* minecraft = dlopen(minecraftName, RTLD_NOW | RTLD_NOLOAD);
    if (minecraft) {
        resolveSignatures();
        dlclose(minecraft);
        return true;
    }
    auto dlName = "libdl.so";
    auto openName = "dlopen";
    bedrocktools::hooks::LibraryHandle libdl = bedrocktools::hooks::openLibrary(dlName);
    if (!libdl) return true;
    void* symbol = reinterpret_cast<void*>(bedrocktools::hooks::symbol(libdl, openName));
    if (symbol) dlopenHook = bedrocktools::hooks::install(symbol, reinterpret_cast<void*>(dlopenDetour), reinterpret_cast<void**>(&dlopenOriginal));
    bedrocktools::hooks::closeLibrary(libdl);
    return true;
}

bool Runtime::enable() {
    enabled.store(true, std::memory_order_release);
    replay::ReplayRuntime::get().setEnabled(true);
    if (!resolved.load(std::memory_order_acquire)) {
        auto name = "libminecraftpe.so";
        void* minecraft = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
        if (!minecraft) return true;
        const bool ok = resolveSignatures();
        dlclose(minecraft);
        if (!ok) return false;
    }
    return install();
}

bool Runtime::disable() {
    enabled.store(false, std::memory_order_release);
    replay::ReplayRuntime::get().setEnabled(false);
    return true;
}

bool Runtime::unload() {
    enabled.store(false, std::memory_order_release);
    replay::ReplayRuntime::get().setEnabled(false);
    replay::ReplayRuntime::get().uninstall();
    gamehooks::uninstall();
    installed.store(false, std::memory_order_release);
    if (dlopenHook) {
        bedrocktools::hooks::remove(dlopenHook);
        dlopenHook = nullptr;
        dlopenOriginal = nullptr;
    }
    bedrocktools::memory::clear();
    resolved.store(false, std::memory_order_release);
    return true;
}

}

#include "InputBridge.hpp"

#include <atomic>
#include <cstddef>
#include <dlfcn.h>
#include <mutex>
#include <utility>

namespace bedrockreplay::input {
namespace {

using LegacyTouchCallback = bool (*)(int, int, float, float);
using LegacyKeyCallback = bool (*)(int, unsigned int, bool);
using LegacyTextCallback = bool (*)(const char*, std::size_t);
using LegacyMouseCallback = bool (*)(int, bool);

struct PreloaderInputInterface {
    void (*RegisterTouchCallback)(LegacyTouchCallback);
    void (*RegisterKeyEventCallback)(LegacyKeyCallback);
    void (*ShowKeyboard)();
    void (*HideKeyboard)();
    void (*RegisterMouseCallback)(LegacyMouseCallback);
    void (*RegisterTextInputCallback)(LegacyTextCallback);
};

using GetPreloaderInputFn = PreloaderInputInterface* (*)();
using RuntimeFlagFn = unsigned char (*)(void*, void*);

std::mutex callbackMutex;
pl::input::TouchCallback touchCallback;
pl::input::KeyCallback keyCallback;
pl::input::TextInputCallback textCallback;
pl::input::MouseCallback mouseCallback;
bool touchRegistered = false;
bool keyRegistered = false;
bool textRegistered = false;
bool mouseRegistered = false;
std::atomic<RuntimeFlagFn> hudFlag{nullptr};
std::atomic<RuntimeFlagFn> menuFlag{nullptr};
std::atomic<RuntimeFlagFn> pauseFlag{nullptr};

void* preloaderHandle() {
    static std::mutex handleMutex;
    static void* handle = nullptr;
    if (handle) return handle;
    std::scoped_lock lock(handleMutex);
    if (handle) return handle;
    auto library = "libpreloader.so";
    handle = dlopen(library, RTLD_NOW | RTLD_NOLOAD);
    if (!handle) handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    return handle;
}

void* preloaderSymbol(const char* name) {
    void* symbol = nullptr;
    if (void* handle = preloaderHandle()) symbol = dlsym(handle, name);
    if (!symbol) symbol = dlsym(RTLD_DEFAULT, name);
    return symbol;
}

std::optional<bool> runtimeFlag(std::atomic<RuntimeFlagFn>& cache, const char* name) {
    auto function = cache.load(std::memory_order_acquire);
    if (!function) {
        function = reinterpret_cast<RuntimeFlagFn>(preloaderSymbol(name));
        if (!function) return std::nullopt;
        cache.store(function, std::memory_order_release);
    }
    return function(nullptr, nullptr) != 0;
}

PreloaderInputInterface* inputInterface() {
    static std::mutex resolveMutex;
    static PreloaderInputInterface* cached = nullptr;
    if (cached) return cached;
    std::scoped_lock lock(resolveMutex);
    if (cached) return cached;
    auto symbolName = "GetPreloaderInput";
    void* symbol = preloaderSymbol(symbolName);
    if (!symbol) return nullptr;
    const auto getter = reinterpret_cast<GetPreloaderInputFn>(symbol);
    cached = getter ? getter() : nullptr;
    return cached;
}

bool touchThunk(int action, int pointerId, float x, float y) {
    pl::input::TouchCallback callback;
    {
        std::scoped_lock lock(callbackMutex);
        callback = touchCallback;
    }
    if (!callback) return false;

    if (action == 5) action = 0;
    else if (action == 6) action = 1;
    return callback(pl::input::TouchEvent{action, pointerId, x, y});
}

bool keyThunk(int keyCode, unsigned int unicodeChar, bool isKeyDown) {
    pl::input::KeyCallback callback;
    {
        std::scoped_lock lock(callbackMutex);
        callback = keyCallback;
    }
    if (!callback) return false;
    return callback(pl::input::KeyEvent{keyCode, unicodeChar, isKeyDown});
}

bool textThunk(const char* text, std::size_t length) {
    pl::input::TextInputCallback callback;
    {
        std::scoped_lock lock(callbackMutex);
        callback = textCallback;
    }
    if (!callback) return false;
    pl::input::TextInputEvent event;
    if (text && length) event.text.assign(text, length);
    return callback(event);
}

bool mouseThunk(int button, bool isDown) {
    pl::input::MouseCallback callback;
    {
        std::scoped_lock lock(callbackMutex);
        callback = mouseCallback;
    }
    if (!callback) return false;
    return callback(pl::input::MouseEvent{button, isDown});
}

}

bool registerTouchCallback(pl::input::TouchCallback callback) {
    auto* input = inputInterface();
    if (!input || !input->RegisterTouchCallback) return false;
    bool registerNow = false;
    {
        std::scoped_lock lock(callbackMutex);
        touchCallback = std::move(callback);
        if (!touchRegistered) {
            touchRegistered = true;
            registerNow = true;
        }
    }
    if (registerNow) input->RegisterTouchCallback(touchThunk);
    return true;
}

bool registerKeyCallback(pl::input::KeyCallback callback) {
    auto* input = inputInterface();
    if (!input || !input->RegisterKeyEventCallback) return false;
    bool registerNow = false;
    {
        std::scoped_lock lock(callbackMutex);
        keyCallback = std::move(callback);
        if (!keyRegistered) {
            keyRegistered = true;
            registerNow = true;
        }
    }
    if (registerNow) input->RegisterKeyEventCallback(keyThunk);
    return true;
}

bool registerTextInputCallback(pl::input::TextInputCallback callback) {
    auto* input = inputInterface();
    if (!input || !input->RegisterTextInputCallback) return false;
    bool registerNow = false;
    {
        std::scoped_lock lock(callbackMutex);
        textCallback = std::move(callback);
        if (!textRegistered) {
            textRegistered = true;
            registerNow = true;
        }
    }
    if (registerNow) input->RegisterTextInputCallback(textThunk);
    return true;
}

bool registerMouseCallback(pl::input::MouseCallback callback) {
    auto* input = inputInterface();
    if (!input || !input->RegisterMouseCallback) return false;
    bool registerNow = false;
    {
        std::scoped_lock lock(callbackMutex);
        mouseCallback = std::move(callback);
        if (!mouseRegistered) {
            mouseRegistered = true;
            registerNow = true;
        }
    }
    if (registerNow) input->RegisterMouseCallback(mouseThunk);
    return true;
}

bool showKeyboard() {
    auto* input = inputInterface();
    if (!input || !input->ShowKeyboard) return false;
    input->ShowKeyboard();
    return true;
}

bool hideKeyboard() {
    auto* input = inputInterface();
    if (!input || !input->HideKeyboard) return false;
    input->HideKeyboard();
    return true;
}

std::optional<bool> hudScreenOpen() {
    return runtimeFlag(hudFlag,
        "Java_org_levimc_launcher_preloader_PreloaderInput_nativeIsHudScreenOpen");
}

std::optional<bool> showingMenu() {
    return runtimeFlag(menuFlag,
        "Java_org_levimc_launcher_preloader_PreloaderInput_nativeIsShowingMenu");
}

std::optional<bool> pauseMenuOpen() {
    return runtimeFlag(pauseFlag,
        "Java_org_levimc_launcher_preloader_PreloaderInput_nativeIsPauseMenuOpen");
}

}

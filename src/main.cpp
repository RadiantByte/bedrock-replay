#include "core/Runtime.hpp"
#include "runtime/Lifecycle.hpp"

#if defined(__clang__) || defined(__GNUC__)
#define MOD_EXPORT __attribute__((visibility("default")))
#else
#define MOD_EXPORT
#endif

extern "C" MOD_EXPORT bool PLMod_Load(JavaVM* vm, const PLModInfo* info) {
    return bedrocktools::core::Runtime::get().load(vm, info);
}

extern "C" MOD_EXPORT bool PLMod_Enable() {
    return bedrocktools::core::Runtime::get().enable();
}

extern "C" MOD_EXPORT bool PLMod_Disable() {
    return bedrocktools::core::Runtime::get().disable();
}

extern "C" MOD_EXPORT bool PLMod_Unload() {
    return bedrocktools::core::Runtime::get().unload();
}

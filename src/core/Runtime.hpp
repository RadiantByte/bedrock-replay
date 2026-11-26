#pragma once

#include "runtime/Lifecycle.hpp"

#include <filesystem>

namespace bedrocktools::core {

class Runtime {
public:
    static Runtime& get();
    bool load(JavaVM* vm, const PLModInfo* info);
    bool enable();
    bool disable();
    bool unload();
    void minecraftLoaded();
    const std::filesystem::path& resourceDirectory() const noexcept;
    const std::filesystem::path& configDirectory() const noexcept;

private:
    bool resolveSignatures();
    bool install();
    std::filesystem::path mResourceDirectory;
    std::filesystem::path mConfigDirectory;
};

}

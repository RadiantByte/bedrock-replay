#include "Rtti.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <link.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bedrocktools::core::rtti {
namespace {

struct Segment {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    std::uint32_t flags{};
};

struct Image {
    std::vector<Segment> segments;
};

Image loadImage() {
    Image image;
    dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* opaque) {
        if (!info || !opaque) return 0;
        const char* name = info->dlpi_name;
        if (!name) return 0;
        std::string_view path(name);
        const auto position = path.find_last_of('/');
        const auto base = position == std::string_view::npos ? path : path.substr(position + 1U);
        if (base != "libminecraftpe.so") return 0;
        auto* image = static_cast<Image*>(opaque);
        for (std::size_t i = 0; i < info->dlpi_phnum; ++i) {
            const auto& phdr = info->dlpi_phdr[i];
            if (phdr.p_type != PT_LOAD || (phdr.p_flags & PF_R) == 0 || phdr.p_memsz == 0) continue;
            const auto begin = static_cast<std::uintptr_t>(info->dlpi_addr + phdr.p_vaddr);
            image->segments.push_back({begin, begin + phdr.p_memsz, static_cast<std::uint32_t>(phdr.p_flags)});
        }
        return 1;
    }, &image);
    return image;
}

const Image& image() {
    static const Image instance = loadImage();
    return instance;
}

bool inExecutable(std::uintptr_t address) {
    for (const auto& segment : image().segments) {
        if ((segment.flags & PF_X) != 0 && address >= segment.begin && address < segment.end) return true;
    }
    return false;
}

std::uintptr_t findName(std::string_view name) {
    std::string needle(name);
    needle.push_back('\0');
    const auto* first = reinterpret_cast<const unsigned char*>(needle.data());
    const auto* last = first + needle.size();
    for (const auto& segment : image().segments) {
        const auto* begin = reinterpret_cast<const unsigned char*>(segment.begin);
        const auto* end = reinterpret_cast<const unsigned char*>(segment.end);
        const auto* found = std::search(begin, end, first, last);
        if (found != end) return reinterpret_cast<std::uintptr_t>(found);
    }
    return 0;
}

std::vector<std::uintptr_t> findPointers(std::uintptr_t value) {
    std::vector<std::uintptr_t> result;
    for (const auto& segment : image().segments) {
        if ((segment.flags & PF_X) != 0) continue;
        auto begin = (segment.begin + alignof(std::uintptr_t) - 1) & ~(alignof(std::uintptr_t) - 1);
        for (auto address = begin; address + sizeof(std::uintptr_t) <= segment.end; address += sizeof(std::uintptr_t)) {
            std::uintptr_t candidate{};
            std::memcpy(&candidate, reinterpret_cast<const void*>(address), sizeof(candidate));
            if (candidate == value) result.push_back(address);
        }
    }
    return result;
}

void** resolve(std::string_view typeName) {
    const auto name = findName(typeName);
    if (!name) return nullptr;
    for (const auto nameRef : findPointers(name)) {
        if (nameRef < sizeof(std::uintptr_t)) continue;
        const auto typeInfo = nameRef - sizeof(std::uintptr_t);
        for (const auto typeInfoRef : findPointers(typeInfo)) {
            if (typeInfoRef < sizeof(std::intptr_t)) continue;
            std::intptr_t offsetToTop{};
            std::memcpy(&offsetToTop, reinterpret_cast<const void*>(typeInfoRef - sizeof(std::intptr_t)), sizeof(offsetToTop));
            if (offsetToTop != 0) continue;
            auto** addressPoint = reinterpret_cast<void**>(typeInfoRef + sizeof(std::uintptr_t));
            const auto first = reinterpret_cast<std::uintptr_t>(addressPoint[0]);
            const auto second = reinterpret_cast<std::uintptr_t>(addressPoint[1]);
            if ((inExecutable(first) || first == 0) && (inExecutable(second) || second == 0)) return addressPoint;
        }
    }
    return nullptr;
}

std::mutex cacheMutex;
std::unordered_map<std::string, void**> cache;

}

void** vtable(std::string_view typeName) {
    std::lock_guard lock(cacheMutex);
    const std::string key(typeName);
    const auto found = cache.find(key);
    if (found != cache.end()) return found->second;
    auto** result = resolve(typeName);
    cache.emplace(key, result);
    return result;
}

void* virtualFunction(std::string_view typeName, std::size_t slot) {
    auto** table = vtable(typeName);
    return table ? table[slot] : nullptr;
}

bool isInstance(const void* object, std::string_view typeName) {
    if (!object) return false;
    auto** expected = vtable(typeName);
    if (!expected) return false;
    return *reinterpret_cast<void***>(const_cast<void*>(object)) == expected;
}

}

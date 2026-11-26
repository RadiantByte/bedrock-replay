#include "ReplayContent.hpp"

#include "NativePacket.hpp"
#include "ReplayRecorder.hpp"
#include "ReplayRuntime.hpp"
#include "ReplayWorldLauncher.hpp"
#include "core/memory/Hooks.hpp"
#include "core/memory/Rtti.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/offsets/Content.hpp>
#include <bedrocktools/sdk/network/MinecraftPacketIds.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace bedrocktools::replay::content {
namespace {
namespace layout = bedrocktools::sdk::offsets::ReplayContent;
using bedrocktools::memory::SignatureId;
using bedrocktools::sdk::MinecraftPacketIds;
using bedrocktools::sdk::Packet;
using bedrocktools::sdk::field;
using LoadDefinitionsFn = void(*)(void*, const void*);
using RegisterFn = void(*)(void*);
using RenderCallback = void(*)(void*, void*);
using RenderFinalizeFn = void(*)(void*, RenderCallback, void*);
using DispatchFn = void(*)(void*, void*, void*, std::shared_ptr<Packet>&);

struct NativeVector {
    std::byte* begin{};
    std::byte* end{};
    std::byte* capacity{};
};

struct ContentIdentity {
    std::array<std::uint64_t, 2> uuid{};
    bool valid{};
    std::array<std::byte, 7> padding{};
    bool operator==(const ContentIdentity& other) const { return uuid == other.uuid; }
};

struct ContentHash {
    std::size_t operator()(const ContentIdentity& value) const noexcept {
        return value.uuid[0] ^ (value.uuid[1] * 0x1F1F1F1FULL);
    }
};

using ContentKeys = std::unordered_map<ContentIdentity, std::string, ContentHash>;

struct ResourceLocation {
    std::uint32_t fileSystem{};
    std::uint32_t padding{};
    std::string path;
    std::uint64_t pathHash{};
    std::uint64_t fullHash{};
};

struct alignas(8) LoadedResource {
    std::array<std::byte, layout::LoadedResourceSize> bytes;
};

struct LoadedResources {
    std::vector<LoadedResource> values;
    void (*destroy)(void*);
    ~LoadedResources() {
        for (auto& resource : values) destroy(&resource);
    }
};

struct GeometryReference {
    std::shared_ptr<bool> alive;
    void* object{};
};

struct MinEngineVersion {
    std::array<std::byte, layout::MinEngineVersionSize> bytes{};
};

struct Visuals {
    std::shared_ptr<void> geometry;
    std::shared_ptr<void> material;
};

struct JsonNode {
    bool string{};
    std::string text;
    std::unordered_map<std::string, JsonNode> members;
    const JsonNode* get(std::string_view name) const {
        const auto at = members.find(std::string(name));
        return at == members.end() ? nullptr : &at->second;
    }
};

class JsonReader {
    std::string_view input;
    std::size_t offset{};
    std::size_t budget{500000};

    void space() {
        while (offset < input.size() && std::isspace(static_cast<unsigned char>(input[offset]))) ++offset;
    }

    char take() {
        if (offset >= input.size()) throw std::runtime_error("Incomplete blocks.json");
        return input[offset++];
    }

    std::uint32_t hex() {
        std::uint32_t value{};
        for (int i = 0; i < 4; ++i) {
            const auto c = take();
            const int digit = c >= '0' && c <= '9' ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (digit < 0) throw std::runtime_error("Invalid blocks.json escape");
            value = value * 16 + digit;
        }
        return value;
    }

    static void utf8(std::string& text, std::uint32_t value) {
        if (value < 0x80) text.push_back(static_cast<char>(value));
        else if (value < 0x800) {
            text.push_back(static_cast<char>(0xC0 | (value >> 6)));
            text.push_back(static_cast<char>(0x80 | (value & 63)));
        } else if (value < 0x10000) {
            text.push_back(static_cast<char>(0xE0 | (value >> 12)));
            text.push_back(static_cast<char>(0x80 | ((value >> 6) & 63)));
            text.push_back(static_cast<char>(0x80 | (value & 63)));
        } else {
            text.push_back(static_cast<char>(0xF0 | (value >> 18)));
            text.push_back(static_cast<char>(0x80 | ((value >> 12) & 63)));
            text.push_back(static_cast<char>(0x80 | ((value >> 6) & 63)));
            text.push_back(static_cast<char>(0x80 | (value & 63)));
        }
    }

    std::string string() {
        if (take() != '"') throw std::runtime_error("Invalid blocks.json string");
        std::string value;
        for (;;) {
            const auto c = take();
            if (c == '"') return value;
            if (static_cast<unsigned char>(c) < 32) throw std::runtime_error("Invalid blocks.json string");
            if (c != '\\') { value.push_back(c); continue; }
            switch (take()) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case 'u': {
                auto code = hex();
                if (code >= 0xD800 && code <= 0xDBFF) {
                    if (take() != '\\' || take() != 'u') throw std::runtime_error("Invalid blocks.json Unicode");
                    const auto low = hex();
                    if (low < 0xDC00 || low > 0xDFFF) throw std::runtime_error("Invalid blocks.json Unicode");
                    code = 0x10000 + ((code - 0xD800) << 10) + low - 0xDC00;
                } else if (code >= 0xDC00 && code <= 0xDFFF) {
                    throw std::runtime_error("Invalid blocks.json Unicode");
                }
                utf8(value, code);
                break;
            }
            default: throw std::runtime_error("Invalid blocks.json escape");
            }
        }
    }

    JsonNode value(unsigned depth) {
        if (depth > 64 || budget-- == 0) throw std::runtime_error("blocks.json is too complex");
        space();
        if (offset >= input.size()) throw std::runtime_error("Incomplete blocks.json");
        JsonNode result;
        if (input[offset] == '"') { result.string = true; result.text = string(); return result; }
        if (input[offset] == '{' || input[offset] == '[') {
            const bool object = take() == '{';
            const char end = object ? '}' : ']';
            space();
            if (offset < input.size() && input[offset] == end) { ++offset; return result; }
            for (;;) {
                space();
                std::string name;
                if (object) {
                    name = string();
                    space();
                    if (take() != ':') throw std::runtime_error("Invalid blocks.json object");
                }
                auto child = value(depth + 1);
                if (object) result.members.insert_or_assign(std::move(name), std::move(child));
                space();
                const auto separator = take();
                if (separator == end) return result;
                if (separator != ',') throw std::runtime_error("Invalid blocks.json separator");
            }
        }
        const auto begin = offset;
        while (offset < input.size() && input[offset] != ',' && input[offset] != '}'
            && input[offset] != ']' && !std::isspace(static_cast<unsigned char>(input[offset]))) ++offset;
        if (begin == offset) throw std::runtime_error("Invalid blocks.json value");
        return result;
    }

public:
    explicit JsonReader(std::string_view text) : input(text) {}
    JsonNode read() {
        if (input.size() > 8 * 1024 * 1024) throw std::runtime_error("blocks.json is too large");
        auto root = value(0);
        space();
        if (offset != input.size()) throw std::runtime_error("Invalid blocks.json tail");
        return root;
    }
};

static_assert(sizeof(std::string) == 24);
static_assert(sizeof(ContentIdentity) == 24);
static_assert(sizeof(ContentKeys) == 40);
static_assert(sizeof(ResourceLocation) == layout::ResourceLocationSize);
static_assert(sizeof(LoadedResource) == layout::LoadedResourceSize);
static_assert(sizeof(GeometryReference) == 24);
static_assert(sizeof(NativeVector) == 24);
static_assert(sizeof(std::shared_ptr<void>) == 16);
static_assert(layout::PaletteEnd == layout::PaletteBegin + sizeof(void*));

std::recursive_mutex mutex;
std::shared_ptr<Packet> recordedStartGame;
std::shared_ptr<Packet> recordedInfo;
std::shared_ptr<Packet> recordedStack;
std::unordered_set<void*> registeredGroups;
std::unordered_set<std::string> blockNames;
std::unordered_set<std::string> geometryPaths;
std::unordered_map<std::string, Visuals> descriptions;
std::unordered_map<std::string, std::string> legacyTextures;
std::string errorMessage;
void* cachedRepository{};
std::unordered_set<void*> pendingRepositories;
bool renderingPrepared{};
bool prepared{};
thread_local void* loadingLevel{};
LoadDefinitionsFn loadOriginal{};
RegisterFn registerOriginal{};
RenderFinalizeFn renderOriginal{};
DispatchFn startGameOriginal{};
std::array<bedrocktools::hooks::Handle, 4> handles{};
void** indexedPaletteTable{};
void** hashedPaletteTable{};

void rememberFailure(std::string_view message) {
    if (errorMessage.empty()) errorMessage = "Replay custom content: " + std::string(message);
}

template<class Function>
Function bind(SignatureId id) {
    return reinterpret_cast<Function>(bedrocktools::memory::resolve(id));
}

template<class Function>
Function method(void* object, std::size_t slot) {
    if (!object) return nullptr;
    auto** table = *reinterpret_cast<void***>(object);
    return table ? reinterpret_cast<Function>(table[slot]) : nullptr;
}

template<class Function>
Function requireMethod(void* object, std::size_t slot) {
    const auto target = method<Function>(object, slot);
    if (!target) throw std::runtime_error("Required native content method is unavailable");
    return target;
}

std::size_t vectorSize(const NativeVector& value, std::size_t stride, std::size_t maximum) {
    const auto begin = reinterpret_cast<std::uintptr_t>(value.begin);
    const auto end = reinterpret_cast<std::uintptr_t>(value.end);
    const auto capacity = reinterpret_cast<std::uintptr_t>(value.capacity);
    if ((!begin && (end || capacity)) || end < begin || capacity < end || (end - begin) % stride
        || (end - begin) / stride > maximum) throw std::runtime_error("Invalid native content vector");
    return (end - begin) / stride;
}

std::optional<std::size_t> itemDescriptorOffset(MinecraftPacketIds id) {
    switch (id) {
    case MinecraftPacketIds::PlayerEquipment: return layout::EquipmentItemDescriptor;
    case MinecraftPacketIds::AddPlayer: return layout::AddPlayerItemDescriptor;
    case MinecraftPacketIds::AddItemActor: return layout::AddItemActorItemDescriptor;
    default: return std::nullopt;
    }
}

bool hasBlockRuntimeIds(MinecraftPacketIds id) {
    return id == MinecraftPacketIds::UpdateBlock || id == MinecraftPacketIds::UpdateBlockSynced
        || id == MinecraftPacketIds::UpdateSubChunkBlocks || itemDescriptorOffset(id).has_value();
}

std::optional<std::uint32_t> paletteHashIndex(void* palette, std::uint32_t hash, std::size_t count) {
    auto* node = field<void*>(palette, layout::PaletteHashRoot);
    for (std::size_t visited = 0; node && visited < 64; ++visited) {
        const auto key = field<std::uint32_t>(node, layout::PaletteHashNodeKey);
        if (hash == key) {
            const auto index = field<std::uint64_t>(node, layout::PaletteHashNodeIndex);
            if (index < count) return static_cast<std::uint32_t>(index);
            return std::nullopt;
        }
        node = field<void*>(node, hash < key ? 0 : sizeof(void*));
    }
    return std::nullopt;
}

const void* blockFromHash(void* level, std::uint32_t hash) {
    auto* palette = requireMethod<void*(*)(void*)>(level, layout::LevelGlobalPaletteSlot)(level);
    if (!palette || *reinterpret_cast<void***>(palette) != hashedPaletteTable) return nullptr;
    const auto& blocks = field<NativeVector>(palette, layout::PaletteBegin);
    const auto count = vectorSize(blocks, sizeof(void*), 4194304);
    const auto index = paletteHashIndex(palette, hash, count);
    if (!index) return nullptr;
    auto* block = *reinterpret_cast<void**>(blocks.begin + *index * sizeof(void*));
    if (!block || field<std::uint32_t>(block, layout::BlockNetworkHash) != hash) return nullptr;
    return block;
}

template <typename Convert>
bool convertBlockRuntimeIds(Packet& packet, MinecraftPacketIds id, const Convert& convert) {
    if (const auto offset = itemDescriptorOffset(id)) {
        auto& runtimeId = field<std::uint32_t>(packet.payload(), *offset + layout::ItemDescriptorBlockRuntimeId);
        return runtimeId == 0 || convert(runtimeId);
    }
    if (id == MinecraftPacketIds::UpdateSubChunkBlocks) {
        for (const auto offset : {std::size_t{0}, layout::SubChunkChangesExtra}) {
            auto& changes = field<NativeVector>(packet.payload(), offset);
            const auto size = vectorSize(changes, layout::SubChunkChangeSize, 1048576);
            for (std::size_t i = 0; i < size; ++i) {
                if (!convert(field<std::uint32_t>(changes.begin + i * layout::SubChunkChangeSize,
                        layout::SubChunkChangeRuntimeId))) return false;
            }
        }
        return true;
    }
    const auto offset = id == MinecraftPacketIds::UpdateBlock
        ? layout::UpdateBlockRuntimeId : layout::UpdateBlockSyncedRuntimeId;
    return convert(field<std::uint32_t>(packet.payload(), offset));
}

void collectGeometry(void* compound, unsigned depth, std::size_t& budget) {
    if (depth > 64) throw std::runtime_error("Block properties are too deeply nested");
    auto* node = field<void*>(compound, layout::CompoundBeginNode);
    auto* end = static_cast<std::byte*>(compound) + layout::CompoundEndNode;
    while (node != end) {
        if (!node || budget-- == 0) throw std::runtime_error("Invalid block property tree");
        auto* tag = static_cast<std::byte*>(node) + layout::CompoundNodeTag;
        const auto kind = field<std::int32_t>(node, layout::CompoundNodeVariant);
        if (kind == 8) {
            const auto& name = field<std::string>(tag, layout::StringTagValue);
            const auto marker = name.find(".block.");
            if (name.starts_with("geometry.") && marker != std::string::npos && marker > 9
                && marker + 7 < name.size() && name.size() <= 512
                && std::all_of(name.begin(), name.end(), [](unsigned char c) {
                    return std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == ':';
                })) {
                auto path = "models/blocks/" + name.substr(9, marker - 9) + '/' + name.substr(marker + 7);
                std::replace(path.begin(), path.end(), '.', '_');
                geometryPaths.emplace(path + ".geometry.json");
            }
        } else if (kind == 10) {
            collectGeometry(tag, depth + 1, budget);
        }
        if (auto* right = field<void*>(node, 8)) {
            node = right;
            while (auto* left = field<void*>(node, 0)) node = left;
        } else {
            auto* parent = field<void*>(node, 16);
            while (parent != end && field<void*>(parent, 0) != node) {
                node = parent;
                parent = field<void*>(node, 16);
            }
            node = parent;
        }
    }
}

void rememberDescriptions(void* group) {
    auto* node = field<void*>(group, layout::DefinitionFirstNode);
    std::size_t budget = 200000;
    while (node) {
        if (budget-- == 0) throw std::runtime_error("Invalid block definition list");
        if (auto* definition = field<void*>(node, layout::DefinitionNodeValue)) {
            const auto& name = field<std::string>(definition, layout::DefinitionName);
            if (blockNames.contains(name)) {
                auto& visual = descriptions[name];
                const auto& list = field<NativeVector>(definition, layout::DefinitionDescriptions);
                const auto count = vectorSize(list, sizeof(std::shared_ptr<void>), 4096);
                for (std::size_t i = 0; i < count; ++i) {
                    const auto& description = *reinterpret_cast<const std::shared_ptr<void>*>(list.begin + i * 16);
                    if (!description) continue;
                    const auto getName = method<const std::string&(*)(const void*)>(description.get(), layout::DescriptionNameSlot);
                    if (!getName) continue;
                    const auto& componentName = getName(description.get());
                    if (componentName == "minecraft:geometry") visual.geometry = description;
                    if (componentName == "minecraft:material_instances") visual.material = description;
                }
            }
        }
        node = field<void*>(node, 0);
    }
}

void registerDetour(void* group) {
    std::scoped_lock lock(mutex);
    if (prepared && recordedStartGame && loadingLevel && group && !registeredGroups.contains(group)
        && errorMessage.empty()) {
        try {
            auto& properties = field<NativeVector>(recordedStartGame.get(), layout::StartGameBlockProperties);
            const auto digest = bind<void(*)(void*, const void*)>(SignatureId::BlockDefinitionGroupDigestServerBlockProperties);
            digest(group, &properties);
            registeredGroups.emplace(group);
        } catch (const std::exception& error) { rememberFailure(error.what()); }
        catch (...) { rememberFailure("Block definition import failed"); }
    }
    if (registerOriginal) registerOriginal(group);
    if (registeredGroups.contains(group) && errorMessage.empty()) {
        try { rememberDescriptions(group); }
        catch (const std::exception& error) { rememberFailure(error.what()); }
        catch (...) { rememberFailure("Block description import failed"); }
    }
}

void loadDetour(void* level, const void* experiments) {
    const auto previous = loadingLevel;
    loadingLevel = level;
    try {
        if (loadOriginal) loadOriginal(level, experiments);
    } catch (...) {
        loadingLevel = previous;
        throw;
    }
    loadingLevel = previous;
}

ResourceLocation resourceLocation(const std::string& path) {
    ResourceLocation value;
    const std::array<std::uintptr_t, 2> view{reinterpret_cast<std::uintptr_t>(path.data()), path.size()};
    bind<void(*)(void*, const void*, int)>(SignatureId::ReplayResourceLocationCtor)(&value, &view, 0);
    return value;
}

void prepareRendering() {
    if (renderingPrepared || blockNames.empty()) return;
    if (registeredGroups.empty()) throw std::runtime_error("Recorded blocks were not registered during world initialization");
    auto* client = ReplayWorldLauncher::get().clientInstance();
    if (!client) throw std::runtime_error("Client instance is unavailable");
    auto* resources = requireMethod<void*(*)(void*)>(client, layout::ClientResourcesSlot)(client);
    if (!resources) throw std::runtime_error("Resource manager is unavailable");
    auto geometry = requireMethod<GeometryReference(*)(void*)>(client, layout::ClientGeometrySlot)(client);
    if (!geometry.alive || !*geometry.alive || !geometry.object) throw std::runtime_error("Geometry group is unavailable");
    auto version = bind<MinEngineVersion(*)(const std::string&)>(SignatureId::ReplayMinEngineVersionFromString)(std::string("0.0.0"));
    struct VersionGuard {
        MinEngineVersion& value;
        ~VersionGuard() {
            const auto destroy = bind<void(*)(void*)>(SignatureId::ReplayInternedStringDtor);
            destroy(value.bytes.data() + 16);
            destroy(value.bytes.data() + 8);
        }
    } guard{version};
    const auto load = requireMethod<bool(*)(const void*, const ResourceLocation&, std::string&)>(resources, layout::ResourcesLoadSlot);
    for (const auto& path : geometryPaths) {
        auto location = resourceLocation(path);
        std::string data;
        if (!load(resources, location, data) || data.empty()) continue;
        if (data.size() > 16 * 1024 * 1024) throw std::runtime_error("Block geometry is too large");
        bind<void(*)(void*, const std::string&, const std::string&, const MinEngineVersion&)>(
            SignatureId::GeometryGroupLoadModelPackFromStringSync)(geometry.object, path, data, version);
    }
    auto location = resourceLocation(std::string("blocks.json"));
    const auto loadAll = bind<std::vector<LoadedResource>(*)(const void*, const ResourceLocation&)>(
        SignatureId::ReplayLoadAllPackResources);
    const auto destroy = bind<void(*)(void*)>(SignatureId::ReplayLoadedResourceDtor);
    const LoadedResources all{loadAll(resources, location), destroy};
    for (const auto& pack : all.values) {
        const auto& data = field<const std::string>(&pack, 0);
        if (data.empty()) continue;
        JsonNode root;
        try { root = JsonReader(data).read(); }
        catch (const std::exception&) { continue; }
        for (const auto& [name, value] : root.members) {
            if (!blockNames.contains(name)) continue;
            const auto* textures = value.get("textures");
            if (!textures) continue;
            std::string texture;
            if (textures->string) texture = textures->text;
            else {
                for (const auto face : {"north", "up", "south", "east", "west", "down"}) {
                    const auto* alias = textures->get(face);
                    if (alias && alias->string) { texture = alias->text; break; }
                }
            }
            if (!texture.empty()) legacyTextures.insert_or_assign(name, std::move(texture));
        }
    }
    renderingPrepared = true;
}

void initializeVisual(void* description, void* storage, void* type) {
    auto& modifiable = field<bool>(storage, layout::StorageModifiable);
    auto& replaceable = field<bool>(storage, layout::StorageReplaceable);
    const auto oldModifiable = modifiable;
    const auto oldReplaceable = replaceable;
    modifiable = true;
    replaceable = true;
    struct FlagGuard {
        bool& modifiable;
        bool& replaceable;
        bool oldModifiable;
        bool oldReplaceable;
        ~FlagGuard() { modifiable = oldModifiable; replaceable = oldReplaceable; }
    } guard{modifiable, replaceable, oldModifiable, oldReplaceable};
    const std::array<void*, 2> context{storage, type};
    requireMethod<void(*)(const void*, const void*)>(description, layout::DescriptionInitializeSlot)(description, &context);
}

void injectVisuals(void* type, const Visuals& visual, void* legacy) {
    auto* typeStorage = static_cast<std::byte*>(type) + layout::TypeStorage;
    const auto inject = [&](void* storage) {
        if (visual.geometry) initializeVisual(visual.geometry.get(), storage, type);
        if (visual.material) initializeVisual(visual.material.get(), storage, type);
        else if (legacy) initializeVisual(legacy, storage, type);
    };
    inject(typeStorage);
    const auto& permutations = field<NativeVector>(type, layout::TypePermutations);
    const auto count = vectorSize(permutations, sizeof(void*), 1048576);
    for (std::size_t i = 0; i < count; ++i) {
        auto* block = *reinterpret_cast<void**>(permutations.begin + i * sizeof(void*));
        if (!block) continue;
        inject(static_cast<std::byte*>(block) + layout::BlockStorage);
        bind<void(*)(void*)>(SignatureId::ReplayBlockFinalizeComponentStorage)(block);
        bind<void(*)(void*)>(SignatureId::ReplayBlockCacheComponentData)(block);
    }
}

struct RenderContext {
    RenderCallback callback;
    void* userdata;
};

void renderBlock(void* userdata, void* type) {
    auto& context = *static_cast<RenderContext*>(userdata);
    if (errorMessage.empty()) {
        try {
            const auto& name = field<std::string>(type, layout::TypeName);
            if (blockNames.contains(name)) {
                const auto at = descriptions.find(name);
                const Visuals empty;
                const auto& visual = at == descriptions.end() ? empty : at->second;
                if (visual.material) injectVisuals(type, visual, nullptr);
                else if (const auto texture = legacyTextures.find(name); texture != legacyTextures.end()) {
                    alignas(8) std::array<std::byte, layout::LegacyMaterialSize> material{};
                    bind<void(*)(void*, const std::string&, int, float, bool, int, bool, bool, bool, bool)>(
                        SignatureId::ReplayLegacyBlockMaterialCtor)(material.data(), texture->second, 5, 1.0f, true, 0, false, false, false, false);
                    struct MaterialGuard {
                        void* object;
                        ~MaterialGuard() { if (auto destroy = method<void(*)(void*)>(object, 0)) destroy(object); }
                    } guard{material.data()};
                    injectVisuals(type, visual, material.data());
                } else if (visual.geometry) injectVisuals(type, visual, nullptr);
            }
        } catch (const std::exception& error) { rememberFailure(error.what()); }
        catch (...) { rememberFailure("Block rendering initialization failed"); }
    }
    if (context.callback) context.callback(context.userdata, type);
}

void renderDetour(void* registry, RenderCallback callback, void* userdata) {
    std::scoped_lock lock(mutex);
    if (!prepared || !recordedStartGame || blockNames.empty() || !errorMessage.empty()) {
        if (renderOriginal) renderOriginal(registry, callback, userdata);
        return;
    }
    try { prepareRendering(); }
    catch (const std::exception& error) { rememberFailure(error.what()); }
    catch (...) { rememberFailure("Resource rendering initialization failed"); }
    RenderContext context{callback, userdata};
    if (renderOriginal) renderOriginal(registry, renderBlock, &context);
}

void startGameDetour(void* dispatcher, void* identifier, void* callback, std::shared_ptr<Packet>& packet) {
    std::shared_ptr<Packet> recorded;
    bool recording{};
    {
        std::scoped_lock lock(mutex);
        recording = !prepared;
        if (prepared && errorMessage.empty()) recorded = recordedStartGame;
    }
    if (recording && packet && ReplayRuntime::get().enabled()) ReplayRecorder::get().observeClientPacket(*packet);
    std::shared_ptr<Packet> copy;
    if (recorded && packet) {
        std::string payload;
        copy = native::createPacket(static_cast<int>(MinecraftPacketIds::StartGame));
        if (!copy || !native::serialize(*recorded, payload) || !native::deserialize(*copy, payload)) {
            std::scoped_lock lock(mutex);
            rememberFailure("Recorded block properties cannot be applied");
            copy.reset();
        }
    }
    if (copy && packet) {
        auto localPacket = packet;
        auto& local = field<NativeVector>(localPacket.get(), layout::StartGameBlockProperties);
        auto& properties = field<NativeVector>(copy.get(), layout::StartGameBlockProperties);
        std::swap(local, properties);
        struct SwapGuard {
            NativeVector& local;
            NativeVector& recorded;
            ~SwapGuard() { std::swap(local, recorded); }
        } guard{local, properties};
        if (startGameOriginal) startGameOriginal(dispatcher, identifier, callback, packet);
    } else if (startGameOriginal) startGameOriginal(dispatcher, identifier, callback, packet);
}

std::shared_ptr<Packet> decode(const SerializedPacket* serialized) {
    if (!serialized) return {};
    auto packet = native::createPacket(serialized->id);
    if (!packet || !native::deserialize(*packet, serialized->payload)) throw std::runtime_error("Recorded configuration packet cannot be decoded");
    return packet;
}

constexpr auto bindings = std::to_array<SignatureId>({
    SignatureId::ReplayLevelLoadBlockDefinitions,
    SignatureId::BlockDefinitionGroupRegisterBlocks,
    SignatureId::BlockDefinitionGroupDigestServerBlockProperties,
    SignatureId::BlockTypeRegistryFinalizeForRendering,
    SignatureId::GeometryGroupLoadModelPackFromStringSync,
    SignatureId::ReplayPacksInfoCollectKeys,
    SignatureId::ReplayResourceLocationCtor,
    SignatureId::ReplayMinEngineVersionFromString,
    SignatureId::ReplayInternedStringDtor,
    SignatureId::ReplayBlockFinalizeComponentStorage,
    SignatureId::ReplayLegacyBlockMaterialCtor,
    SignatureId::ReplayBlockCacheComponentData,
    SignatureId::ReplayLoadAllPackResources,
    SignatureId::ReplayLoadedResourceDtor
});
}

bool install() {
    indexedPaletteTable = bedrocktools::core::rtti::vtable("12BlockPalette");
    hashedPaletteTable = bedrocktools::core::rtti::vtable("16BlockHashPalette");
    if (!indexedPaletteTable || !hashedPaletteTable) return false;
    if (handles[0]) return true;
    for (const auto id : bindings) if (!bedrocktools::memory::resolve(id)) return false;
    const auto hook = [&](std::size_t index, SignatureId id, void* detour, void** original) {
        handles[index] = bedrocktools::hooks::install(reinterpret_cast<void*>(bedrocktools::memory::resolve(id)), detour, original);
        return handles[index] != nullptr;
    };
    bool ready = hook(0, SignatureId::ReplayLevelLoadBlockDefinitions, reinterpret_cast<void*>(loadDetour), reinterpret_cast<void**>(&loadOriginal))
        && hook(1, SignatureId::BlockDefinitionGroupRegisterBlocks, reinterpret_cast<void*>(registerDetour), reinterpret_cast<void**>(&registerOriginal))
        && hook(2, SignatureId::BlockTypeRegistryFinalizeForRendering, reinterpret_cast<void*>(renderDetour), reinterpret_cast<void**>(&renderOriginal));
    if (ready) {
        auto packet = native::createPacket(static_cast<int>(MinecraftPacketIds::StartGame));
        auto* dispatcher = packet ? native::packetHandler(*packet) : nullptr;
        auto target = method<DispatchFn>(dispatcher, 2);
        if (target) handles[3] = bedrocktools::hooks::install(reinterpret_cast<void*>(target), reinterpret_cast<void*>(startGameDetour), reinterpret_cast<void**>(&startGameOriginal));
        ready = handles[3] != nullptr;
    }
    if (!ready) uninstall();
    return ready;
}

void release() {
    std::scoped_lock lock(mutex);
    prepared = false;
    recordedStartGame.reset();
    recordedInfo.reset();
    recordedStack.reset();
    registeredGroups.clear();
    blockNames.clear();
    geometryPaths.clear();
    descriptions.clear();
    legacyTextures.clear();
    renderingPrepared = false;
    errorMessage.clear();
    if (cachedRepository) pendingRepositories.emplace(std::exchange(cachedRepository, nullptr));
}

void flushRelease() {
    std::scoped_lock lock(mutex);
    auto* client = ReplayWorldLauncher::get().clientInstance();
    if (!client || client->localPlayer()) return;
    for (auto* repository : pendingRepositories) {
        try { requireMethod<void(*)(void*)>(repository, layout::RepositoryRemoveCacheSlot)(repository); }
        catch (...) {}
    }
    pendingRepositories.clear();
}

void uninstall() {
    release();
    flushRelease();
    for (auto& handle : handles) {
        if (handle) bedrocktools::hooks::remove(handle);
        handle = nullptr;
    }
    loadOriginal = nullptr;
    registerOriginal = nullptr;
    renderOriginal = nullptr;
    startGameOriginal = nullptr;
    indexedPaletteTable = nullptr;
    hashedPaletteTable = nullptr;
}

bool prepare(const std::vector<SerializedPacket>& packets, std::string& error) {
    std::scoped_lock lock(mutex);
    release();
    const auto find = [&](MinecraftPacketIds id) -> const SerializedPacket* {
        for (auto at = packets.rbegin(); at != packets.rend(); ++at) if (at->id == static_cast<int>(id)) return &*at;
        return nullptr;
    };
    try {
        if (!handles[3]) throw std::runtime_error("Custom content hooks are unavailable");
        auto* client = ReplayWorldLauncher::get().clientInstance();
        if (!client || client->localPlayer()) throw std::runtime_error("Leave the current world before opening a replay");
        flushRelease();
        recordedStartGame = decode(find(MinecraftPacketIds::StartGame));
        recordedInfo = decode(find(MinecraftPacketIds::ResourcePacksInfo));
        recordedStack = decode(find(MinecraftPacketIds::ResourcePackStack));
        if (recordedStartGame) {
            const auto& properties = field<NativeVector>(recordedStartGame.get(), layout::StartGameBlockProperties);
            const auto count = vectorSize(properties, layout::ServerBlockPropertySize, 65536);
            std::size_t budget = 1000000;
            for (std::size_t i = 0; i < count; ++i) {
                auto* property = properties.begin + i * layout::ServerBlockPropertySize;
                const auto& name = *reinterpret_cast<const std::string*>(property);
                if (name.empty() || name.size() > 1024) throw std::runtime_error("Invalid custom block name");
                blockNames.emplace(name);
                collectGeometry(property + layout::ServerBlockPropertyTag, 0, budget);
            }
        }
        if (!blockNames.empty() && (!recordedInfo || !recordedStack)) throw std::runtime_error("Custom blocks require the recorded resource-pack handshake");
        if (recordedInfo && recordedStack) {
            auto* client = ReplayWorldLauncher::get().clientInstance();
            if (!client) throw std::runtime_error("Client instance is unavailable");
            auto* repository = requireMethod<void*(*)(void*)>(client, layout::ClientRepositorySlot)(client);
            if (!repository) throw std::runtime_error("Resource-pack repository is unavailable");
            auto keys = bind<ContentKeys(*)(const void*)>(SignatureId::ReplayPacksInfoCollectKeys)(recordedInfo->payload());
            cachedRepository = repository;
            requireMethod<void(*)(void*, const ContentKeys*)>(repository, layout::RepositoryAddCacheSlot)(repository, &keys);
        } else {
            recordedInfo.reset();
            recordedStack.reset();
        }
        prepared = true;
        return true;
    } catch (const std::exception& failure) { error = "Replay custom content: " + std::string(failure.what()); }
    catch (...) { error = "Replay custom content preparation failed"; }
    release();
    return false;
}

std::string failure() {
    std::scoped_lock lock(mutex);
    return errorMessage;
}

void reportFailure(std::string_view message) {
    std::scoped_lock lock(mutex);
    rememberFailure(message);
}

std::shared_ptr<Packet> handshakePacket(int id) {
    std::scoped_lock lock(mutex);
    if (!prepared) return {};
    if (id == static_cast<int>(MinecraftPacketIds::ResourcePacksInfo)) return recordedInfo;
    if (id == static_cast<int>(MinecraftPacketIds::ResourcePackStack)) return recordedStack;
    return {};
}

std::optional<bool> blockPaletteUsesHashes() {
    try {
        auto* client = ReplayWorldLauncher::get().clientInstance();
        auto* player = client ? client->localPlayer() : nullptr;
        auto* level = player ? player->level() : nullptr;
        if (!level || !indexedPaletteTable || !hashedPaletteTable) return std::nullopt;
        auto* palette = requireMethod<void*(*)(void*)>(level, layout::LevelPaletteSlot)(level);
        if (!palette) return std::nullopt;
        auto** table = *reinterpret_cast<void***>(palette);
        if (table == hashedPaletteTable) return true;
        if (table == indexedPaletteTable) return false;
    } catch (...) {}
    return std::nullopt;
}

bool serializeReplayPacket(const Packet& packet, std::string& output) {
    try {
        const auto id = static_cast<MinecraftPacketIds>(native::packetId(packet));
        if (!hasBlockRuntimeIds(id)) return native::serialize(packet, output);
        if (const auto offset = itemDescriptorOffset(id);
            offset && field<std::uint32_t>(packet.payload(), *offset + layout::ItemDescriptorBlockRuntimeId) == 0) {
            return native::serialize(packet, output);
        }
        auto* client = ReplayWorldLauncher::get().clientInstance();
        auto* player = client ? client->localPlayer() : nullptr;
        auto* level = player ? player->level() : nullptr;
        if (!level) return false;
        auto* palette = requireMethod<void*(*)(void*)>(level, layout::LevelPaletteSlot)(level);
        if (!palette || !indexedPaletteTable || !hashedPaletteTable) return false;
        auto** table = *reinterpret_cast<void***>(palette);
        if (table == hashedPaletteTable) return native::serialize(packet, output);
        if (table != indexedPaletteTable) return false;
        if (!native::serialize(packet, output)) return false;
        auto copy = native::createPacket(static_cast<int>(id));
        if (!copy || !native::deserialize(*copy, output)) return false;
        const auto& blocks = field<NativeVector>(palette, layout::PaletteBegin);
        const auto count = vectorSize(blocks, sizeof(void*), 4194304);
        const auto convert = [&](std::uint32_t& runtimeId) {
            const auto* block = runtimeId < count
                ? *reinterpret_cast<void**>(blocks.begin + runtimeId * sizeof(void*))
                : blockFromHash(level, runtimeId);
            if (!block) return false;
            runtimeId = field<std::uint32_t>(block, layout::BlockNetworkHash);
            return true;
        };
        if (!convertBlockRuntimeIds(*copy, id, convert)) return false;
        return native::serialize(*copy, output);
    } catch (...) {
        output.clear();
        return false;
    }
}

bool prepareReplayPacket(Packet& packet) {
    try {
        const auto id = static_cast<MinecraftPacketIds>(native::packetId(packet));
        if (!hasBlockRuntimeIds(id)) return true;
        if (const auto offset = itemDescriptorOffset(id);
            offset && field<std::uint32_t>(packet.payload(), *offset + layout::ItemDescriptorBlockRuntimeId) == 0) return true;
        auto* client = ReplayWorldLauncher::get().clientInstance();
        auto* player = client ? client->localPlayer() : nullptr;
        auto* level = player ? player->level() : nullptr;
        if (!level) return false;
        auto* palette = requireMethod<void*(*)(void*)>(level, layout::LevelPaletteSlot)(level);
        if (!palette || !indexedPaletteTable || !hashedPaletteTable) return false;
        auto** table = *reinterpret_cast<void***>(palette);
        if (table == hashedPaletteTable) return true;
        if (table != indexedPaletteTable) return false;
        const auto& blocks = field<NativeVector>(palette, layout::PaletteBegin);
        const auto count = vectorSize(blocks, sizeof(void*), 4194304);
        const auto convert = [&](std::uint32_t& runtimeId) {
            if (const auto* block = blockFromHash(level, runtimeId)) {
                const auto index = field<std::uint32_t>(block, layout::BlockNetworkRuntimeId);
                if (index >= count || *reinterpret_cast<void**>(blocks.begin + index * sizeof(void*)) != block) return false;
                runtimeId = index;
                return true;
            }
            return runtimeId < count;
        };
        return convertBlockRuntimeIds(packet, id, convert);
    } catch (...) {
        return false;
    }
}
}

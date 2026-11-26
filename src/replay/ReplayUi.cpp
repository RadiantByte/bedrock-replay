#include "ReplayUi.hpp"
#include "ReplayIcons.hpp"
#include "ReplayLogo.hpp"
#include "ReplayLoadingBackground.hpp"

#include "CinematicCamera.hpp"
#include "ReplayArchive.hpp"
#include "ReplayExporter.hpp"
#include "ReplayPlayer.hpp"
#include "ReplayRecorder.hpp"
#include "core/Runtime.hpp"
#include "runtime/InputBridge.hpp"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/bitmap.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <fstream>
#include <limits>
#include <system_error>
#include <zlib.h>

struct AImageDecoder;
struct AImageDecoderHeaderInfo;

namespace bedrocktools::replay {
namespace {

constexpr int ImageDecoderSuccess = 0;

constexpr int ActionDown = 0;
constexpr int ActionUp = 1;
constexpr int ActionMove = 2;
constexpr int ActionCancel = 3;
constexpr int KeyBack = 4;
constexpr int KeyEnter = 66;
constexpr int KeyBackspace = 67;
constexpr int KeyTab = 61;
constexpr int KeyLeft = 21;
constexpr int KeyRight = 22;
constexpr int KeyUp = 19;
constexpr int KeyDown = 20;
constexpr int KeyEscape = 111;

constexpr ImU32 ColorBackground = IM_COL32(7, 10, 17, 250);
constexpr ImU32 ColorPanel = IM_COL32(15, 20, 30, 252);
constexpr ImU32 ColorPanelRaised = IM_COL32(24, 31, 44, 255);
constexpr ImU32 ColorBorder = IM_COL32(42, 51, 67, 255);
constexpr ImU32 ColorText = IM_COL32(238, 243, 251, 255);
constexpr ImU32 ColorTextDim = IM_COL32(145, 162, 186, 255);
constexpr ImU32 ColorAccent = IM_COL32(159, 121, 250, 255);
constexpr ImU32 ColorAccentHover = IM_COL32(180, 151, 255, 255);
constexpr ImU32 ColorCyan = IM_COL32(96, 157, 247, 255);
constexpr ImU32 ColorTime = IM_COL32(249, 179, 81, 255);
constexpr ImU32 ColorDanger = IM_COL32(218, 87, 118, 255);
constexpr std::uint32_t ThumbnailMagic = 0x4D485452u;

using DocumentResultCallback = void (*)(bool, const char*, const char*, const char*, void*);
using RequestDocumentFn = bool (*)(const char*, DocumentResultCallback, void*);
bool replayGameplayVisible() {
    const auto hud = bedrockreplay::input::hudScreenOpen();
    const auto menu = bedrockreplay::input::showingMenu();
    const auto pause = bedrockreplay::input::pauseMenuOpen();
    if (!hud || !menu || !pause) {
        const auto state = ReplayPlayer::get().state();
        return state == ReplayPlayer::State::Playing
            || state == ReplayPlayer::State::Paused
            || state == ReplayPlayer::State::Finished
            || state == ReplayPlayer::State::ResettingSnapshot;
    }
    return *hud && !*menu && !*pause;
}

ImGuiKey mapKey(int keyCode) {
    switch (keyCode) {
        case KeyTab: return ImGuiKey_Tab;
        case KeyLeft: return ImGuiKey_LeftArrow;
        case KeyRight: return ImGuiKey_RightArrow;
        case KeyUp: return ImGuiKey_UpArrow;
        case KeyDown: return ImGuiKey_DownArrow;
        case KeyEnter: return ImGuiKey_Enter;
        case KeyBackspace: return ImGuiKey_Backspace;
        case KeyEscape:
        case KeyBack: return ImGuiKey_Escape;
        default: return ImGuiKey_None;
    }
}

float uiScale() {
    return std::clamp(ImGui::GetIO().DisplaySize.y / 720.0f, 0.78f, 1.45f);
}

std::uint64_t monotonicMilliseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string durationText(std::uint32_t ticks) {
    const std::uint32_t seconds = ticks / 20;
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%u:%02u", seconds / 60, seconds % 60);
    return buffer;
}

std::string sizeText(std::uintmax_t bytes) {
    char buffer[32]{};
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        std::snprintf(buffer, sizeof(buffer), "%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024ull) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return buffer;
}

std::string modifiedText(const std::filesystem::path& path) {
    std::error_code ec;
    const auto fileTime = std::filesystem::last_write_time(path, ec);
    if (ec) return "Unknown";
    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        fileTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t value = std::chrono::system_clock::to_time_t(systemTime);
    std::tm local{};
    localtime_r(&value, &local);
    char buffer[40]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d  %H:%M", &local);
    return buffer;
}

std::string lowerText(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string safeReplayStem(std::string value) {
    const auto extension = std::filesystem::path(value).extension().string();
    if (!extension.empty()) value.resize(value.size() - extension.size());
    for (char& character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (!(std::isalnum(byte) || character == ' ' || character == '-' || character == '_')) character = '_';
    }
    while (!value.empty() && (value.front() == ' ' || value.front() == '.')) value.erase(value.begin());
    while (!value.empty() && (value.back() == ' ' || value.back() == '.')) value.pop_back();
    if (value.empty()) value = "Replay";
    if (value.size() > 96) value.resize(96);
    return value;
}

std::filesystem::path thumbnailPath(std::filesystem::path replay) {
    replay.replace_extension(".replaythumb");
    return replay;
}

std::filesystem::path metadataPath(std::filesystem::path replay) {
    replay.replace_extension(".replaymeta");
    return replay;
}

void writeWorldMetadata(const std::filesystem::path& replay, std::string worldName) {
    std::replace(worldName.begin(), worldName.end(), '\n', ' ');
    std::replace(worldName.begin(), worldName.end(), '\r', ' ');
    if (worldName.empty()) worldName = "Unknown world";
    std::ofstream output(metadataPath(replay), std::ios::binary | std::ios::trunc);
    if (output) output << "RPLMETA1\n" << worldName;
}

std::string readWorldMetadata(const std::filesystem::path& replay) {
    std::ifstream input(metadataPath(replay), std::ios::binary);
    std::string marker;
    std::string worldName;
    if (!input || !std::getline(input, marker) || marker != "RPLMETA1") return "Unknown world";
    std::getline(input, worldName, '\0');
    return worldName.empty() ? "Unknown world" : worldName;
}

bool activeReplayState(ReplayPlayer::State state) {
    return state == ReplayPlayer::State::StartingWorld
        || state == ReplayPlayer::State::LoadingSnapshot
        || state == ReplayPlayer::State::ResettingSnapshot
        || state == ReplayPlayer::State::Playing
        || state == ReplayPlayer::State::Paused
        || state == ReplayPlayer::State::Finished
        || state == ReplayPlayer::State::Failed
        || state == ReplayPlayer::State::Stopping;
}

void accentButtonColors() {
    ImGui::PushStyleColor(ImGuiCol_Button, ColorAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ColorAccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(126, 91, 216, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(12, 11, 27, 255));
}

void cyanButtonColors() {
    ImGui::PushStyleColor(ImGuiCol_Button, ColorCyan);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(128, 182, 255, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(71, 127, 217, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(9, 18, 38, 255));
}

void dangerButtonColors() {
    ImGui::PushStyleColor(ImGuiCol_Button, ColorDanger);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(238, 107, 135, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(181, 62, 94, 255));
}

void drawFallbackThumbnail(ImDrawList* draw, ImVec2 minimum, ImVec2 maximum) {
    draw->AddRectFilledMultiColor(
        minimum,
        maximum,
        IM_COL32(35, 44, 80, 255),
        IM_COL32(37, 33, 76, 255),
        IM_COL32(13, 20, 39, 255),
        IM_COL32(18, 27, 50, 255)
    );
    const float width = maximum.x - minimum.x;
    const float height = maximum.y - minimum.y;
    draw->AddCircleFilled(ImVec2(minimum.x + width * 0.77f, minimum.y + height * 0.25f), height * 0.11f, ColorCyan);
    const ImVec2 ridge[] = {
        {minimum.x, maximum.y},
        {minimum.x + width * 0.32f, minimum.y + height * 0.42f},
        {minimum.x + width * 0.55f, maximum.y},
    };
    draw->AddTriangleFilled(ridge[0], ridge[1], ridge[2], IM_COL32(66, 93, 167, 255));
    const ImVec2 ridge2[] = {
        {minimum.x + width * 0.30f, maximum.y},
        {minimum.x + width * 0.65f, minimum.y + height * 0.34f},
        {maximum.x, maximum.y},
    };
    draw->AddTriangleFilled(ridge2[0], ridge2[1], ridge2[2], IM_COL32(103, 82, 179, 255));
}

}

ReplayUi& ReplayUi::get() {
    static ReplayUi ui;
    return ui;
}

void ReplayUi::installInput() {
    if (mInputInstalled.load(std::memory_order_acquire)) return;
    bool expected = false;
    if (!mInputInstalling.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    const bool touch = bedrockreplay::input::registerTouchCallback([](const pl::input::TouchEvent& event) { return ReplayUi::get().onTouch(event); });
    const bool key = bedrockreplay::input::registerKeyCallback([](const pl::input::KeyEvent& event) { return ReplayUi::get().onKey(event); });
    const bool text = bedrockreplay::input::registerTextInputCallback([](const pl::input::TextInputEvent& event) { return ReplayUi::get().onText(event); });
    const bool mouse = bedrockreplay::input::registerMouseCallback([](const pl::input::MouseEvent& event) { return ReplayUi::get().onMouse(event); });
    mInputInstalled.store(touch && key && text && mouse, std::memory_order_release);
    mInputInstalling.store(false, std::memory_order_release);
}

void ReplayUi::requestBrowserOpen() {
    mOpenDelayFrames.store(4, std::memory_order_release);
}

void ReplayUi::openBrowser() {
    CinematicCamera::get().endSession();
    mEditMode.store(false, std::memory_order_release);
    mReplayControlsHidden.store(false, std::memory_order_release);
    mEditAutoPausePending.store(false, std::memory_order_release);
    mOpenDelayFrames.store(0, std::memory_order_release);
    mRefreshRequested = true;
    mBrowserVisible.store(true, std::memory_order_release);
}

void ReplayUi::closeBrowser() {
    if (mKeyboardVisible.exchange(false, std::memory_order_acq_rel)) bedrockreplay::input::hideKeyboard();
    mBrowserVisible.store(false, std::memory_order_release);
    std::scoped_lock lock(mMutex);
    mPendingInput.clear();
}

bool ReplayUi::browserVisible() const noexcept {
    return mBrowserVisible.load(std::memory_order_acquire);
}

bool ReplayUi::editingReplay() const noexcept {
    return mEditMode.load(std::memory_order_acquire);
}

bool ReplayUi::advancedViewportActive() const noexcept {
    return mAdvancedInputActive.load(std::memory_order_acquire)
        && mEditMode.load(std::memory_order_acquire)
        && !mReplayControlsHidden.load(std::memory_order_acquire)
        && !mBrowserVisible.load(std::memory_order_acquire);
}

void ReplayUi::loadAdvancedSensitivity() {
    const auto path = ReplayRecorder::get().libraryDirectory().parent_path() / "editor-settings.txt";
    if (mAdvancedSensitivityLoaded && path == mAdvancedSensitivityFile) return;
    mAdvancedSensitivityFile = path;
    mAdvancedSensitivityLoaded = true;
    mAdvancedLookSensitivity.store(2.0f, std::memory_order_release);
    std::ifstream input(path);
    std::string key;
    float value = 0.0f;
    if (input >> key >> value && key == "look_sensitivity" && std::isfinite(value)
        && value >= 0.25f && value <= 6.0f) {
        mAdvancedLookSensitivity.store(value, std::memory_order_release);
    }
}

void ReplayUi::saveAdvancedSensitivity() {
    if (!mAdvancedSensitivityLoaded || mAdvancedSensitivityFile.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(mAdvancedSensitivityFile.parent_path(), error);
    if (error) return;
    auto temporary = mAdvancedSensitivityFile;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return;
        output << "look_sensitivity " << mAdvancedLookSensitivity.load(std::memory_order_acquire) << '\n';
        if (!output) return;
    }
    std::filesystem::rename(temporary, mAdvancedSensitivityFile, error);
    if (error) std::filesystem::remove(temporary, error);
}

void ReplayUi::refresh() {
    std::vector<Entry> entries;
    std::string error;
    std::size_t unreadableCount = 0;
    std::filesystem::path selectedBefore;
    {
        std::scoped_lock lock(mMutex);
        selectedBefore = mSelectedPath;
    }
    const auto directory = ReplayRecorder::get().libraryDirectory();
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        error = "Unable to open replay library";
    } else {
        for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file()) continue;
            const auto path = it->path();
            if (lowerText(path.extension().string()) != ".playback") continue;
            ReplayArchiveData replay;
            std::string readError;
            if (!readReplayArchive(path, replay, &readError)) {
                ++unreadableCount;
                continue;
            }
            Entry entry;
            entry.path = path;
            entry.name = path.stem().string();
            entry.worldName = readWorldMetadata(path);
            entry.durationTicks = replay.durationTicks;
            if (path == selectedBefore) entry.archive = std::make_shared<ReplayArchiveData>(std::move(replay));
            entry.size = it->file_size(ec);
            if (ec) {
                ec.clear();
                entry.size = 0;
            }
            entry.modified = std::filesystem::last_write_time(path, ec);
            if (ec) ec.clear();

            std::ifstream thumbnailInput(thumbnailPath(path), std::ios::binary);
            std::uint32_t magic{}, width{}, height{};
            if (thumbnailInput.read(reinterpret_cast<char*>(&magic), sizeof(magic))
                && thumbnailInput.read(reinterpret_cast<char*>(&width), sizeof(width))
                && thumbnailInput.read(reinterpret_cast<char*>(&height), sizeof(height))
                && magic == ThumbnailMagic && width > 0 && height > 0 && width <= 640 && height <= 360) {
                auto thumbnail = std::make_shared<Thumbnail>();
                thumbnail->width = static_cast<int>(width);
                thumbnail->height = static_cast<int>(height);
                thumbnail->pixels.resize(static_cast<std::size_t>(width) * height * 4);
                if (thumbnailInput.read(reinterpret_cast<char*>(thumbnail->pixels.data()),
                        static_cast<std::streamsize>(thumbnail->pixels.size()))) {
                    entry.thumbnail = std::move(thumbnail);
                }
            }
            entries.emplace_back(std::move(entry));
        }
        if (ec) error = "Unable to scan replay library";
        else if (unreadableCount != 0) error = "Skipped " + std::to_string(unreadableCount) + " unreadable replay file(s)";
    }

    const auto mode = mSortMode;
    std::sort(entries.begin(), entries.end(), [mode](const Entry& a, const Entry& b) {
        if (mode == SortMode::Name) {
            const auto left = lowerText(a.name);
            const auto right = lowerText(b.name);
            return left == right ? a.modified > b.modified : left < right;
        }
        return a.modified == b.modified ? lowerText(a.name) < lowerText(b.name) : a.modified > b.modified;
    });

    std::scoped_lock lock(mMutex);
    mEntries = std::move(entries);
    mError = std::move(error);
    if (!mSelectedPath.empty()) {
        const bool exists = std::any_of(mEntries.begin(), mEntries.end(), [&](const Entry& entry) {
            return entry.path == mSelectedPath;
        });
        if (!exists) mSelectedPath.clear();
    }
    mRefreshRequested = false;
}

void ReplayUi::resetImGui(void* context) {
    mRenderReady.store(false, std::memory_order_release);
    mAdvancedInputActive.store(false, std::memory_order_release);

    mAdvancedSceneTexture = 0;
    mAdvancedSceneFramebuffer = 0;
    mAdvancedSceneWidth = mAdvancedSceneHeight = 0;
    mAdvancedSceneSourceWidth = mAdvancedSceneSourceHeight = 0;
    mAdvancedSceneSurface = nullptr;
    mAdvancedSceneDirectCopy = mAdvancedSceneCopyChecked = false;
    mLogoTexture = 0;
    mLoadingBackgroundTexture = 0;
    if (mImGuiInitialized && ImGui::GetCurrentContext()) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
    }
    mImGuiInitialized = false;
    mEglContext = context;
    std::scoped_lock lock(mMutex);
    mPendingInput.clear();
    mAdvancedMovePointer = mAdvancedLookPointer = mAdvancedLiftPointer = mAdvancedTurnPointer = -1;
    mAdvancedMoveX = mAdvancedMoveY = mAdvancedLift = mAdvancedTurn = mAdvancedLookX = mAdvancedLookY = 0;
    mControlMinX = mControlMinY = mControlMaxX = mControlMaxY = 0.0f;
    for (auto& entry : mEntries) if (entry.thumbnail) entry.thumbnail->texture = 0;
}

void ReplayUi::ensureImGui(float displayWidth, float displayHeight) {
    if (mImGuiInitialized) return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    const float scale = std::clamp(displayHeight / 720.0f, 0.78f, 1.45f);
    const auto fontPath = bedrocktools::core::Runtime::get().resourceDirectory() / "minecraft.ttf";
    std::error_code ec;
    if (std::filesystem::is_regular_file(fontPath, ec)) {
        io.FontDefault = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 18.0f * scale);
    }
    if (!io.FontDefault) io.FontDefault = io.Fonts->AddFontDefault();

    ImFontConfig iconConfig;
    iconConfig.MergeMode = true;
    iconConfig.PixelSnapH = true;
    iconConfig.FontDataOwnedByAtlas = false;
    static const ImWchar iconRanges[] = {0xe000, 0xe7ff, 0};
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(icons::FontBytes),
        static_cast<int>(sizeof(icons::FontBytes)), 21.0f * scale, &iconConfig, iconRanges);

    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(15.0f, 13.0f);
    style.FramePadding = ImVec2(13.0f, 8.0f);
    style.ItemSpacing = ImVec2(9.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(7.0f, 5.0f);
    style.WindowRounding = 14.0f;
    style.ChildRounding = 11.0f;
    style.FrameRounding = 9.0f;
    style.PopupRounding = 11.0f;
    style.ScrollbarSize = 15.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabMinSize = 22.0f;
    style.GrabRounding = 8.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.ScaleAllSizes(scale);
    auto* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(ColorBackground);
    colors[ImGuiCol_ChildBg] = ImGui::ColorConvertU32ToFloat4(ColorPanel);
    colors[ImGuiCol_PopupBg] = ImGui::ColorConvertU32ToFloat4(ColorPanelRaised);
    colors[ImGuiCol_Border] = ImGui::ColorConvertU32ToFloat4(ColorBorder);
    colors[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(ColorText);
    colors[ImGuiCol_TextDisabled] = ImGui::ColorConvertU32ToFloat4(ColorTextDim);
    colors[ImGuiCol_Button] = ImGui::ColorConvertU32ToFloat4(ColorPanelRaised);
    colors[ImGuiCol_ButtonHovered] = ImGui::ColorConvertU32ToFloat4(IM_COL32(44, 54, 73, 255));
    colors[ImGuiCol_ButtonActive] = ImGui::ColorConvertU32ToFloat4(IM_COL32(72, 58, 119, 255));
    colors[ImGuiCol_FrameBg] = ImGui::ColorConvertU32ToFloat4(IM_COL32(20, 27, 39, 255));
    colors[ImGuiCol_FrameBgHovered] = ImGui::ColorConvertU32ToFloat4(IM_COL32(34, 43, 64, 255));
    colors[ImGuiCol_FrameBgActive] = ImGui::ColorConvertU32ToFloat4(IM_COL32(49, 46, 82, 255));
    colors[ImGuiCol_Header] = ImGui::ColorConvertU32ToFloat4(IM_COL32(38, 42, 69, 255));
    colors[ImGuiCol_HeaderHovered] = ImGui::ColorConvertU32ToFloat4(IM_COL32(58, 51, 96, 255));
    colors[ImGuiCol_HeaderActive] = ImGui::ColorConvertU32ToFloat4(ColorAccent);
    colors[ImGuiCol_SliderGrab] = ImGui::ColorConvertU32ToFloat4(ColorAccent);
    colors[ImGuiCol_SliderGrabActive] = ImGui::ColorConvertU32ToFloat4(ColorAccentHover);
    colors[ImGuiCol_Separator] = ImGui::ColorConvertU32ToFloat4(ColorBorder);
    colors[ImGuiCol_CheckMark] = ImGui::ColorConvertU32ToFloat4(ColorAccent);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.01f, 0.02f, 0.06f, 0.76f);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    mImGuiInitialized = true;
}

void ReplayUi::requestImport() {
    if (mImportPending.exchange(true, std::memory_order_acq_rel)) return;
    auto requestName = "PLRequestDocument";
    auto request = reinterpret_cast<RequestDocumentFn>(dlsym(RTLD_DEFAULT, requestName));
    if (!request) {
        std::scoped_lock lock(mMutex);
        mStatusMessage = "Import requires the updated LeviLauncher preloader";
        mImportPending = false;
        return;
    }
    const bool opened = request(
        "*/*",
        [](bool success, const char* path, const char* displayName, const char* error, void* userData) {
            static_cast<ReplayUi*>(userData)->finishImport(success, path, displayName, error);
        },
        this);
    if (!opened) {
        std::scoped_lock lock(mMutex);
        mStatusMessage = "Another mod is already choosing a document";
        mImportPending = false;
    }
}

void ReplayUi::finishImport(bool success, const char* path, const char* displayName, const char* error) {
    std::string status;
    if (!success || !path || !*path) {
        status = error && *error ? error : "Import cancelled";
    } else {
        const std::filesystem::path source(path);
        const std::string suppliedName = displayName && *displayName ? displayName : source.filename().string();
        if (lowerText(std::filesystem::path(suppliedName).extension().string()) != ".playback") {
            status = "Choose a .playback replay file";
        } else {
            ReplayArchiveData replay;
            std::string readError;
            if (!readReplayArchive(source, replay, &readError)) {
                status = "Import failed: " + readError;
            } else {
                std::error_code ec;
                const auto directory = ReplayRecorder::get().libraryDirectory();
                std::filesystem::create_directories(directory, ec);
                const std::string stem = safeReplayStem(suppliedName);
                std::filesystem::path target = directory / (stem + ".playback");
                for (int suffix = 2; !ec && std::filesystem::exists(target, ec) && suffix < 1000; ++suffix) {
                    target = directory / (stem + " (" + std::to_string(suffix) + ").playback");
                }
                const auto staging = target.string() + ".importing";
                std::filesystem::copy_file(source, staging, std::filesystem::copy_options::overwrite_existing, ec);
                if (!ec) std::filesystem::rename(staging, target, ec);
                if (ec) {
                    std::filesystem::remove(staging, ec);
                    status = "Unable to copy the replay into the library";
                } else {
                    status = "Imported " + target.filename().string();
                    std::scoped_lock lock(mMutex);
                    mSelectedPath = target;
                    mRefreshRequested = true;
                }
            }
        }
    }
    {
        std::scoped_lock lock(mMutex);
        mStatusMessage = std::move(status);
    }
    mImportPending = false;
}

unsigned int ReplayUi::thumbnailTexture(const std::shared_ptr<Thumbnail>& thumbnail) {
    if (!thumbnail || thumbnail->pixels.empty()) return 0;
    if (thumbnail->texture) return thumbnail->texture;
    glGenTextures(1, &thumbnail->texture);
    glBindTexture(GL_TEXTURE_2D, thumbnail->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, thumbnail->width, thumbnail->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, thumbnail->pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return thumbnail->texture;
}

unsigned int ReplayUi::logoTexture() {
    if (mLogoTexture) return mLogoTexture;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(logo::Width) * logo::Height * 4);
    uLongf outputSize = static_cast<uLongf>(pixels.size());
    if (uncompress(pixels.data(), &outputSize, logo::CompressedPixels,
            sizeof(logo::CompressedPixels)) != Z_OK || outputSize != pixels.size()) return 0;
    glGenTextures(1, &mLogoTexture);
    glBindTexture(GL_TEXTURE_2D, mLogoTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, logo::Width, logo::Height, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return mLogoTexture;
}

unsigned int ReplayUi::loadingBackgroundTexture() {
    if (mLoadingBackgroundTexture) return mLoadingBackgroundTexture;

    struct DecoderApi {
        using Create = int (*)(const void*, std::size_t, AImageDecoder**);
        using Destroy = void (*)(AImageDecoder*);
        using Header = const AImageDecoderHeaderInfo* (*)(AImageDecoder*);
        using Dimension = std::int32_t (*)(const AImageDecoderHeaderInfo*);
        using Format = int (*)(AImageDecoder*, std::int32_t);
        using Stride = std::size_t (*)(AImageDecoder*);
        using Decode = int (*)(AImageDecoder*, void*, std::size_t, std::size_t);
        Create create{};
        Destroy destroy{};
        Header header{};
        Dimension width{};
        Dimension height{};
        Format format{};
        Stride stride{};
        Decode decode{};
    };
    static const DecoderApi api = [] {
        DecoderApi result;
        void* library = dlopen("libjnigraphics.so", RTLD_NOW | RTLD_LOCAL);
        if (!library) return result;
        result.create = reinterpret_cast<decltype(result.create)>(dlsym(library, "AImageDecoder_createFromBuffer"));
        result.destroy = reinterpret_cast<decltype(result.destroy)>(dlsym(library, "AImageDecoder_delete"));
        result.header = reinterpret_cast<decltype(result.header)>(dlsym(library, "AImageDecoder_getHeaderInfo"));
        result.width = reinterpret_cast<decltype(result.width)>(dlsym(library, "AImageDecoderHeaderInfo_getWidth"));
        result.height = reinterpret_cast<decltype(result.height)>(dlsym(library, "AImageDecoderHeaderInfo_getHeight"));
        result.format = reinterpret_cast<decltype(result.format)>(dlsym(library, "AImageDecoder_setAndroidBitmapFormat"));
        result.stride = reinterpret_cast<decltype(result.stride)>(dlsym(library, "AImageDecoder_getMinimumStride"));
        result.decode = reinterpret_cast<decltype(result.decode)>(dlsym(library, "AImageDecoder_decodeImage"));
        return result;
    }();
    if (!api.create || !api.destroy || !api.header || !api.width || !api.height
        || !api.format || !api.stride || !api.decode) return 0;
    AImageDecoder* decoder = nullptr;
    if (api.create(loadingBackground::EncodedImage, sizeof(loadingBackground::EncodedImage), &decoder)
            != ImageDecoderSuccess || !decoder) return 0;
    const auto* info = api.header(decoder);
    if (!info || api.width(info) != loadingBackground::Width || api.height(info) != loadingBackground::Height) {
        api.destroy(decoder);
        return 0;
    }
    bool rgb565 = api.format(decoder, ANDROID_BITMAP_FORMAT_RGB_565) == ImageDecoderSuccess;
    if (!rgb565 && api.format(decoder, ANDROID_BITMAP_FORMAT_RGBA_8888) != ImageDecoderSuccess) {
        api.destroy(decoder);
        return 0;
    }
    const auto rowBytes = api.stride(decoder);
    const auto expectedRowBytes = static_cast<std::size_t>(loadingBackground::Width) * (rgb565 ? 2u : 4u);
    if (rowBytes != expectedRowBytes) {
        api.destroy(decoder);
        return 0;
    }
    std::vector<std::uint8_t> pixels(rowBytes * loadingBackground::Height);
    const int decoded = api.decode(decoder, pixels.data(), rowBytes, pixels.size());
    api.destroy(decoder);
    if (decoded != ImageDecoderSuccess) return 0;
    glGenTextures(1, &mLoadingBackgroundTexture);
    glBindTexture(GL_TEXTURE_2D, mLoadingBackgroundTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const GLenum format = rgb565 ? GL_RGB : GL_RGBA;
    glTexImage2D(GL_TEXTURE_2D, 0, format, loadingBackground::Width,
        loadingBackground::Height, 0, format,
        rgb565 ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return mLoadingBackgroundTexture;
}

void ReplayUi::drawThumbnail(const std::shared_ptr<Thumbnail>& thumbnail, float width, float height) {
    const ImVec2 minimum = ImGui::GetCursorScreenPos();
    const ImVec2 maximum(minimum.x + width, minimum.y + height);
    const auto texture = thumbnailTexture(thumbnail);
    ImGui::Dummy(ImVec2(width, height));
    auto* draw = ImGui::GetWindowDrawList();
    if (texture) {

        const float sourceAspect = static_cast<float>(thumbnail->width) / thumbnail->height;
        const float targetAspect = width / height;
        ImVec2 uvMin(0.0f, 0.0f), uvMax(1.0f, 1.0f);
        if (sourceAspect > targetAspect) {
            const float margin = (1.0f - targetAspect / sourceAspect) * 0.5f;
            uvMin.x = margin;
            uvMax.x = 1.0f - margin;
        } else if (sourceAspect < targetAspect) {
            const float margin = (1.0f - sourceAspect / targetAspect) * 0.5f;
            uvMin.y = margin;
            uvMax.y = 1.0f - margin;
        }
        draw->AddImageRounded(reinterpret_cast<ImTextureID>(static_cast<std::intptr_t>(texture)),
            minimum, maximum, uvMin, uvMax, IM_COL32_WHITE, 8.0f * uiScale());
    } else {
        drawFallbackThumbnail(draw, minimum, maximum);
    }
    draw->AddRect(minimum, maximum, ColorBorder, 8.0f * uiScale(), 0, 1.0f);
}

void ReplayUi::renameReplay(const std::filesystem::path& source, std::string requestedName) {
    const std::string stem = safeReplayStem(std::move(requestedName));
    const auto target = source.parent_path() / (stem + ".playback");
    std::error_code ec;
    if (target == source) {
        std::scoped_lock lock(mMutex);
        mPendingRename.clear();
        mStatusMessage = "Replay name unchanged";
        return;
    }
    if (std::filesystem::exists(target, ec)) {
        std::scoped_lock lock(mMutex);
        mStatusMessage = "A replay with that name already exists";
        return;
    }
    std::filesystem::rename(source, target, ec);
    if (!ec) {
        for (const auto& extension : {".replaythumb", ".replaymeta", ".replaycamera"}) {
            auto from = source;
            auto to = target;
            from.replace_extension(extension);
            to.replace_extension(extension);
            std::error_code sidecarError;
            if (std::filesystem::exists(from, sidecarError)) std::filesystem::rename(from, to, sidecarError);
        }
    }
    std::scoped_lock lock(mMutex);
    mStatusMessage = ec ? "Unable to rename the replay" : "Replay renamed to " + target.filename().string();
    if (!ec) mSelectedPath = target;
    mPendingRename.clear();
    mRefreshRequested = true;
}

void ReplayUi::deleteReplayFiles(const std::filesystem::path& replay) {
    std::error_code ec;
    std::filesystem::remove(replay, ec);
    if (!ec) {
        std::error_code sidecarError;
        std::filesystem::remove(thumbnailPath(replay), sidecarError);
        sidecarError.clear();
        std::filesystem::remove(metadataPath(replay), sidecarError);
        sidecarError.clear();
        std::filesystem::remove(cinematicCameraPath(replay), sidecarError);
    }
    std::scoped_lock lock(mMutex);
    mStatusMessage = ec ? "Unable to delete the replay" : "Replay deleted";
    if (!ec && mSelectedPath == replay) mSelectedPath.clear();
    mPendingDelete.clear();
    mRefreshRequested = true;
}

void ReplayUi::drawBrowser() {
    if (mRefreshRequested) refresh();
    auto& io = ImGui::GetIO();
    const float scale = uiScale();
    const ImVec2 size(std::min(io.DisplaySize.x - 16.0f * scale, 1440.0f * scale), io.DisplaySize.y - 16.0f * scale);
    const ImVec2 position((io.DisplaySize.x - size.x) * 0.5f, (io.DisplaySize.y - size.y) * 0.5f);
    ImGui::SetNextWindowPos(position, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    if (!ImGui::Begin("##ReplayLibrary", nullptr, flags)) {
        ImGui::End();
        return;
    }

    std::vector<Entry> entries;
    std::string error;
    std::string status;
    std::filesystem::path selectedPath;
    {
        std::scoped_lock lock(mMutex);
        entries = mEntries;
        error = mError;
        status = mStatusMessage;
        selectedPath = mSelectedPath;
    }

    const ImVec2 headerMinimum = ImGui::GetCursorScreenPos();
    const float headerHeight = 48.0f * scale;
    const ImVec2 headerMaximum(headerMinimum.x + ImGui::GetContentRegionAvail().x, headerMinimum.y + headerHeight);
    auto* windowDraw = ImGui::GetWindowDrawList();
    windowDraw->AddRectFilledMultiColor(
        headerMinimum,
        headerMaximum,
        IM_COL32(46, 39, 83, 255),
        IM_COL32(30, 49, 92, 255),
        IM_COL32(18, 26, 47, 255),
        IM_COL32(26, 28, 63, 255)
    );
    windowDraw->AddRect(headerMinimum, headerMaximum, ColorBorder, 10.0f * scale);
    const ImVec2 headerCursor = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(headerCursor.x + 13.0f * scale, headerCursor.y + 10.0f * scale));
    ImGui::TextUnformatted("REPLAY");
    ImGui::SameLine();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ColorAccent), "LIBRARY");
    ImGui::SameLine();
    ImGui::TextDisabled("%zu", entries.size());

    const float compactButton = 82.0f * scale;
    const float actionTotal = compactButton * 3.0f + 18.0f * scale;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - actionTotal - 12.0f * scale));
    ImGui::BeginDisabled(mImportPending.load(std::memory_order_acquire));
    cyanButtonColors();
    if (ImGui::Button(mImportPending.load(std::memory_order_acquire) ? "Importing" : "Import", ImVec2(compactButton, 0))) requestImport();
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(compactButton, 0))) mRefreshRequested = true;
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(compactButton, 0))) closeBrowser();
    ImGui::SetCursorScreenPos(ImVec2(headerMinimum.x, headerMaximum.y + 7.0f * scale));

    if (!status.empty() || !error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(35, 31, 62, 255));
        ImGui::BeginChild("##Status", ImVec2(0, 37.0f * scale), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::TextWrapped("%s", !status.empty() ? status.c_str() : error.c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    const float sortWidth = 142.0f * scale;
    ImGui::SetNextItemWidth(std::max(120.0f * scale, ImGui::GetContentRegionAvail().x - sortWidth - ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputTextWithHint("##ReplaySearch", "Search replay or world", mSearch, sizeof(mSearch));
    if (ImGui::IsItemActivated() && !mKeyboardVisible.exchange(true, std::memory_order_acq_rel)) bedrockreplay::input::showKeyboard();
    if (ImGui::IsItemDeactivated() && mKeyboardVisible.exchange(false, std::memory_order_acq_rel)) bedrockreplay::input::hideKeyboard();
    ImGui::SameLine();
    const char* sortLabel = mSortMode == SortMode::Newest ? "Date: newest" : "Name: A-Z";
    if (ImGui::Button(sortLabel, ImVec2(sortWidth, 0))) {
        mSortMode = mSortMode == SortMode::Newest ? SortMode::Name : SortMode::Newest;
        mRefreshRequested = true;
    }

    const std::string query = lowerText(mSearch);
    std::vector<std::size_t> filtered;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (query.empty() || lowerText(entries[i].name).find(query) != std::string::npos
            || lowerText(entries[i].worldName).find(query) != std::string::npos) {
            filtered.push_back(i);
        }
    }

    const bool split = ImGui::GetContentRegionAvail().x >= 720.0f * scale;
    const float gap = 10.0f * scale;
    const float listWidth = split ? ImGui::GetContentRegionAvail().x * 0.46f : ImGui::GetContentRegionAvail().x;
    const float listHeight = split ? ImGui::GetContentRegionAvail().y : std::max(150.0f * scale, ImGui::GetContentRegionAvail().y * 0.52f);
    ImGui::BeginChild("##ReplayList", ImVec2(listWidth, listHeight), true);
    if (filtered.empty()) {
        ImGui::TextDisabled("%s", entries.empty() ? "No replays yet" : "No matching replays");
        ImGui::Spacing();
        ImGui::TextWrapped("Use /record start and /record stop, or import a .playback file.");
    }
    for (const auto index : filtered) {
        auto& entry = entries[index];
        ImGui::PushID(static_cast<int>(index));
        const ImVec2 minimum = ImGui::GetCursorScreenPos();
        const ImVec2 cardSize(ImGui::GetContentRegionAvail().x, 100.0f * scale);
        ImGui::InvisibleButton("##ReplayCard", cardSize);
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        const bool selected = entry.path == selectedPath;
        const ImVec2 maximum(minimum.x + cardSize.x, minimum.y + cardSize.y);
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(minimum, maximum, selected ? IM_COL32(45, 39, 77, 255) : hovered ? ColorPanelRaised : IM_COL32(18, 24, 36, 255), 9.0f * scale);
        draw->AddRect(minimum, maximum, selected ? ColorAccent : ColorBorder, 9.0f * scale, 0, selected ? 2.0f * scale : 1.0f);
        const ImVec2 imageMinimum(minimum.x + 7.0f * scale, minimum.y + 7.0f * scale);
        const float imageWidth = std::min(153.0f * scale, cardSize.x * 0.34f);
        const ImVec2 imageMaximum(imageMinimum.x + imageWidth, imageMinimum.y + imageWidth * 9.0f / 16.0f);
        const auto texture = thumbnailTexture(entry.thumbnail);
        if (texture) {
            const float sourceAspect = static_cast<float>(entry.thumbnail->width) / entry.thumbnail->height;
            const float targetAspect = 16.0f / 9.0f;
            ImVec2 uvMin(0.0f, 0.0f), uvMax(1.0f, 1.0f);
            if (sourceAspect > targetAspect) {
                uvMin.x = (1.0f - targetAspect / sourceAspect) * 0.5f;
                uvMax.x = 1.0f - uvMin.x;
            } else if (sourceAspect < targetAspect) {
                uvMin.y = (1.0f - sourceAspect / targetAspect) * 0.5f;
                uvMax.y = 1.0f - uvMin.y;
            }
            draw->AddImageRounded(reinterpret_cast<ImTextureID>(static_cast<std::intptr_t>(texture)),
                imageMinimum, imageMaximum, uvMin, uvMax, IM_COL32_WHITE, 6.0f * scale);
        } else drawFallbackThumbnail(draw, imageMinimum, imageMaximum);
        draw->AddRect(imageMinimum, imageMaximum, ColorBorder, 6.0f * scale);
        const float textX = imageMaximum.x + 11.0f * scale;
        draw->PushClipRect(ImVec2(textX, minimum.y), ImVec2(maximum.x - 8.0f * scale, maximum.y), true);
        draw->AddText(ImVec2(textX, minimum.y + 14.0f * scale), ColorText, entry.name.c_str());
        draw->AddText(ImVec2(textX, minimum.y + 42.0f * scale), ColorCyan, entry.worldName.c_str());
        const std::string metadata = durationText(entry.durationTicks) + "  |  " + sizeText(entry.size);
        draw->AddText(ImVec2(textX, minimum.y + 69.0f * scale), ColorTextDim, metadata.c_str());
        draw->PopClipRect();
        if (clicked) {
            ReplayArchiveData replay;
            std::string readError;
            auto archive = std::make_shared<ReplayArchiveData>();
            if (readReplayArchive(entry.path, replay, &readError)) {
                *archive = std::move(replay);
            } else {
                archive.reset();
            }
            for (auto& candidate : entries) candidate.archive.reset();
            entry.archive = archive;
            selectedPath = entry.path;
            std::scoped_lock lock(mMutex);
            mSelectedPath = entry.path;
            for (auto& candidate : mEntries) {
                candidate.archive = candidate.path == entry.path ? archive : nullptr;
            }
            if (!archive) mStatusMessage = "Unable to prepare replay: " + readError;
        }
        ImGui::Dummy(ImVec2(0, 5.0f * scale));
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (split) ImGui::SameLine(0, gap);
    else ImGui::Dummy(ImVec2(0, gap));

    ImGui::BeginChild("##ReplayDetails", ImVec2(split ? ImGui::GetContentRegionAvail().x : listWidth, ImGui::GetContentRegionAvail().y), true);
    auto selected = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) { return entry.path == selectedPath; });
    if (selected == entries.end()) {
        ImGui::TextDisabled("SELECT A REPLAY");
        ImGui::Spacing();
        ImGui::TextWrapped("Choose a replay for playback, editing, rename, or deletion.");
    } else {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
            ImVec2(ImGui::GetStyle().ItemSpacing.x, 5.0f * scale));
        const float detailWidth = ImGui::GetContentRegionAvail().x;
        const float previewWidth = std::min({detailWidth * 0.88f,
            480.0f * scale, std::max(1.0f, ImGui::GetContentRegionAvail().y - 215.0f * scale) * 16.0f / 9.0f});
        const float previewHeight = previewWidth * 9.0f / 16.0f;
        const float detailStartX = ImGui::GetCursorPosX();
        ImGui::SetCursorPosX(detailStartX + (detailWidth - previewWidth) * 0.5f);
        drawThumbnail(selected->thumbnail, previewWidth, previewHeight);
        ImGui::SetCursorPosX(detailStartX);
        ImGui::TextWrapped("%s", selected->name.c_str());
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ColorAccent), "%s", selected->worldName.c_str());
        const float buttonHeight = 38.0f * scale;
        const float buttonGap = ImGui::GetStyle().ItemSpacing.x;
        const float buttonWidth = (ImGui::GetContentRegionAvail().x - buttonGap) * 0.5f;
        accentButtonColors();
        if (ImGui::Button("Open replay", ImVec2(buttonWidth, buttonHeight))) {
            const auto path = selected->path;
            const auto archive = selected->archive;
            CinematicCamera::get().endSession();
            mEditMode.store(false, std::memory_order_release);
            mReplayControlsHidden.store(false, std::memory_order_release);
            mEditAutoPausePending.store(false, std::memory_order_release);
            closeBrowser();
            mRefreshRequested = true;
            const bool opened = archive ? ReplayPlayer::get().open(path, std::move(*archive))
                : ReplayPlayer::get().open(path);
            if (!opened) openBrowser();
        }
        ImGui::PopStyleColor(4);
        ImGui::SameLine();
        cyanButtonColors();
        if (ImGui::Button("Edit", ImVec2(buttonWidth, buttonHeight))) {
            const auto path = selected->path;
            const auto archive = selected->archive;
            const auto durationTicks = selected->durationTicks;
            CinematicCamera::get().beginSession(path, durationTicks);
            mCameraPropertiesExpanded = false;
            mAdvancedMode = false;
            mAdvancedToolsOpen = false;
            mAdvancedZoom = 1.0f;
            mAdvancedViewStart = 0.0f;
            mAdvancedScrubTrack = -1;
            {
                std::scoped_lock lock(mMutex);
                mAdvancedMovePointer = mAdvancedLookPointer = mAdvancedLiftPointer = mAdvancedTurnPointer = -1;
                mAdvancedMoveX = mAdvancedMoveY = mAdvancedLift = mAdvancedTurn = mAdvancedLookX = mAdvancedLookY = 0;
            }
            mEditMode.store(true, std::memory_order_release);
            mReplayControlsHidden.store(false, std::memory_order_release);
            mEditAutoPausePending.store(true, std::memory_order_release);
            closeBrowser();
            mRefreshRequested = true;
            const bool opened = archive ? ReplayPlayer::get().open(path, std::move(*archive))
                : ReplayPlayer::get().open(path);
            if (!opened) {
                CinematicCamera::get().endSession();
                openBrowser();
            }
        }
        ImGui::PopStyleColor(4);
        if (ImGui::Button("Rename", ImVec2(buttonWidth, buttonHeight))) {
            std::scoped_lock lock(mMutex);
            mPendingRename = selected->path;
            std::snprintf(mRename, sizeof(mRename), "%s", selected->name.c_str());
        }
        ImGui::SameLine();
        dangerButtonColors();
        if (ImGui::Button("Delete", ImVec2(buttonWidth, buttonHeight))) {
            std::scoped_lock lock(mMutex);
            mPendingDelete = selected->path;
        }
        ImGui::PopStyleColor(3);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Duration");
        ImGui::SameLine(132.0f * scale);
        ImGui::TextUnformatted(durationText(selected->durationTicks).c_str());
        ImGui::TextDisabled("File size");
        ImGui::SameLine(132.0f * scale);
        ImGui::TextUnformatted(sizeText(selected->size).c_str());
        ImGui::TextDisabled("Date");
        ImGui::SameLine(132.0f * scale);
        ImGui::TextUnformatted(modifiedText(selected->path).c_str());
        ImGui::TextDisabled("Replay file");
        ImGui::PushTextWrapPos();
        ImGui::TextWrapped("%s", selected->path.string().c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();

    std::filesystem::path pendingRename;
    std::filesystem::path pendingDelete;
    {
        std::scoped_lock lock(mMutex);
        pendingRename = mPendingRename;
        pendingDelete = mPendingDelete;
    }

    if (!pendingRename.empty()) ImGui::OpenPopup("Rename replay");
    const float popupWidth = std::max(1.0f, std::min(460.0f * scale, io.DisplaySize.x - 32.0f * scale));
    const float renameHeight = std::max(1.0f, std::min(165.0f * scale, io.DisplaySize.y - 24.0f * scale));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(popupWidth, renameHeight), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Rename replay", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ColorAccent), "RENAME REPLAY");
        ImGui::TextUnformatted("New replay name");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::InputText("##RenameValue", mRename, sizeof(mRename));
        if (ImGui::IsItemActivated() && !mKeyboardVisible.exchange(true, std::memory_order_acq_rel)) bedrockreplay::input::showKeyboard();
        const float width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Cancel", ImVec2(width, 36.0f * scale))) {
            std::scoped_lock lock(mMutex);
            mPendingRename.clear();
            if (mKeyboardVisible.exchange(false, std::memory_order_acq_rel)) bedrockreplay::input::hideKeyboard();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        accentButtonColors();
        if (ImGui::Button("Save name", ImVec2(width, 36.0f * scale))) {
            renameReplay(pendingRename, mRename);
            if (mKeyboardVisible.exchange(false, std::memory_order_acq_rel)) bedrockreplay::input::hideKeyboard();
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(4);
        ImGui::EndPopup();
    }

    if (!pendingDelete.empty()) ImGui::OpenPopup("Delete replay?");
    const float deleteHeight = std::max(1.0f, std::min(206.0f * scale, io.DisplaySize.y - 24.0f * scale));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(popupWidth, deleteHeight), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Delete replay?", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ColorDanger), "DELETE REPLAY");
        ImGui::TextUnformatted("This will remove the replay and its thumbnail.");
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(32, 29, 49, 255));
        ImGui::BeginChild("##DeleteName", ImVec2(0, 42.0f * scale), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(pendingDelete.filename().string().c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::TextDisabled("This action cannot be undone.");
        const float width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Cancel", ImVec2(width, 38.0f * scale))) {
            std::scoped_lock lock(mMutex);
            mPendingDelete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        dangerButtonColors();
        if (ImGui::Button("Delete replay", ImVec2(width, 38.0f * scale))) {
            deleteReplayFiles(pendingDelete);
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::EndPopup();
    }
    ImGui::End();
}

void ReplayUi::drawReplayControls() {
    if ((!mEditMode.load(std::memory_order_acquire) || !mAdvancedMode || mCameraPropertiesExpanded
        || mReplayControlsHidden.load(std::memory_order_acquire) || ReplayExporter::get().active())
        && mAdvancedSceneTexture) releaseAdvancedScene();
    mAdvancedInputActive.store(false, std::memory_order_release);
    {
        std::scoped_lock lock(mMutex);
        mAdvancedControlRects.clear();
    }
    const auto state = ReplayPlayer::get().state();
    const bool loadingReplay = state == ReplayPlayer::State::StartingWorld
        || state == ReplayPlayer::State::LoadingSnapshot;
    const auto exportState = ReplayExporter::get().state();
    const bool exporting = ReplayExporter::get().active();
    if ((exportState == ReplayExporter::State::Completed || exportState == ReplayExporter::State::Failed)
        && exportState != mLastExportState) {
        mOpenExportResultPopup = true;
    }
    mLastExportState = exportState;
    if (!activeReplayState(state) || browserVisible()
        || (state != ReplayPlayer::State::Failed && !loadingReplay && !exporting && !replayGameplayVisible())) {
        mModalVisible.store(false, std::memory_order_release);
        std::scoped_lock lock(mMutex);
        mControlMinX = mControlMinY = mControlMaxX = mControlMaxY = 0.0f;
        mReplayUiPointer = -1;
        return;
    }

    auto& io = ImGui::GetIO();
    const float scale = uiScale();
    if (state == ReplayPlayer::State::ResettingSnapshot) {
        auto* draw = ImGui::GetForegroundDrawList();
        const char* label = "Seeking replay";
        const auto labelSize = ImGui::CalcTextSize(label);
        const float width = labelSize.x + 62.0f * scale;
        const float height = 42.0f * scale;
        const ImVec2 minimum((io.DisplaySize.x - width) * 0.5f, 18.0f * scale);
        const ImVec2 maximum(minimum.x + width, minimum.y + height);
        draw->AddRectFilled(minimum, maximum, IM_COL32(17, 23, 39, 245), 12.0f * scale);
        draw->AddRect(minimum, maximum, ColorBorder, 12.0f * scale, 0, 1.0f * scale);
        const ImVec2 center(minimum.x + 22.0f * scale, minimum.y + height * 0.5f);
        const float phase = static_cast<float>(ImGui::GetTime() * 4.0);
        for (int index = 0; index < 8; ++index) {
            const float angle = phase + static_cast<float>(index) * 0.78539816f;
            const int alpha = 70 + index * 23;
            draw->AddCircleFilled(
                ImVec2(center.x + std::cos(angle) * 8.0f * scale, center.y + std::sin(angle) * 8.0f * scale),
                1.7f * scale,
                IM_COL32(159, 121, 250, alpha)
            );
        }
        draw->AddText(
            ImVec2(minimum.x + 43.0f * scale, minimum.y + (height - labelSize.y) * 0.5f),
            ColorText,
            label
        );
        mModalVisible.store(false, std::memory_order_release);
        std::scoped_lock lock(mMutex);
        mControlMinX = mControlMinY = mControlMaxX = mControlMaxY = 0.0f;
        mReplayUiPointer = -1;
        return;
    }

    if (loadingReplay && !exporting) {
        auto& player = ReplayPlayer::get();
        const float progress = std::clamp(player.preparationProgress(), 0.0f, 1.0f);
        const std::string status = player.status();
        const auto replayPath = player.replayPath();
        const std::string replayName = replayPath.empty() ? "Replay world" : replayPath.stem().string();
        const bool editor = mEditMode.load(std::memory_order_acquire);
        const ImVec2 display = io.DisplaySize;
        const bool compact = display.x < 950.0f || display.y < 440.0f;
        const float margin = std::clamp(display.x * 0.047f, 20.0f * scale, 68.0f * scale);
        const float headerHeight = std::max(56.0f * scale, display.y * 0.087f);
        const float cardHeight = std::clamp(display.y * 0.235f, 88.0f * scale, 180.0f * scale);
        const float cardTop = display.y - margin * 0.72f - cardHeight;
        const float heroTop = headerHeight + (cardTop - headerHeight) * 0.25f;
        auto* font = ImGui::GetFont();

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(display, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        constexpr ImGuiWindowFlags loadingFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs;
        ImGui::Begin("##ReplayWorldPreparation", nullptr, loadingFlags);
        auto* draw = ImGui::GetWindowDrawList();

        const auto background = loadingBackgroundTexture();
        if (background) {
            const float sourceAspect = static_cast<float>(loadingBackground::Width) / loadingBackground::Height;
            const float screenAspect = display.x / display.y;
            ImVec2 uvMin(0, 0), uvMax(1, 1);
            if (sourceAspect > screenAspect) {
                uvMin.x = (1.0f - screenAspect / sourceAspect) * 0.5f;
                uvMax.x = 1.0f - uvMin.x;
            } else {
                uvMin.y = (1.0f - sourceAspect / screenAspect) * 0.5f;
                uvMax.y = 1.0f - uvMin.y;
            }
            draw->AddImage(reinterpret_cast<ImTextureID>(static_cast<std::intptr_t>(background)),
                ImVec2(0, 0), display, uvMin, uvMax);
        } else {
            draw->AddRectFilled(ImVec2(0, 0), display, ColorBackground);
        }
        draw->AddRectFilledMultiColor(ImVec2(0, headerHeight), display,
            IM_COL32(5, 8, 28, 190), IM_COL32(7, 8, 28, 25),
            IM_COL32(5, 7, 24, 95), IM_COL32(4, 6, 22, 205));
        draw->AddRectFilledMultiColor(ImVec2(0, display.y * 0.62f), display,
            IM_COL32(5, 7, 25, 0), IM_COL32(7, 6, 27, 0),
            IM_COL32(6, 7, 27, 190), IM_COL32(5, 7, 26, 190));
        draw->AddRectFilled(ImVec2(0, 0), ImVec2(display.x, headerHeight),
            IM_COL32(7, 10, 28, 222));
        draw->AddLine(ImVec2(0, headerHeight), ImVec2(display.x, headerHeight),
            IM_COL32(121, 91, 190, 95), 1.0f);
        draw->AddRectFilled(ImVec2(margin, headerHeight * 0.27f),
            ImVec2(margin + 5.0f * scale, headerHeight * 0.73f), ColorAccent, 3.0f * scale);
        const float brandSize = 25.0f * scale;
        const ImVec2 brandPos(margin + 24.0f * scale, (headerHeight - brandSize) * 0.5f);
        draw->AddText(font, brandSize, brandPos, ColorText, "BEDROCK");
        const float brandFirstWidth = font->CalcTextSizeA(brandSize, 1000.0f, 0.0f, "BEDROCK").x;
        draw->AddText(font, brandSize, ImVec2(brandPos.x + brandFirstWidth, brandPos.y),
            ColorAccent, "REPLAY");
        const char* mode = editor ? "EDITOR / WORLD PREPARATION" : "PLAYER / WORLD PREPARATION";
        const float modeSize = 16.0f * scale;
        const float modeWidth = font->CalcTextSizeA(modeSize, 1000.0f, 0.0f, mode).x;
        const float brandWidth = brandFirstWidth + font->CalcTextSizeA(brandSize, 1000.0f, 0.0f, "REPLAY").x;
        if (display.x - margin - modeWidth > brandPos.x + brandWidth + 18.0f * scale) {
            draw->AddText(font, modeSize,
                ImVec2(display.x - margin - modeWidth, (headerHeight - modeSize) * 0.5f),
                ColorTextDim, mode);
        }

        const ImVec2 ringCenter(display.x * (compact ? 0.77f : 0.765f),
            headerHeight + (cardTop - headerHeight) * 0.49f);
        const float ringRadius = std::min({display.y * 0.216f, display.x * 0.17f,
            (cardTop - headerHeight) * 0.36f});
        draw->AddCircleFilled(ringCenter, ringRadius * 0.90f, IM_COL32(12, 16, 42, 230));
        draw->AddCircle(ringCenter, ringRadius * 1.11f, IM_COL32(110, 103, 191, 90), 0, 1.5f * scale);
        draw->AddCircle(ringCenter, ringRadius, IM_COL32(62, 71, 125, 215), 0, 6.0f * scale);
        const auto arc = [&](float from, float to, ImU32 color, ImU32 glow) {
            if (to <= from) return;
            constexpr float turn = 6.283185307f;
            const float start = -1.570796327f + from * turn;
            const float end = -1.570796327f + to * turn;
            const int steps = std::max(2, static_cast<int>((to - from) * 120.0f));
            draw->PathArcTo(ringCenter, ringRadius, start, end, steps);
            draw->PathStroke(glow, 0, 15.0f * scale);
            draw->PathArcTo(ringCenter, ringRadius, start, end, steps);
            draw->PathStroke(color, 0, 6.0f * scale);
        };
        arc(0.0f, std::min(progress, 0.5f), ColorCyan, IM_COL32(96, 157, 247, 65));
        arc(0.5f, progress, ColorAccent, IM_COL32(159, 121, 250, 75));
        if (const auto texture = logoTexture()) {
            const float logoSide = ringRadius * 1.41f;
            draw->AddImage(reinterpret_cast<ImTextureID>(static_cast<std::intptr_t>(texture)),
                ImVec2(ringCenter.x - logoSide * 0.5f, ringCenter.y - logoSide * 0.5f),
                ImVec2(ringCenter.x + logoSide * 0.5f, ringCenter.y + logoSide * 0.5f));
        }

        const float leftRight = std::max(margin + 1.0f,
            ringCenter.x - ringRadius * 1.17f - 20.0f * scale);
        const ImVec4 leftClip(margin, heroTop, leftRight, cardTop - 5.0f * scale);
        draw->AddText(font, 21.0f * scale, ImVec2(margin, heroTop), ColorCyan,
            editor ? "REPLAY EDITOR" : "REPLAY PLAYER");
        const float titleSize = (compact ? 34.0f : 43.0f) * scale;
        const ImVec2 titlePos(margin, heroTop + 32.0f * scale);
        draw->AddText(font, titleSize, titlePos, ColorText,
            "Preparing your ", nullptr, 0.0f, &leftClip);
        const float titleLead = font->CalcTextSizeA(titleSize, 10000.0f, 0.0f, "Preparing your ").x;
        draw->AddText(font, titleSize, ImVec2(titlePos.x + titleLead, titlePos.y),
            ColorAccent, "replay", nullptr, 0.0f, &leftClip);
        const float accentY = heroTop + 85.0f * scale;
        draw->AddRectFilledMultiColor(ImVec2(margin, accentY),
            ImVec2(margin + 64.0f * scale, accentY + 4.0f * scale),
            ColorCyan, ColorAccent, ColorAccent, ColorCyan);

        const float worldY = heroTop + 108.0f * scale;
        const float iconSide = 42.0f * scale;
        draw->AddRectFilled(ImVec2(margin, worldY),
            ImVec2(margin + iconSide, worldY + iconSide), IM_COL32(35, 34, 75, 225), 7.0f * scale);
        const ImVec2 cube(margin + iconSide * 0.5f, worldY + iconSide * 0.5f);
        const float cr = 10.0f * scale;
        const ImU32 cubeColor = IM_COL32(147, 166, 255, 255);
        draw->AddLine(ImVec2(cube.x, cube.y - cr), ImVec2(cube.x - cr, cube.y - cr * 0.48f), cubeColor, 1.7f * scale);
        draw->AddLine(ImVec2(cube.x, cube.y - cr), ImVec2(cube.x + cr, cube.y - cr * 0.48f), cubeColor, 1.7f * scale);
        draw->AddLine(ImVec2(cube.x - cr, cube.y - cr * 0.48f), ImVec2(cube.x, cube.y + cr * 0.10f), cubeColor, 1.7f * scale);
        draw->AddLine(ImVec2(cube.x + cr, cube.y - cr * 0.48f), ImVec2(cube.x, cube.y + cr * 0.10f), cubeColor, 1.7f * scale);
        draw->AddLine(ImVec2(cube.x - cr, cube.y - cr * 0.48f), ImVec2(cube.x - cr, cube.y + cr * 0.55f), cubeColor, 1.7f * scale);
        draw->AddLine(ImVec2(cube.x + cr, cube.y - cr * 0.48f), ImVec2(cube.x + cr, cube.y + cr * 0.55f), cubeColor, 1.7f * scale);
        draw->AddLine(ImVec2(cube.x - cr, cube.y + cr * 0.55f), ImVec2(cube.x, cube.y + cr), cubeColor, 1.7f * scale);
        draw->AddLine(ImVec2(cube.x + cr, cube.y + cr * 0.55f), ImVec2(cube.x, cube.y + cr), cubeColor, 1.7f * scale);
        draw->AddLine(ImVec2(cube.x, cube.y + cr * 0.10f), ImVec2(cube.x, cube.y + cr), cubeColor, 1.7f * scale);
        const float worldTextX = margin + iconSide + 15.0f * scale;
        draw->AddText(font, 20.0f * scale, ImVec2(worldTextX, worldY - 2.0f * scale),
            ColorText, "OPENING WORLD");
        draw->PushClipRect(ImVec2(worldTextX, worldY + 22.0f * scale),
            ImVec2(leftRight, worldY + 44.0f * scale), true);
        draw->AddText(ImVec2(worldTextX, worldY + 22.0f * scale), ColorTextDim, replayName.c_str());
        draw->PopClipRect();

        const ImVec2 cardMin(margin, cardTop);
        const ImVec2 cardMax(display.x - margin, cardTop + cardHeight);
        draw->AddRectFilled(cardMin, cardMax, IM_COL32(10, 14, 39, 237), 13.0f * scale);
        draw->AddRect(cardMin, cardMax, IM_COL32(77, 100, 187, 195), 13.0f * scale, 0, 1.0f * scale);
        const float cardPad = 27.0f * scale;
        const float statusX = cardMin.x + cardPad + 41.0f * scale;
        const float labelY = cardTop + cardHeight * 0.16f;
        draw->AddCircle(ImVec2(cardMin.x + cardPad + 10.0f * scale, labelY + 10.0f * scale),
            7.0f * scale, ColorCyan, 0, 2.0f * scale);
        draw->AddCircleFilled(ImVec2(cardMin.x + cardPad + 10.0f * scale, labelY + 10.0f * scale),
            2.0f * scale, ColorAccent);
        draw->AddText(ImVec2(statusX, labelY), ColorText, "PREPARING WORLD");
        char percentage[16]{};
        std::snprintf(percentage, sizeof(percentage), "%d%%", static_cast<int>(progress * 100.0f));
        const float percentageSize = 32.0f * scale;
        const float percentageWidth = font->CalcTextSizeA(percentageSize, 1000.0f, 0.0f, percentage).x;
        draw->AddText(font, percentageSize,
            ImVec2(cardMax.x - cardPad - percentageWidth, cardTop + cardHeight * 0.17f),
            ColorText, percentage);
        const float stateY = cardTop + cardHeight * 0.37f;
        draw->PushClipRect(ImVec2(statusX, stateY),
            ImVec2(cardMax.x - cardPad - percentageWidth - 12.0f * scale,
                stateY + 23.0f * scale), true);
        draw->AddText(ImVec2(statusX, stateY), ColorTextDim,
            status.empty() ? "Waiting for replay world" : status.c_str());
        draw->PopClipRect();
        const float barLeft = cardMin.x + cardPad;
        const float barRight = cardMax.x - cardPad;
        const float barTop = cardTop + cardHeight * 0.61f;
        const float barRadius = 7.0f * scale;
        draw->AddRectFilled(ImVec2(barLeft, barTop), ImVec2(barRight, barTop + 2.0f * barRadius),
            IM_COL32(44, 47, 88, 255), barRadius);
        if (progress > 0.0f) {
            const float filled = (barRight - barLeft - 2.0f * barRadius) * progress;
            const float start = barLeft + barRadius;
            const float end = start + filled;
            draw->AddCircleFilled(ImVec2(start, barTop + barRadius), barRadius, ColorCyan);
            if (filled > 0.0f) {
                draw->AddRectFilledMultiColor(ImVec2(start, barTop),
                    ImVec2(end, barTop + 2.0f * barRadius),
                    ColorCyan, ColorAccent, ColorAccent, ColorCyan);
                draw->AddCircleFilled(ImVec2(end, barTop + barRadius), barRadius, ColorAccent);
            }
        }
        const float footerY = cardTop + cardHeight * 0.81f;
        draw->AddCircle(ImVec2(barLeft + 6.0f * scale, footerY + 6.0f * scale),
            6.0f * scale, ColorTextDim, 0, 1.3f * scale);
        draw->AddText(font, 11.0f * scale,
            ImVec2(barLeft + 4.8f * scale, footerY + 0.3f * scale), ColorTextDim, "i");
        draw->AddText(ImVec2(barLeft + 21.0f * scale, footerY), ColorTextDim,
            "Your replay will open when the world is ready.");
        ImGui::End();
        ImGui::PopStyleVar(3);
        mModalVisible.store(true, std::memory_order_release);
        std::scoped_lock lock(mMutex);
        mControlMinX = mControlMinY = 0.0f;
        mControlMaxX = display.x;
        mControlMaxY = display.y;
        return;
    }

    if (loadingReplay || exporting) {
        const bool preparingEditor = mEditMode.load(std::memory_order_acquire);
        float progress = ReplayPlayer::get().preparationProgress();
        const char* badge = preparingEditor ? "REPLAY EDITOR" : "REPLAY PLAYER";
        const char* title = preparingEditor ? "Opening replay editor" : "Opening replay";
        const char* subtitle = "Loading terrain, entities and lighting";
        std::string status = ReplayPlayer::get().status();
        if (exporting) {
            badge = "VIDEO EXPORT";
            status = ReplayExporter::get().status();
            if (exportState == ReplayExporter::State::Exporting) {
                title = "Exporting replay";
                subtitle = "Rendering clean MP4 frames";
                progress = ReplayExporter::get().progress();
            } else if (exportState == ReplayExporter::State::Finishing) {
                title = "Finishing video";
                subtitle = "Writing the final MP4 file";
                progress = 1.0f;
            } else if (exportState == ReplayExporter::State::Cancelling) {
                title = "Cancelling export";
                subtitle = "Cleaning up the unfinished video";
            } else {
                title = "Preparing video export";
                subtitle = "Restoring the replay before capture starts";
            }
        }
        progress = std::clamp(progress, 0.0f, 1.0f);

        const float overlayOverscan = 6.0f * scale;
        ImGui::SetNextWindowPos(ImVec2(-overlayOverscan, -overlayOverscan), ImGuiCond_Always);
        ImGui::SetNextWindowSize(
            ImVec2(io.DisplaySize.x + overlayOverscan * 2.0f, io.DisplaySize.y + overlayOverscan * 2.0f),
            ImGuiCond_Always
        );
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        const auto overlayFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;
        ImGui::Begin("##ReplayLoadingScreen", nullptr, overlayFlags);
        auto* draw = ImGui::GetWindowDrawList();
        if (!exporting) {
            draw->AddRectFilledMultiColor(
                ImVec2(-overlayOverscan, -overlayOverscan),
                ImVec2(io.DisplaySize.x + overlayOverscan, io.DisplaySize.y + overlayOverscan),
                IM_COL32(8, 11, 25, 255),
                IM_COL32(20, 24, 50, 255),
                IM_COL32(18, 30, 62, 255),
                IM_COL32(14, 16, 36, 255)
            );
            for (int line = 1; line < 8; ++line) {
                const float y = io.DisplaySize.y * static_cast<float>(line) / 8.0f;
                draw->AddLine(
                    ImVec2(-overlayOverscan, y),
                    ImVec2(io.DisplaySize.x + overlayOverscan, y),
                    IM_COL32(96, 157, 247, 20),
                    1.0f
                );
            }
        }
        const float cardWidth = exporting
            ? std::max(1.0f, std::min(io.DisplaySize.x - 28.0f * scale, 1040.0f * scale))
            : std::max(1.0f, std::min(580.0f * scale, io.DisplaySize.x - 40.0f * scale));
        const float cardHeight = exporting
            ? std::min(174.0f * scale, std::max(112.0f * scale, io.DisplaySize.y - 96.0f * scale))
            : std::max(1.0f, std::min(252.0f * scale, io.DisplaySize.y - 40.0f * scale));
        const ImVec2 cardMin(
            (io.DisplaySize.x - cardWidth) * 0.5f,
            exporting ? io.DisplaySize.y - cardHeight - 14.0f * scale : (io.DisplaySize.y - cardHeight) * 0.5f
        );
        const ImVec2 cardMax(cardMin.x + cardWidth, cardMin.y + cardHeight);
        draw->AddRect(
            ImVec2(cardMin.x - 5.0f * scale, cardMin.y - 5.0f * scale),
            ImVec2(cardMax.x + 5.0f * scale, cardMax.y + 5.0f * scale),
            IM_COL32(159, 121, 250, 24),
            22.0f * scale,
            0,
            5.0f * scale
        );
        draw->AddRect(
            ImVec2(cardMin.x - 2.0f * scale, cardMin.y - 2.0f * scale),
            ImVec2(cardMax.x + 2.0f * scale, cardMax.y + 2.0f * scale),
            IM_COL32(159, 121, 250, 55),
            20.0f * scale,
            0,
            2.0f * scale
        );
        draw->AddRectFilled(cardMin, cardMax, IM_COL32(17, 22, 39, 250), 18.0f * scale);
        draw->AddRect(cardMin, cardMax, IM_COL32(88, 76, 141, 210), 18.0f * scale, 0, 1.2f * scale);
        draw->AddRectFilled(
            ImVec2(cardMin.x + 24.0f * scale, cardMin.y + 1.0f * scale),
            ImVec2(cardMax.x - 24.0f * scale, cardMin.y + 4.0f * scale),
            ColorAccent,
            3.0f * scale
        );
        const auto badgeSize = ImGui::CalcTextSize(badge);
        const ImVec2 badgeMin(
            exporting ? cardMin.x + 18.0f * scale : cardMin.x + (cardWidth - badgeSize.x - 20.0f * scale) * 0.5f,
            cardMin.y + (exporting ? 15.0f : 21.0f) * scale
        );
        const ImVec2 badgeMax(
            badgeMin.x + badgeSize.x + 20.0f * scale,
            badgeMin.y + badgeSize.y + 8.0f * scale
        );
        draw->AddRectFilled(badgeMin, badgeMax, IM_COL32(56, 42, 91, 255), 10.0f * scale);
        draw->AddRect(badgeMin, badgeMax, IM_COL32(159, 121, 250, 120), 10.0f * scale);
        draw->AddText(ImVec2(badgeMin.x + 10.0f * scale, badgeMin.y + 4.0f * scale), ColorAccent, badge);

        if (!exporting) {
            draw->AddRectFilled(ImVec2(cardMin.x + cardWidth * 0.5f - 25.0f * scale, cardMin.y + 77.0f * scale),
                ImVec2(cardMin.x + cardWidth * 0.5f + 25.0f * scale, cardMin.y + 81.0f * scale), ColorCyan, 2.0f * scale);
        }
        const auto titleSize = ImGui::CalcTextSize(title);
        const auto subtitleSize = ImGui::CalcTextSize(subtitle);
        draw->AddText(
            ImVec2(
                exporting ? cardMin.x + 64.0f * scale : cardMin.x + (cardWidth - titleSize.x) * 0.5f,
                cardMin.y + (exporting ? 54.0f : 119.0f) * scale
            ),
            ColorText,
            title
        );
        draw->AddText(
            ImVec2(
                exporting ? cardMin.x + 64.0f * scale : cardMin.x + (cardWidth - subtitleSize.x) * 0.5f,
                cardMin.y + (exporting ? 82.0f : 146.0f) * scale
            ),
            ColorTextDim,
            subtitle
        );
        char progressText[16]{};
        std::snprintf(progressText, sizeof(progressText), "%d%%", static_cast<int>(progress * 100.0f));
        const auto progressSize = ImGui::CalcTextSize(progressText);
        const float barLeft = cardMin.x + (exporting ? 18.0f : 34.0f) * scale;
        const float barRight = cardMax.x - (exporting ? 18.0f : 34.0f) * scale;
        const float barTop = cardMin.y + (exporting ? 145.0f : 205.0f) * scale;
        draw->PushClipRect(ImVec2(barLeft, barTop - 25.0f * scale),
            ImVec2(barRight - progressSize.x - 12.0f * scale, barTop), true);
        draw->AddText(ImVec2(barLeft, barTop - 25.0f * scale), ColorTextDim, status.c_str());
        draw->PopClipRect();
        draw->AddText(ImVec2(barRight - progressSize.x, barTop - 25.0f * scale), ColorAccent, progressText);
        draw->AddRectFilled(
            ImVec2(barLeft, barTop),
            ImVec2(barRight, barTop + 14.0f * scale),
            IM_COL32(35, 42, 67, 255),
            7.0f * scale
        );
        if (progress > 0.0f) {
            const float fillRight = barLeft + (barRight - barLeft) * progress;
            draw->AddRectFilled(
                ImVec2(barLeft, barTop),
                ImVec2(fillRight, barTop + 14.0f * scale),
                ColorAccent,
                7.0f * scale
            );
            draw->AddRectFilled(ImVec2(barLeft, barTop + 10.0f * scale),
                ImVec2(fillRight, barTop + 14.0f * scale), ColorCyan, 2.0f * scale);
        }
        if (exporting) {
            const float buttonWidth = 168.0f * scale;
            const float buttonHeight = 32.0f * scale;
            ImGui::SetCursorScreenPos(ImVec2(cardMax.x - buttonWidth - 18.0f * scale, cardMin.y + 14.0f * scale));
            dangerButtonColors();
            if (ImGui::Button("Cancel export##Loading", ImVec2(buttonWidth, buttonHeight))) {
                ReplayExporter::get().cancel();
            }
            ImGui::PopStyleColor(3);
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
        mModalVisible.store(true, std::memory_order_release);
        std::scoped_lock lock(mMutex);
        mControlMinX = 0.0f;
        mControlMinY = 0.0f;
        mControlMaxX = io.DisplaySize.x;
        mControlMaxY = io.DisplaySize.y;
        return;
    }

    const bool editing = mEditMode.load(std::memory_order_acquire);
    if (editing && state == ReplayPlayer::State::Playing
        && mEditAutoPausePending.exchange(false, std::memory_order_acq_rel)) {
        ReplayPlayer::get().pause();
    }
    mModalVisible.store(false, std::memory_order_release);

    if (mReplayControlsHidden.load(std::memory_order_acquire)) {
        const char* restoreLabel = mEditMode.load(std::memory_order_acquire) ? "Show Editor" : "Show Replay UI";
        const float buttonWidth = (mEditMode.load(std::memory_order_acquire) ? 132.0f : 154.0f) * scale;
        const float buttonHeight = 38.0f * scale;
        const ImVec2 restoreSize(buttonWidth + 12.0f * scale, buttonHeight + 12.0f * scale);
        const ImVec2 restorePosition(
            (io.DisplaySize.x - restoreSize.x) * 0.5f,
            io.DisplaySize.y - restoreSize.y - 12.0f * scale
        );
        ImGui::SetNextWindowPos(restorePosition, ImGuiCond_Always);
        ImGui::SetNextWindowSize(restoreSize, ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(14, 20, 34, 238));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(77, 77, 119, 230));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * scale, 6.0f * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f * scale);
        const auto restoreFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoScrollbar;
        if (ImGui::Begin("##ReplayControlsRestore", nullptr, restoreFlags)) {
            accentButtonColors();
            if (ImGui::Button(restoreLabel, ImVec2(buttonWidth, buttonHeight))) {
                mReplayControlsHidden.store(false, std::memory_order_release);
            }
            ImGui::PopStyleColor(4);
            const ImVec2 minimum = ImGui::GetWindowPos();
            const ImVec2 maximum(minimum.x + ImGui::GetWindowSize().x, minimum.y + ImGui::GetWindowSize().y);
            std::scoped_lock lock(mMutex);
            mControlMinX = minimum.x;
            mControlMinY = minimum.y;
            mControlMaxX = maximum.x;
            mControlMaxY = maximum.y;
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        if (editing || mModalVisible.load(std::memory_order_acquire) || mOpenExportPopup) drawExportPopup();
        drawExportResultPopup();
        return;
    }

    auto selectedEditorKeyframe = editing
        ? CinematicCamera::get().selectedIndex()
        : std::optional<std::size_t>{};
    if (!selectedEditorKeyframe) mCameraPropertiesExpanded = false;
    const bool keyframeEditor = editing && mCameraPropertiesExpanded && selectedEditorKeyframe.has_value();
    if (editing && mAdvancedMode && !keyframeEditor && state != ReplayPlayer::State::Failed) {
        drawAdvancedControls();
        drawExportPopup();
        drawExportResultPopup();
        return;
    }
    if (editing && state != ReplayPlayer::State::Failed)
        drawCameraMarkers(ImGui::GetBackgroundDrawList(), 0, 0, io.DisplaySize.x, io.DisplaySize.y,
            io.DisplaySize.x / std::max(1.f, io.DisplaySize.y));
    const float panelWidth = std::min(
        io.DisplaySize.x - 24.0f * scale,
        (editing ? 980.0f : 1040.0f) * scale
    );
    const float estimatedDeckWidth = panelWidth - 30.0f * scale;
    const float estimatedCardWidth = (estimatedDeckWidth - 8.0f * scale) * 0.5f;
    const float estimatedCameraActionWidth = (estimatedCardWidth - 29.0f * scale) * 0.5f;
    const float requiredCameraActionWidth = std::max(
        ImGui::CalcTextSize("Delete Keyframe").x,
        ImGui::CalcTextSize("Delete Time Keyframe").x
    )
        + ImGui::GetStyle().FramePadding.x * 2.0f;
    const bool landscapeEditor = editing && !keyframeEditor
        && io.DisplaySize.x >= io.DisplaySize.y * 1.2f
        && panelWidth >= 580.0f
        && estimatedCameraActionWidth >= requiredCameraActionWidth;
    const float requestedHeight = state == ReplayPlayer::State::Failed ? 104.0f
        : editing ? (keyframeEditor ? 450.0f : landscapeEditor ? 350.0f : 510.0f) : 260.0f;
    const float preservedGameplayHeight = editing ? 92.0f * scale : 96.0f * scale;
    const float heightLimit = std::max(112.0f * scale, io.DisplaySize.y - preservedGameplayHeight);
    const float panelHeight = std::min(requestedHeight * scale, heightLimit);
    const ImVec2 size(panelWidth, panelHeight);
    const ImVec2 position((io.DisplaySize.x - panelWidth) * 0.5f, io.DisplaySize.y - panelHeight - 14.0f * scale);
    ImGui::SetNextWindowPos(position, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(14, 20, 34, 246));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(77, 77, 119, 230));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 15.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15.0f * scale, 12.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(9.0f * scale, 7.0f * scale));
    auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    if (!keyframeEditor && (!editing || landscapeEditor) && panelHeight >= requestedHeight * scale)
        flags |= ImGuiWindowFlags_NoScrollbar;

    if (ImGui::Begin("##ReplayControls", nullptr, flags)) {
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 windowMinimum = ImGui::GetWindowPos();
        const ImVec2 windowMaximum(windowMinimum.x + ImGui::GetWindowSize().x, windowMinimum.y + ImGui::GetWindowSize().y);
        draw->AddRectFilled(
            ImVec2(windowMinimum.x + 24.0f * scale, windowMinimum.y),
            ImVec2(windowMaximum.x - 24.0f * scale, windowMinimum.y + 3.0f * scale),
            ColorAccent,
            2.0f * scale
        );
        const auto current = ReplayPlayer::get().currentTick();
        const auto duration = ReplayPlayer::get().durationTicks();
        const std::string time = durationText(current) + " / " + durationText(duration);
        const char* panelTitle = keyframeEditor ? "KEYFRAME EDITOR" : editing ? "BASIC EDITOR" : "REPLAY PLAYER";
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ColorAccent), "%s", panelTitle);
        const float hideWidth = 64.0f * scale;
        const float exitWidth = 72.0f * scale;
        const float timeWidth = ImGui::CalcTextSize(time.c_str()).x;
        const float exitStart = ImGui::GetWindowContentRegionMax().x - exitWidth;
        const float hideStart = exitStart - hideWidth - 8.0f * scale;
        const float timeStart = hideStart - timeWidth - 14.0f * scale;
        if (timeStart >= ImGui::GetCursorPosX() + 8.0f * scale) {
            ImGui::SameLine(timeStart);
            ImGui::TextDisabled("%s", time.c_str());
        }
        ImGui::SameLine(hideStart);
        if (ImGui::Button("Hide", ImVec2(hideWidth, 30.0f * scale))) {
            mReplayControlsHidden.store(true, std::memory_order_release);
        }
        ImGui::SameLine(exitStart);
        dangerButtonColors();
        if (ImGui::Button("Close", ImVec2(exitWidth, 30.0f * scale))) {
            CinematicCamera::get().endSession();
            mCameraPropertiesExpanded = false;
            mEditMode.store(false, std::memory_order_release);
            ReplayPlayer::get().stop();
        }
        ImGui::PopStyleColor(3);

        if (editing && !keyframeEditor && state != ReplayPlayer::State::Failed) {
            cyanButtonColors();
            if (ImGui::Button("Advanced editor", ImVec2(164.0f * scale, 32.0f * scale))) {
                mAdvancedMode = true;
                mAdvancedInputActive.store(true, std::memory_order_release);
                mAdvancedToolsOpen = true;
                mAdvancedScrubTrack = -1;
            }
            ImGui::PopStyleColor(4);
        }

        if (state == ReplayPlayer::State::Failed) {
            ImGui::Spacing();
            ImGui::TextWrapped("%s", ReplayPlayer::get().status().c_str());
        } else {
            auto& camera = CinematicCamera::get();
            auto keyframes = editing ? camera.keyframes() : std::vector<CameraKeyframe>{};
            auto selectedIndex = editing ? camera.selectedIndex() : std::optional<std::size_t>{};
            auto timeKeyframes = editing ? camera.timeKeyframes() : std::vector<std::uint32_t>{};
            auto selectedTimeKeyframe = editing
                ? camera.selectedTimeKeyframeIndex()
                : std::optional<std::size_t>{};

            if (keyframeEditor && selectedIndex && *selectedIndex < keyframes.size()) {
                auto keyframe = keyframes[*selectedIndex];
                const ImVec2 cardMinimum = ImGui::GetCursorScreenPos();
                const ImVec2 cardSize(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
                const ImVec2 cardMaximum(cardMinimum.x + cardSize.x, cardMinimum.y + cardSize.y);
                draw->AddRectFilled(cardMinimum, cardMaximum, IM_COL32(22, 28, 46, 245), 12.0f * scale);
                draw->AddRect(cardMinimum, cardMaximum, IM_COL32(77, 77, 119, 225), 12.0f * scale);
                const float padding = 12.0f * scale;
                ImGui::SetCursorScreenPos(ImVec2(cardMinimum.x + padding, cardMinimum.y + padding));
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ColorCyan), "CAMERA KEY %zu OF %zu", *selectedIndex + 1, keyframes.size());
                const float backWidth = 96.0f * scale;
                ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - backWidth));
                if (ImGui::Button("Back", ImVec2(backWidth, 32.0f * scale))) mCameraPropertiesExpanded = false;
                ImGui::TextDisabled("Camera values at %s", durationText(keyframe.tick).c_str());
                ImGui::Separator();

                bool changed = false;
                ImGui::PushID(static_cast<int>(*selectedIndex));
                const float available = cardSize.x - padding * 2.0f;
                const float gap = ImGui::GetStyle().ItemSpacing.x;
                const float columnWidth = (available - gap) * 0.5f;
                int tickValue = static_cast<int>(std::min<std::uint32_t>(
                    keyframe.tick, static_cast<std::uint32_t>(std::numeric_limits<int>::max())
                ));
                const int maximumTick = static_cast<int>(std::min<std::uint32_t>(
                    duration, static_cast<std::uint32_t>(std::numeric_limits<int>::max())
                ));

                ImGui::BeginGroup();
                ImGui::TextDisabled("REPLAY TICK");
                ImGui::SetNextItemWidth(columnWidth);
                if (ImGui::SliderInt("##KeyframeTick", &tickValue, 0, std::max(0, maximumTick))) {
                    keyframe.tick = static_cast<std::uint32_t>(std::max(0, tickValue));
                    changed = true;
                }
                ImGui::EndGroup();
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::TextDisabled("FIELD OF VIEW");
                ImGui::SetNextItemWidth(columnWidth);
                changed |= ImGui::DragFloat("##KeyframeFov", &keyframe.fov, 0.25f, 1.0f, 179.0f, "%.1f");
                ImGui::EndGroup();

                ImGui::BeginGroup();
                ImGui::TextDisabled("DIMENSION");
                ImGui::SetNextItemWidth(columnWidth);
                changed |= ImGui::InputInt("##KeyframeDimension", &keyframe.dimension, 1, 1);
                ImGui::EndGroup();
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::TextDisabled("INTERPOLATION");
                int interpolation = static_cast<int>(keyframe.interpolation);
                const char* interpolationNames[] = {"Linear", "Smooth", "Ease", "Hold", "Hermite", "Cubic Bezier"};
                ImGui::SetNextItemWidth(columnWidth);
                if (ImGui::Combo("##KeyframeCurve", &interpolation, interpolationNames, IM_ARRAYSIZE(interpolationNames))) {
                    keyframe.interpolation = static_cast<CameraInterpolation>(interpolation);
                    changed = true;
                }
                ImGui::EndGroup();

                ImGui::TextDisabled("POSITION  X / Y / Z");
                ImGui::SetNextItemWidth(available);
                changed |= ImGui::DragFloat3("##KeyframePosition", &keyframe.position.x, 0.05f, 0.0f, 0.0f, "%.2f");
                ImGui::TextDisabled("ROTATION  YAW / PITCH / ROLL");
                ImGui::SetNextItemWidth(available);
                changed |= ImGui::DragFloat3("##KeyframeRotation", &keyframe.yaw, 0.25f, 0.0f, 0.0f, "%.1f");
                if (keyframe.interpolation == CameraInterpolation::CubicBezier) {
                    ImGui::BeginGroup();
                    ImGui::TextDisabled("BEZIER START");
                    ImGui::SetNextItemWidth(columnWidth);
                    changed |= ImGui::DragFloat2("##BezierStart", &keyframe.bezierControl1.x, 0.01f, -2.0f, 2.0f, "%.2f");
                    ImGui::EndGroup();
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    ImGui::TextDisabled("BEZIER END");
                    ImGui::SetNextItemWidth(columnWidth);
                    changed |= ImGui::DragFloat2("##BezierEnd", &keyframe.bezierControl2.x, 0.01f, -2.0f, 2.0f, "%.2f");
                    ImGui::EndGroup();
                }
                ImGui::TextDisabled("Changes are saved to the camera path automatically.");
                if (changed) camera.updateSelected(keyframe);
                ImGui::PopID();
            } else {
                const float cardGap = 8.0f * scale;
                const float cardPadding = 10.0f * scale;
                const float deckWidth = ImGui::GetContentRegionAvail().x;
                const float cardWidth = landscapeEditor ? (deckWidth - cardGap) * 0.5f : deckWidth;
                const float playbackHeight = (landscapeEditor ? 220.0f : editing ? 222.0f : 180.0f) * scale;
                const ImVec2 playbackMinimum = ImGui::GetCursorScreenPos();
                const ImVec2 playbackMaximum(
                    playbackMinimum.x + cardWidth,
                    playbackMinimum.y + playbackHeight
                );
                draw->AddRectFilled(playbackMinimum, playbackMaximum, IM_COL32(22, 28, 46, 245), 12.0f * scale);
                draw->AddRect(playbackMinimum, playbackMaximum, IM_COL32(71, 79, 116, 230), 12.0f * scale);
                ImGui::SetCursorScreenPos(ImVec2(playbackMinimum.x + cardPadding, playbackMinimum.y + cardPadding));
                const float playbackHeaderY = ImGui::GetCursorScreenPos().y;
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ColorAccent), "PLAYBACK");
                char speedBuffer[24]{};
                std::snprintf(speedBuffer, sizeof(speedBuffer), "%.1fx", ReplayPlayer::get().playbackSpeed());
                const std::string speedText = speedBuffer;
                ImGui::SetCursorScreenPos(ImVec2(
                    playbackMaximum.x - cardPadding - ImGui::CalcTextSize(speedText.c_str()).x,
                    playbackHeaderY
                ));
                ImGui::TextDisabled("%s", speedText.c_str());

                const float innerWidth = playbackMaximum.x - playbackMinimum.x - cardPadding * 2.0f;
                const float gap = ImGui::GetStyle().ItemSpacing.x;
                const float rowStartX = playbackMinimum.x + cardPadding;
                const float controlsY = playbackHeaderY + ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y;
                const auto togglePlayback = [&] {
                    CinematicCamera::get().enterReplayPreview();
                    if (state == ReplayPlayer::State::Playing) ReplayPlayer::get().pause();
                    else ReplayPlayer::get().resume();
                };
                float speed = ReplayPlayer::get().playbackSpeed();
                if (landscapeEditor) {
                    const float actionWidth = (innerWidth - gap * 2.0f) / 3.0f;
                    const float actionHeight = 38.0f * scale;
                    ImGui::SetCursorScreenPos(ImVec2(rowStartX, controlsY));
                    accentButtonColors();
                    if (ImGui::Button(state == ReplayPlayer::State::Playing ? "Pause" : "Play",
                            ImVec2(actionWidth, actionHeight))) togglePlayback();
                    ImGui::PopStyleColor(4);
                    ImGui::SameLine();
                    if (ImGui::Button(ReplayPlayer::get().muted() ? "Muted" : "Sound",
                            ImVec2(actionWidth, actionHeight))) {
                        ReplayPlayer::get().setMuted(!ReplayPlayer::get().muted());
                    }
                    ImGui::SameLine();
                    accentButtonColors();
                    if (ImGui::Button("Export MP4", ImVec2(actionWidth, actionHeight))) mOpenExportPopup = true;
                    ImGui::PopStyleColor(4);
                    ImGui::SetCursorScreenPos(ImVec2(
                        rowStartX,
                        controlsY + actionHeight + ImGui::GetStyle().ItemSpacing.y
                    ));
                    ImGui::TextDisabled("SPEED");
                    ImGui::SameLine();
                    const float speedLabelWidth = ImGui::CalcTextSize("SPEED").x + gap;
                    ImGui::SetNextItemWidth(std::max(72.0f * scale, innerWidth - speedLabelWidth));
                    if (ImGui::SliderFloat("##ReplaySpeed", &speed, 0.1f, 8.0f, "%.1fx", ImGuiSliderFlags_Logarithmic)) {
                        ReplayPlayer::get().setPlaybackSpeed(speed);
                    }
                } else {
                    const float actionWidth = (innerWidth - gap * (editing ? 2.0f : 1.0f)) / (editing ? 3.0f : 2.0f);
                    ImGui::SetCursorScreenPos(ImVec2(rowStartX, controlsY));
                    accentButtonColors();
                    if (ImGui::Button(state == ReplayPlayer::State::Playing ? "Pause" : "Play",
                            ImVec2(actionWidth, 36.0f * scale))) togglePlayback();
                    ImGui::PopStyleColor(4);
                    ImGui::SameLine();
                    if (ImGui::Button(ReplayPlayer::get().muted() ? "Muted" : "Sound", ImVec2(actionWidth, 36.0f * scale))) {
                        ReplayPlayer::get().setMuted(!ReplayPlayer::get().muted());
                    }
                    if (editing) {
                        ImGui::SameLine();
                        accentButtonColors();
                        if (ImGui::Button("Export MP4", ImVec2(actionWidth, 36.0f * scale))) mOpenExportPopup = true;
                        ImGui::PopStyleColor(4);
                    }
                    ImGui::SetCursorScreenPos(ImVec2(rowStartX, controlsY + 36.0f * scale + gap));
                    ImGui::TextDisabled("SPEED");
                    ImGui::SameLine();
                    const float speedLabelWidth = ImGui::CalcTextSize("SPEED").x + gap;
                    ImGui::SetNextItemWidth(std::max(1.0f, innerWidth - speedLabelWidth));
                    if (ImGui::SliderFloat("##ReplaySpeed", &speed, 0.1f, 8.0f, "%.1fx", ImGuiSliderFlags_Logarithmic)) {
                        ReplayPlayer::get().setPlaybackSpeed(speed);
                    }
                }

                const float replayY = ImGui::GetCursorScreenPos().y;
                const ImVec2 replayMinimum(rowStartX, replayY);
                ImGui::SetCursorScreenPos(replayMinimum);
                const ImVec2 replaySize(innerWidth, 40.0f * scale);
                ImGui::InvisibleButton("##ReplayTimeline", replaySize);
                const ImVec2 replayMaximum(replayMinimum.x + replaySize.x, replayMinimum.y + replaySize.y);
                draw->AddRectFilled(replayMinimum, replayMaximum, IM_COL32(14, 19, 34, 250), 8.0f * scale);
                draw->AddRect(replayMinimum, replayMaximum, IM_COL32(60, 72, 111, 230), 8.0f * scale);
                const auto elapsedText = durationText(current);
                const auto totalText = durationText(duration);
                draw->AddText(ImVec2(replayMinimum.x + 9.0f * scale, replayMinimum.y + 5.0f * scale), ColorText, elapsedText.c_str());
                draw->AddText(
                    ImVec2(replayMaximum.x - 9.0f * scale - ImGui::CalcTextSize(totalText.c_str()).x, replayMinimum.y + 5.0f * scale),
                    ColorTextDim,
                    totalText.c_str()
                );
                const float replayStartX = replayMinimum.x + 9.0f * scale;
                const float replayEndX = replayMaximum.x - 9.0f * scale;
                const float replayLineY = replayMinimum.y + 29.0f * scale;
                draw->AddRectFilled(
                    ImVec2(replayStartX, replayLineY - 3.5f * scale),
                    ImVec2(replayEndX, replayLineY + 3.5f * scale),
                    IM_COL32(42, 48, 76, 255),
                    4.0f * scale
                );
                const auto replayX = [&](std::uint32_t tick) {
                    const float amount = duration == 0 ? 0.0f
                        : std::clamp(static_cast<float>(tick) / static_cast<float>(duration), 0.0f, 1.0f);
                    return std::lerp(replayStartX, replayEndX, amount);
                };
                for (int marker = 1; marker < 10; ++marker) {
                    const float x = std::lerp(replayStartX, replayEndX, static_cast<float>(marker) / 10.0f);
                    draw->AddLine(ImVec2(x, replayLineY - 2.0f * scale), ImVec2(x, replayLineY + 2.0f * scale), IM_COL32(150, 155, 201, 110), 1.0f);
                }
                const float replayCursorX = replayX(current);
                if (replayCursorX > replayStartX) {
                    draw->AddRectFilled(
                        ImVec2(replayStartX, replayLineY - 3.5f * scale),
                        ImVec2(replayCursorX, replayLineY + 3.5f * scale),
                        ColorAccent,
                        4.0f * scale
                    );
                }
                draw->AddCircleFilled(ImVec2(replayCursorX, replayLineY), 7.0f * scale, IM_COL32(159, 121, 250, 75));
                draw->AddCircleFilled(ImVec2(replayCursorX, replayLineY), 4.5f * scale, IM_COL32(241, 232, 255, 255));
                if (timeKeyframes.size() >= 2) {
                    draw->AddRectFilled(
                        ImVec2(replayX(timeKeyframes.front()), replayLineY - 7.0f * scale),
                        ImVec2(replayX(timeKeyframes.back()), replayLineY - 4.5f * scale),
                        ColorTime,
                        2.0f * scale
                    );
                }
                for (std::size_t index = 0; index < timeKeyframes.size(); ++index) {
                    const float timeX = replayX(timeKeyframes[index]);
                    const bool selected = selectedTimeKeyframe && *selectedTimeKeyframe == index;
                    draw->AddLine(
                        ImVec2(timeX, replayMinimum.y + 15.0f * scale),
                        ImVec2(timeX, replayLineY + 7.0f * scale),
                        ColorTime,
                        selected ? 3.0f * scale : 2.0f * scale
                    );
                    const float markerRadius = (selected ? 7.0f : 5.0f) * scale;
                    draw->AddQuadFilled(
                        ImVec2(timeX, replayLineY - 4.0f * scale - markerRadius),
                        ImVec2(timeX + markerRadius, replayLineY - 4.0f * scale),
                        ImVec2(timeX, replayLineY - 4.0f * scale + markerRadius),
                        ImVec2(timeX - markerRadius, replayLineY - 4.0f * scale),
                        ColorTime
                    );
                }
                if (ImGui::IsItemClicked()) {
                    const auto mouse = ImGui::GetIO().MousePos;
                    CinematicCamera::get().enterReplayPreview();
                    std::optional<std::size_t> clickedMarker;
                    float closestDistance = 13.0f * scale;
                    for (std::size_t index = 0; index < timeKeyframes.size(); ++index) {
                        const float distance = std::abs(mouse.x - replayX(timeKeyframes[index]));
                        if (distance <= closestDistance) {
                            closestDistance = distance;
                            clickedMarker = index;
                        }
                    }
                    if (clickedMarker) {
                        camera.selectTimeKeyframe(clickedMarker);
                        selectedTimeKeyframe = clickedMarker;
                        ReplayPlayer::get().seek(timeKeyframes[*clickedMarker]);
                    } else {
                        const float amount = std::clamp(
                            (mouse.x - replayStartX) / std::max(1.0f, replayEndX - replayStartX), 0.0f, 1.0f
                        );
                        ReplayPlayer::get().seek(static_cast<std::uint32_t>(amount * duration));
                    }
                }

                if (editing) {
                    const float timeActionWidth = (innerWidth - gap) * 0.5f;
                    const float timeControlsY = replayMaximum.y + ImGui::GetStyle().ItemSpacing.y;
                    ImGui::SetCursorScreenPos(ImVec2(rowStartX, timeControlsY));
                    accentButtonColors();
                    if (ImGui::Button("Add Time Keyframe", ImVec2(timeActionWidth, 36.0f * scale))) {
                        camera.captureCurrentTimeKeyframe();
                        timeKeyframes = camera.timeKeyframes();
                        selectedTimeKeyframe = camera.selectedTimeKeyframeIndex();
                    }
                    ImGui::PopStyleColor(4);
                    ImGui::SameLine();
                    const bool hasSelectedTimeKeyframe = selectedTimeKeyframe.has_value();
                    if (!hasSelectedTimeKeyframe) ImGui::BeginDisabled();
                    dangerButtonColors();
                    if (ImGui::Button("Delete Time Keyframe", ImVec2(timeActionWidth, 36.0f * scale))) {
                        camera.deleteSelectedTimeKeyframe();
                        timeKeyframes = camera.timeKeyframes();
                        selectedTimeKeyframe = camera.selectedTimeKeyframeIndex();
                    }
                    ImGui::PopStyleColor(3);
                    if (!hasSelectedTimeKeyframe) ImGui::EndDisabled();
                }

                if (editing) {
                    keyframes = camera.keyframes();
                    selectedIndex = camera.selectedIndex();
                    const float cameraHeight = 210.0f * scale;
                    const ImVec2 cameraMinimum = landscapeEditor
                        ? ImVec2(playbackMaximum.x + cardGap, playbackMinimum.y)
                        : ImVec2(playbackMinimum.x, playbackMaximum.y + cardGap);
                    const ImVec2 cameraMaximum(
                        cameraMinimum.x + (landscapeEditor ? cardWidth : deckWidth),
                        cameraMinimum.y + cameraHeight
                    );
                    draw->AddRectFilled(cameraMinimum, cameraMaximum, IM_COL32(21, 29, 48, 245), 12.0f * scale);
                    draw->AddRect(cameraMinimum, cameraMaximum, IM_COL32(64, 92, 145, 230), 12.0f * scale);
                    ImGui::SetCursorScreenPos(ImVec2(cameraMinimum.x + cardPadding, cameraMinimum.y + cardPadding));
                    const float cameraHeaderY = ImGui::GetCursorScreenPos().y;
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ColorCyan), "CAMERA KEYS");
                    ImGui::SameLine();
                    ImGui::TextDisabled("%zu saved", keyframes.size());
                    const char* cameraMode = camera.previewMode() == CinematicCamera::PreviewMode::Cinematic
                        ? "camera locked" : "free camera";
                    ImGui::SetCursorScreenPos(ImVec2(
                        cameraMaximum.x - cardPadding - ImGui::CalcTextSize(cameraMode).x,
                        cameraHeaderY
                    ));
                    ImGui::TextDisabled("%s", cameraMode);

                    const float cameraInnerWidth = cameraMaximum.x - cameraMinimum.x - cardPadding * 2.0f;
                    const float cameraActionWidth = (cameraInnerWidth - gap) * 0.5f;
                    const float cameraActionHeight = 44.0f * scale;
                    const float cameraStatusY = cameraHeaderY + ImGui::GetTextLineHeight()
                        + ImGui::GetStyle().ItemSpacing.y;
                    ImGui::SetCursorScreenPos(ImVec2(cameraMinimum.x + cardPadding, cameraStatusY));
                    draw->PushClipRect(
                        ImVec2(cameraMinimum.x + cardPadding, cameraStatusY),
                        ImVec2(cameraMaximum.x - cardPadding, cameraStatusY + ImGui::GetTextLineHeight()),
                        true
                    );
                    ImGui::TextDisabled("%s", camera.status().c_str());
                    draw->PopClipRect();
                    const float cameraControlsY = cameraStatusY + ImGui::GetTextLineHeight()
                        + ImGui::GetStyle().ItemSpacing.y;
                    ImGui::SetCursorScreenPos(ImVec2(cameraMinimum.x + cardPadding, cameraControlsY));
                    cyanButtonColors();
                    if (ImGui::Button(camera.cinematicPlaying() ? "Pause Camera" : "Play Camera",
                            ImVec2(cameraActionWidth, cameraActionHeight))) {
                        camera.toggleCinematicPreview();
                    }
                    ImGui::PopStyleColor(4);
                    ImGui::SameLine();
                    accentButtonColors();
                    if (ImGui::Button("Add Keyframe", ImVec2(cameraActionWidth, cameraActionHeight))) {
                        camera.captureCurrentKeyframe();
                        keyframes = camera.keyframes();
                        selectedIndex = camera.selectedIndex();
                    }
                    ImGui::PopStyleColor(4);

                    const ImVec2 timelineMinimum(
                        cameraMinimum.x + cardPadding,
                        cameraControlsY + cameraActionHeight + ImGui::GetStyle().ItemSpacing.y
                    );
                    ImGui::SetCursorScreenPos(timelineMinimum);
                    const ImVec2 timelineSize(cameraInnerWidth, 42.0f * scale);
                    ImGui::InvisibleButton("##CameraTimeline", timelineSize);
                    const ImVec2 timelineMaximum(timelineMinimum.x + timelineSize.x, timelineMinimum.y + timelineSize.y);
                    draw->AddRectFilled(timelineMinimum, timelineMaximum, IM_COL32(14, 21, 39, 250), 8.0f * scale);
                    draw->AddRect(timelineMinimum, timelineMaximum, IM_COL32(65, 94, 149, 230), 8.0f * scale);
                    const float cameraStartX = timelineMinimum.x + 9.0f * scale;
                    const float cameraEndX = timelineMaximum.x - 9.0f * scale;
                    const float cameraLineY = timelineMinimum.y + 21.0f * scale;
                    draw->AddRectFilled(
                        ImVec2(cameraStartX, cameraLineY - 3.0f * scale),
                        ImVec2(cameraEndX, cameraLineY + 3.0f * scale),
                        IM_COL32(38, 51, 82, 255),
                        3.0f * scale
                    );
                    const auto cameraX = [&](std::uint32_t tick) {
                        const float amount = duration == 0 ? 0.0f
                            : std::clamp(static_cast<float>(tick) / static_cast<float>(duration), 0.0f, 1.0f);
                        return std::lerp(cameraStartX, cameraEndX, amount);
                    };
                    for (std::size_t index = 0; index < keyframes.size(); ++index) {
                        const float x = cameraX(keyframes[index].tick);
                        const bool selected = selectedIndex && *selectedIndex == index;
                        const ImU32 color = selected ? IM_COL32(238, 243, 255, 255) : ColorCyan;
                        draw->AddTriangleFilled(
                            ImVec2(x, cameraLineY - 10.0f * scale),
                            ImVec2(x - 5.0f * scale, cameraLineY - 3.0f * scale),
                            ImVec2(x + 5.0f * scale, cameraLineY - 3.0f * scale),
                            color
                        );
                        draw->AddLine(
                            ImVec2(x, cameraLineY - 3.0f * scale),
                            ImVec2(x, cameraLineY + 7.0f * scale),
                            color,
                            selected ? 2.5f * scale : 1.5f * scale
                        );
                    }
                    const float cameraCursorX = cameraX(current);
                    draw->AddCircleFilled(ImVec2(cameraCursorX, cameraLineY), 6.0f * scale, IM_COL32(96, 157, 247, 70));
                    draw->AddCircleFilled(ImVec2(cameraCursorX, cameraLineY), 3.7f * scale, ColorCyan);
                    if (ImGui::IsItemClicked()) {
                        const auto mouse = ImGui::GetIO().MousePos;
                        const float amount = std::clamp(
                            (mouse.x - cameraStartX) / std::max(1.0f, cameraEndX - cameraStartX), 0.0f, 1.0f
                        );
                        const auto targetTick = static_cast<std::uint32_t>(amount * duration);
                        camera.seekCinematic(targetTick);
                        std::optional<std::size_t> nearest;
                        float nearestDistance = 18.0f * scale;
                        for (std::size_t index = 0; index < keyframes.size(); ++index) {
                            const float distance = std::abs(cameraX(keyframes[index].tick) - mouse.x);
                            if (distance <= nearestDistance) {
                                nearestDistance = distance;
                                nearest = index;
                            }
                        }
                        camera.select(nearest);
                        selectedIndex = nearest;
                    }

                    const float cameraFooterY = timelineMaximum.y + ImGui::GetStyle().ItemSpacing.y;
                    ImGui::SetCursorScreenPos(ImVec2(cameraMinimum.x + cardPadding, cameraFooterY));
                    const bool hasSelectedKeyframe = selectedIndex && *selectedIndex < keyframes.size();
                    if (!hasSelectedKeyframe) ImGui::BeginDisabled();
                    dangerButtonColors();
                    if (ImGui::Button("Delete Keyframe", ImVec2(cameraActionWidth, 36.0f * scale))) {
                        camera.deleteSelected();
                        keyframes = camera.keyframes();
                        selectedIndex = camera.selectedIndex();
                    }
                    ImGui::PopStyleColor(3);
                    ImGui::SameLine();
                    cyanButtonColors();
                    if (ImGui::Button("Edit Keyframe", ImVec2(cameraActionWidth, 36.0f * scale))) {
                        mCameraPropertiesExpanded = true;
                    }
                    ImGui::PopStyleColor(4);
                    if (!hasSelectedKeyframe) ImGui::EndDisabled();
                    ImGui::SetCursorScreenPos(ImVec2(
                        playbackMinimum.x,
                        std::max(playbackMaximum.y, cameraMaximum.y) + cardGap
                    ));
                }
            }
        }

        const ImVec2 minimum = ImGui::GetWindowPos();
        const ImVec2 maximum(minimum.x + ImGui::GetWindowSize().x, minimum.y + ImGui::GetWindowSize().y);
        std::scoped_lock lock(mMutex);
        mControlMinX = minimum.x;
        mControlMinY = minimum.y;
        mControlMaxX = maximum.x;
        mControlMaxY = maximum.y;
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
    if (editing || mModalVisible.load(std::memory_order_acquire) || mOpenExportPopup) drawExportPopup();
    drawExportResultPopup();
}

void ReplayUi::drawExportResultPopup() {
    if (mOpenExportResultPopup) {
        ImGui::OpenPopup("Replay export result");
        mOpenExportResultPopup = false;
    }
    const bool succeeded = ReplayExporter::get().state() == ReplayExporter::State::Completed;
    const float scale = uiScale();
    const auto display = ImGui::GetIO().DisplaySize;
    const float popupWidth = std::max(1.0f, std::min(520.0f * scale, display.x - 48.0f * scale));
    const float popupHeight = std::max(1.0f, std::min(330.0f * scale, display.y - 40.0f * scale));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(popupWidth, popupHeight), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(16, 22, 38, 252));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(79, 75, 129, 230));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * scale, 16.0f * scale));
    const bool visible = ImGui::BeginPopupModal("Replay export result", nullptr,
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
    if (!visible) {
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        return;
    }
    mModalVisible.store(true, std::memory_order_release);
    auto* draw = ImGui::GetWindowDrawList();
    const auto popupMinimum = ImGui::GetWindowPos();
    const auto popupMaximum = ImVec2(popupMinimum.x + ImGui::GetWindowSize().x, popupMinimum.y + ImGui::GetWindowSize().y);
    draw->AddRectFilled(
        ImVec2(popupMinimum.x + 22.0f * scale, popupMinimum.y),
        ImVec2(popupMaximum.x - 22.0f * scale, popupMinimum.y + 3.0f * scale),
        ColorAccent,
        2.0f * scale
    );
    ImGui::TextColored(
        succeeded ? ImGui::ColorConvertU32ToFloat4(ColorAccent) : ImVec4(1.0f, 0.38f, 0.34f, 1.0f),
        "%s",
        succeeded ? "EXPORT COMPLETE" : "EXPORT FAILED"
    );
    ImGui::TextDisabled("%s", succeeded ? "Your replay video is ready." : "The unfinished MP4 was removed.");
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(30, 35, 59, 230));
    ImGui::BeginChild("##ExportResultDetails", ImVec2(0.0f, 196.0f * scale), true);
    if (succeeded) {
        const auto output = ReplayExporter::get().outputPath();
        const auto settings = ReplayExporter::get().settings();
        std::error_code sizeError;
        const auto outputSize = std::filesystem::file_size(output, sizeError);
        ImGui::TextDisabled("FILE NAME");
        ImGui::TextWrapped("%s", output.filename().string().c_str());
        ImGui::Separator();
        ImGui::TextDisabled("SAVED TO");
        ImGui::TextWrapped("%s", output.parent_path().string().c_str());
        ImGui::Separator();
        ImGui::TextDisabled("VIDEO DETAILS");
        ImGui::Text("%d x %d  |  %d FPS  |  %d Mbps",
            settings.width,
            settings.height,
            settings.frameRate,
            settings.bitrate / 1000000
        );
        ImGui::Text("File size: %s", sizeError ? "Unknown" : sizeText(outputSize).c_str());
    } else {
        ImGui::TextDisabled("ERROR");
        ImGui::Spacing();
        ImGui::TextWrapped("%s", ReplayExporter::get().status().c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextDisabled("You can close this message and try a lower export preset if the device encoder rejects 1080p60.");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    accentButtonColors();
    if (ImGui::Button("Done", ImVec2(ImGui::GetContentRegionAvail().x, 36.0f * scale))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(4);
    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void ReplayUi::drainInput() {
    std::vector<PendingInput> pending;
    {
        std::scoped_lock lock(mMutex);
        pending.swap(mPendingInput);
    }
    auto& io = ImGui::GetIO();
    for (const auto& event : pending) {
        if (event.type == PendingInput::Type::Touch) {
            io.AddMousePosEvent(event.x, event.y);
            if (event.a == ActionDown) io.AddMouseButtonEvent(0, true);
            else if (event.a == ActionUp || event.a == ActionCancel) io.AddMouseButtonEvent(0, false);
        } else if (event.type == PendingInput::Type::Key) {
            const auto key = mapKey(event.a);
            if (key != ImGuiKey_None) io.AddKeyEvent(key, event.down);
            if (event.down && event.codepoint >= 0x20) io.AddInputCharacter(event.codepoint);
        } else if (event.type == PendingInput::Type::Text && !event.text.empty()) {
            io.AddInputCharactersUTF8(event.text.c_str());
        }
    }
}

void ReplayUi::captureReplayPreview(int displayWidth, int displayHeight) {
    auto& recorder = ReplayRecorder::get();
    const auto state = recorder.state();
    const bool recording = state == ReplayRecorder::State::Recording || state == ReplayRecorder::State::Paused;
    if (!recording) {
        mPreviewReplayPath.clear();
        return;
    }
    const auto path = recorder.activeReplayPath();
    if (path.empty()) return;
    if (path != mPreviewReplayPath) {
        mPreviewReplayPath = path;
        mPreviewCaptureAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
        writeWorldMetadata(path, recorder.currentWorldName());
    }
    if (mPreviewCaptureAt == std::chrono::steady_clock::time_point::max()
        || std::chrono::steady_clock::now() < mPreviewCaptureAt) return;

    constexpr int outputWidth = 640;
    constexpr int outputHeight = 360;
    captureAdvancedScene(displayWidth, displayHeight);
    GLint previousReadBuffer = 0, previousPackAlignment = 0, previousPackBuffer = 0;
    GLint previousPackRowLength = 0, previousPackSkipRows = 0, previousPackSkipPixels = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadBuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPackBuffer);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &previousPackRowLength);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &previousPackSkipRows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &previousPackSkipPixels);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, mAdvancedSceneFramebuffer);
    const bool copied = mAdvancedSceneTexture && mAdvancedSceneFramebuffer
        && glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (!copied) glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    const int sourceWidth = copied ? mAdvancedSceneWidth : displayWidth;
    const int sourceHeight = copied ? mAdvancedSceneHeight : displayHeight;
    const float outputAspect = static_cast<float>(outputWidth) / outputHeight;
    const int cropWidth = std::min(sourceWidth,
        std::max(1, static_cast<int>(sourceHeight * outputAspect)));
    const int cropHeight = std::min(sourceHeight,
        std::max(1, static_cast<int>(sourceWidth / outputAspect)));
    const int cropLeft = (sourceWidth - cropWidth) / 2;
    const int cropBottom = (sourceHeight - cropHeight) / 2;
    std::vector<std::uint8_t> source(static_cast<std::size_t>(cropWidth) * cropHeight * 4);
    std::vector<std::uint8_t> preview(static_cast<std::size_t>(outputWidth) * outputHeight * 4);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glReadPixels(cropLeft, cropBottom, cropWidth, cropHeight, GL_RGBA, GL_UNSIGNED_BYTE, source.data());
    glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, previousPackRowLength);
    glPixelStorei(GL_PACK_SKIP_ROWS, previousPackSkipRows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, previousPackSkipPixels);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previousPackBuffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadBuffer));
    releaseAdvancedScene();
    for (int y = 0; y < outputHeight; ++y) {
        const int sourceY = cropHeight - 1 - (y * cropHeight / outputHeight);
        for (int x = 0; x < outputWidth; ++x) {
            const int sourceX = x * cropWidth / outputWidth;
            const auto sourceOffset = (static_cast<std::size_t>(sourceY) * cropWidth + sourceX) * 4;
            const auto targetOffset = (static_cast<std::size_t>(y) * outputWidth + x) * 4;
            std::memcpy(preview.data() + targetOffset, source.data() + sourceOffset, 4);
            preview[targetOffset + 3] = 255;
        }
    }
    std::ofstream output(thumbnailPath(path), std::ios::binary | std::ios::trunc);
    const std::uint32_t width = outputWidth;
    const std::uint32_t height = outputHeight;
    if (output) {
        output.write(reinterpret_cast<const char*>(&ThumbnailMagic), sizeof(ThumbnailMagic));
        output.write(reinterpret_cast<const char*>(&width), sizeof(width));
        output.write(reinterpret_cast<const char*>(&height), sizeof(height));
        output.write(reinterpret_cast<const char*>(preview.data()), static_cast<std::streamsize>(preview.size()));
    }
    mPreviewCaptureAt = std::chrono::steady_clock::time_point::max();
}

void ReplayUi::frame() {
    if (!mInputInstalled.load(std::memory_order_acquire)) installInput();
    const int delayed = mOpenDelayFrames.load(std::memory_order_acquire);
    if (delayed > 0 && mOpenDelayFrames.fetch_sub(1, std::memory_order_acq_rel) == 1) openBrowser();

    const auto recorderState = ReplayRecorder::get().state();
    const bool recording = recorderState == ReplayRecorder::State::Recording || recorderState == ReplayRecorder::State::Paused;
    const bool browser = browserVisible();
    const bool replay = activeReplayState(ReplayPlayer::get().state());
    if (!browser && !replay && !recording) {
        mRenderReady.store(false, std::memory_order_release);
        mAdvancedInputActive.store(false, std::memory_order_release);
        if (mAdvancedSceneTexture && reinterpret_cast<void*>(eglGetCurrentContext()) == mEglContext)
            releaseAdvancedScene();
        return;
    }

    const EGLContext context = eglGetCurrentContext();
    if (context == EGL_NO_CONTEXT) {
        mRenderReady.store(false, std::memory_order_release);
        return;
    }
    if (reinterpret_cast<void*>(context) != mEglContext) resetImGui(reinterpret_cast<void*>(context));
    const EGLDisplay display = eglGetCurrentDisplay();
    const EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
    if (display == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE) {
        mRenderReady.store(false, std::memory_order_release);
        return;
    }
    EGLint width = 0;
    EGLint height = 0;
    if (!eglQuerySurface(display, surface, EGL_WIDTH, &width)
        || !eglQuerySurface(display, surface, EGL_HEIGHT, &height) || width <= 0 || height <= 0) {
        mRenderReady.store(false, std::memory_order_release);
        return;
    }

    if (recording) captureReplayPreview(width, height);
    if (!browserVisible() && !replay) {
        mRenderReady.store(false, std::memory_order_release);
        return;
    }

    ensureImGui(static_cast<float>(width), static_cast<float>(height));
    if (mEditMode.load(std::memory_order_acquire) && mAdvancedMode && !mCameraPropertiesExpanded
        && !mReplayControlsHidden.load(std::memory_order_acquire) && !ReplayExporter::get().active()
        && replayGameplayVisible()) captureAdvancedScene(width, height);
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DeltaTime = 1.0f / 60.0f;
    ImGui_ImplOpenGL3_NewFrame();
    drainInput();
    ImGui::NewFrame();
    if (browserVisible()) drawBrowser();
    drawReplayControls();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    mLastRenderedAtMs.store(monotonicMilliseconds(), std::memory_order_release);
    mRenderReady.store(true, std::memory_order_release);
}

bool ReplayUi::pointInsideControls(float x, float y) const {
    std::scoped_lock lock(mMutex);
    if (!mAdvancedControlRects.empty()) {
        return std::any_of(mAdvancedControlRects.begin(), mAdvancedControlRects.end(), [=](const ControlRect& rect) {
            return x >= rect.minX && x <= rect.maxX && y >= rect.minY && y <= rect.maxY;
        });
    }
    return x >= mControlMinX && x <= mControlMaxX && y >= mControlMinY && y <= mControlMaxY;
}

bool ReplayUi::recentlyRendered() const noexcept {
    if (!mRenderReady.load(std::memory_order_acquire)) return false;
    const auto last = mLastRenderedAtMs.load(std::memory_order_acquire);
    const auto now = monotonicMilliseconds();
    return last != 0 && now >= last && now - last <= 500;
}

bool ReplayUi::onTouch(const pl::input::TouchEvent& event) {
    const bool browser = browserVisible();
    const bool replay = activeReplayState(ReplayPlayer::get().state());
    const bool exporting = ReplayExporter::get().active();
    if ((!browser && !replay && !exporting) || (!exporting && !recentlyRendered())) return false;
    if (replay && !browser && !exporting && !mModalVisible.load(std::memory_order_acquire)
        && onAdvancedTouch(event)) return true;
    if (exporting) {
        std::scoped_lock lock(mMutex);
        if (event.action == ActionDown && mReplayUiPointer < 0) mReplayUiPointer = event.pointerId;
        if (event.pointerId == mReplayUiPointer) {
            PendingInput pending;
            pending.type = PendingInput::Type::Touch;
            pending.a = event.action;
            pending.b = event.pointerId;
            pending.x = event.x;
            pending.y = event.y;
            mPendingInput.emplace_back(std::move(pending));
            if (event.action == ActionUp || event.action == ActionCancel) mReplayUiPointer = -1;
        }
        return true;
    }
    bool capture = browser || advancedViewportActive();
    if (replay && !browser) {
        const bool modal = mModalVisible.load(std::memory_order_acquire);
        const bool inside = pointInsideControls(event.x, event.y);
        std::scoped_lock lock(mMutex);
        if (event.action == ActionDown && (inside || modal)) {
            if (mReplayUiPointer >= 0) return true;
            mReplayUiPointer = event.pointerId;
            capture = true;
        } else if (event.pointerId == mReplayUiPointer) {
            capture = true;
            if (event.action == ActionUp || event.action == ActionCancel) mReplayUiPointer = -1;
        } else {

            return capture || CinematicCamera::get().blocksManualInput();
        }
    }
    if (!capture) return CinematicCamera::get().blocksManualInput();
    {
        std::scoped_lock lock(mMutex);
        PendingInput pending;
        pending.type = PendingInput::Type::Touch;
        pending.a = event.action;
        pending.b = event.pointerId;
        pending.x = event.x;
        pending.y = event.y;
        mPendingInput.emplace_back(std::move(pending));
    }
    return true;
}

bool ReplayUi::onKey(const pl::input::KeyEvent& event) {
    const bool browser = browserVisible();
    const bool replay = activeReplayState(ReplayPlayer::get().state());
    const bool exporting = ReplayExporter::get().active();
    if ((!browser && !replay && !exporting) || (!exporting && !recentlyRendered())) return false;
    if (event.keyCode == KeyBack && event.isKeyDown && browser && !mKeyboardVisible.load(std::memory_order_acquire)) {
        closeBrowser();
        return true;
    }
    if (exporting) return true;
    if (replay && !browser) return CinematicCamera::get().blocksManualInput();
    {
        std::scoped_lock lock(mMutex);
        PendingInput pending;
        pending.type = PendingInput::Type::Key;
        pending.a = event.keyCode;
        pending.codepoint = event.unicodeChar;
        pending.down = event.isKeyDown;
        mPendingInput.emplace_back(std::move(pending));
    }
    return browser;
}

bool ReplayUi::onMouse(const pl::input::MouseEvent&) {
    return ReplayExporter::get().active() || advancedViewportActive() || CinematicCamera::get().blocksManualInput();
}

bool ReplayUi::onText(const pl::input::TextInputEvent& event) {
    const bool browser = browserVisible();
    if (!browser || !recentlyRendered()) return false;
    {
        std::scoped_lock lock(mMutex);
        PendingInput pending;
        pending.type = PendingInput::Type::Text;
        pending.text = event.text;
        mPendingInput.emplace_back(std::move(pending));
    }
    return true;
}

}

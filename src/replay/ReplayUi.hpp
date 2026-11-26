#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <bedrocktools/sdk/Types.hpp>
#include "ReplayExporter.hpp"

namespace pl::input { struct TouchEvent; struct KeyEvent; struct TextInputEvent; struct MouseEvent; }
struct ImDrawList;

namespace bedrocktools::replay {

struct ReplayArchiveData;
}
namespace bedrocktools::sdk { class Player; }
namespace bedrocktools::replay {

class ReplayUi {
public:
    static ReplayUi& get();

    void installInput();
    void frame();
    void requestBrowserOpen();
    void openBrowser();
    void closeBrowser();
    bool browserVisible() const noexcept;
    bool editingReplay() const noexcept;
    bool advancedViewportActive() const noexcept;
    void applyAdvancedCameraInput(bedrocktools::sdk::Player* player);
    std::optional<bedrocktools::sdk::Vec3> advancedCameraMovement();
    void applyAdvancedLookInput(bedrocktools::sdk::Player* player = nullptr);
    void clearAdvancedLookPlayer();

private:
    struct PendingInput {
        enum class Type { Touch, Key, Text };
        Type type{};
        int a{};
        int b{};
        float x{};
        float y{};
        unsigned int codepoint{};
        bool down{};
        std::string text;
    };

    struct Thumbnail {
        std::vector<std::uint8_t> pixels;
        int width{};
        int height{};
        unsigned int texture{};
    };

    struct Entry {
        std::filesystem::path path;
        std::string name;
        std::string worldName{"Unknown world"};
        std::uint32_t durationTicks{};
        std::uintmax_t size{};
        std::filesystem::file_time_type modified{};
        std::shared_ptr<Thumbnail> thumbnail;
        std::shared_ptr<ReplayArchiveData> archive;
    };

    enum class SortMode { Newest, Name };

    ReplayUi() = default;
    void refresh();
    void ensureImGui(float displayWidth, float displayHeight);
    void resetImGui(void* context);
    void drawBrowser();
    void drawReplayControls();
    void drawAdvancedControls();
    void drawCameraMarkers(ImDrawList* draw, float left, float top, float right, float bottom,
        float sourceAspect, float cropX = 0, float cropY = 0);
    void loadAdvancedSensitivity();
    void saveAdvancedSensitivity();
    void captureAdvancedScene(int displayWidth, int displayHeight);
    void releaseAdvancedScene();
    bool onAdvancedTouch(const pl::input::TouchEvent& event);
    void drawExportPopup();
    void drawExportResultPopup();
    void requestImport();
    void finishImport(bool success, const char* path, const char* displayName, const char* error);
    void drainInput();
    void captureReplayPreview(int displayWidth, int displayHeight);
    unsigned int thumbnailTexture(const std::shared_ptr<Thumbnail>& thumbnail);
    unsigned int logoTexture();
    unsigned int loadingBackgroundTexture();
    void drawThumbnail(const std::shared_ptr<Thumbnail>& thumbnail, float width, float height);
    void renameReplay(const std::filesystem::path& source, std::string requestedName);
    void deleteReplayFiles(const std::filesystem::path& replay);
    bool onTouch(const pl::input::TouchEvent& event);
    bool onKey(const pl::input::KeyEvent& event);
    bool onText(const pl::input::TextInputEvent& event);
    bool onMouse(const pl::input::MouseEvent& event);
    bool pointInsideControls(float x, float y) const;
    bool recentlyRendered() const noexcept;

    std::atomic_bool mInputInstalled{false};
    std::atomic_bool mInputInstalling{false};
    std::atomic_bool mBrowserVisible{false};
    std::atomic_bool mEditMode{false};
    std::atomic_bool mReplayControlsHidden{false};
    std::atomic_bool mEditAutoPausePending{false};
    std::atomic_bool mModalVisible{false};
    std::atomic_bool mRenderReady{false};
    std::atomic_int mOpenDelayFrames{0};
    std::atomic<std::uint64_t> mLastRenderedAtMs{0};
    bool mImGuiInitialized{};
    void* mEglContext{};
    unsigned int mLogoTexture{};
    unsigned int mLoadingBackgroundTexture{};
    std::atomic_bool mRefreshRequested{true};
    std::atomic_bool mImportPending{false};
    std::vector<Entry> mEntries;
    std::vector<PendingInput> mPendingInput;
    std::string mError;
    std::string mStatusMessage;
    std::filesystem::path mSelectedPath;
    std::filesystem::path mPendingDelete;
    std::filesystem::path mPendingRename;
    std::filesystem::path mPreviewReplayPath;
    std::chrono::steady_clock::time_point mPreviewCaptureAt{};
    char mSearch[96]{};
    char mRename[96]{};
    SortMode mSortMode{SortMode::Newest};
    ReplayExporter::Settings mExportSettings;
    bool mOpenExportPopup{};
    bool mOpenExportResultPopup{};
    bool mCameraPropertiesExpanded{};
    bool mAdvancedMode{};
    bool mAdvancedToolsOpen{};
    std::atomic_bool mAdvancedInputActive{false};
    int mAdvancedToolTab{};
    float mAdvancedZoom{1.0f};
    float mAdvancedViewStart{};
    int mAdvancedScrubTrack{-1};
    std::uint32_t mAdvancedScrubTick{};
    struct ControlRect { float minX, minY, maxX, maxY; };
    std::vector<ControlRect> mAdvancedControlRects;
    ControlRect mAdvancedViewportRect{};
    ControlRect mAdvancedMoveRect{};
    ControlRect mAdvancedLookRect{};
    ControlRect mAdvancedTurnLeftRect{};
    ControlRect mAdvancedTurnRightRect{};
    ControlRect mAdvancedAscendRect{};
    ControlRect mAdvancedDescendRect{};
    ControlRect mAdvancedToolsRect{};
    int mAdvancedToolPointer{-1};
    float mAdvancedToolLastY{};
    float mAdvancedToolScrollDelta{};
    bool mAdvancedToolSwiping{};
    int mAdvancedMovePointer{-1};
    int mAdvancedLookPointer{-1};
    int mAdvancedLiftPointer{-1};
    int mAdvancedTurnPointer{-1};
    float mAdvancedMoveX{};
    float mAdvancedMoveY{};
    float mAdvancedLift{};
    float mAdvancedTurn{};
    float mAdvancedLookX{};
    float mAdvancedLookY{};
    std::atomic<float> mAdvancedLookSensitivity{2.0f};
    std::filesystem::path mAdvancedSensitivityFile;
    bool mAdvancedSensitivityLoaded{};
    std::atomic<bedrocktools::sdk::Player*> mAdvancedLookPlayer{nullptr};
    float mAdvancedLastLookX{};
    float mAdvancedLastLookY{};
    unsigned int mAdvancedSceneTexture{};
    unsigned int mAdvancedSceneFramebuffer{};
    int mAdvancedSceneWidth{};
    int mAdvancedSceneHeight{};
    int mAdvancedSceneSourceWidth{};
    int mAdvancedSceneSourceHeight{};
    void* mAdvancedSceneSurface{};
    bool mAdvancedSceneDirectCopy{};
    bool mAdvancedSceneCopyChecked{};
    ReplayExporter::State mLastExportState{ReplayExporter::State::Idle};
    int mReplayUiPointer{-1};
    std::atomic_bool mKeyboardVisible{false};
    mutable std::mutex mMutex;
    float mControlMinX{};
    float mControlMinY{};
    float mControlMaxX{};
    float mControlMaxY{};
};

}

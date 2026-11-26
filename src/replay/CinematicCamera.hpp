#pragma once

#include "CameraPath.hpp"
#include "EditorCameraMotion.hpp"
#include "PlayerView.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bedrocktools::sdk { class Player; }

namespace bedrocktools::replay {

std::filesystem::path cinematicCameraPath(const std::filesystem::path& replayPath);
std::vector<std::uint32_t> cinematicTimeKeyframes(const std::filesystem::path& replayPath);

class CinematicCamera {
public:
    enum class PreviewMode { Replay, Cinematic };

    static CinematicCamera& get();

    bool beginSession(const std::filesystem::path& replayPath, std::uint32_t durationTicks);
    void endSession();
    bool sessionActive() const;
    std::string status() const;

    std::vector<CameraKeyframe> keyframes() const;
    std::optional<std::size_t> selectedIndex() const;
    void select(std::optional<std::size_t> index);
    bool captureCurrentKeyframe();
    bool updateSelected(CameraKeyframe keyframe);
    bool deleteSelected();
    std::vector<std::uint32_t> timeKeyframes() const;
    std::optional<std::size_t> selectedTimeKeyframeIndex() const;
    void selectTimeKeyframe(std::optional<std::size_t> index);
    bool captureCurrentTimeKeyframe();
    bool deleteSelectedTimeKeyframe();

    PreviewMode previewMode() const;
    void enterReplayPreview();
    bool toggleCinematicPreview();
    bool seekCinematic(std::uint32_t tick);
    bool cinematicPlaying() const;
    bool blocksManualInput() const;
    PlayerPerspective playerPerspective() const;
    std::optional<PlayerViewId> selectedPlayer() const;
    void selectPlayerView(const PlayerViewId& id, PlayerPerspective perspective);
    void enterFreeCamera();
    void beginRenderFrame();
    std::uint64_t firstPersonRenderTarget() const;
    std::uint64_t firstPersonRenderTarget(const bedrocktools::sdk::Vec3* origin) const;

    struct MarkerView {
        std::optional<CameraPose> camera;
        std::vector<CameraKeyframe> keys;
        std::optional<std::size_t> selected;
    };
    MarkerView markerView() const;

    void onPlayerTick(bedrocktools::sdk::Player* player);
    void applyCamera(void* renderer, void* camera);
    void applyRendererCamera(void* renderer, void* renderCameraComponent = nullptr);
    void applyLevelCamera(void* levelRenderer);
    void releaseEditorCamera();
    float overrideFov(float nativeFov);
    void observeNativeFov(float nativeFov);

private:
    CinematicCamera() = default;

    bool loadLocked();
    bool saveLocked();
    bool shouldApplyLocked(bool exporting) const;
    bool shouldFollowLocked(bool exporting) const;
    std::optional<CameraPose> playerCameraLocked();

    mutable std::recursive_mutex mMutex;
    CameraPath mPath;
    std::filesystem::path mReplayPath;
    std::uint32_t mDurationTicks{};
    std::optional<std::size_t> mSelected;
    std::vector<std::uint32_t> mTimeKeyframes;
    std::optional<std::size_t> mSelectedTimeKeyframe;
    PreviewMode mPreviewMode{PreviewMode::Replay};
    bool mSessionActive{};
    bool mResumeRequested{};
    std::optional<CameraPose> mObservedCamera;
    std::optional<CameraPose> mRenderedCamera;
    EditorCameraMotion mEditorMotion;
    std::optional<CameraPose> mEditorCamera;
    PlayerPerspective mPlayerPerspective{PlayerPerspective::Free};
    std::optional<PlayerViewId> mSelectedPlayer;
    std::optional<CameraPose> mPlayerCamera;
    std::uint64_t mPlayerRuntimeId{};
    bool mPlayerFramePrepared{};
    float mLastNativeFov{70.0f};
    std::string mStatus{"Camera editor ready"};
};

}

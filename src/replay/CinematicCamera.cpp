#include "CinematicCamera.hpp"
#include "CameraVisibility.hpp"

#include "ReplayExporter.hpp"
#include "ReplayPlayer.hpp"
#include "ReplayUi.hpp"

#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/input/MoveInput.hpp>
#include <bedrocktools/sdk/offsets/Render.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <system_error>

namespace bedrocktools::replay {
namespace {

constexpr std::array<char, 8> CameraMagic{'B', 'R', 'C', 'A', 'M', '0', '1', '\0'};
constexpr std::uint32_t LegacyCameraVersion = 1;
constexpr std::uint32_t SingleTimeKeyframeVersion = 2;
constexpr std::uint32_t CameraVersion = 3;
constexpr std::uint32_t NoTimeKeyframe = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t MaximumKeyframes = 100000;
constexpr std::uint32_t MaximumTimeKeyframes = 100000;
constexpr std::int32_t ReplayWorldDimensionId = 0;
constexpr float Pi = 3.14159265358979323846f;
constexpr float DegreesPerRadian = 180.0f / Pi;
constexpr float RadiansPerDegree = Pi / 180.0f;

template <class Value>
bool readValue(std::istream& input, Value& value) {
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(input);
}

template <class Value>
bool writeValue(std::ostream& output, const Value& value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return static_cast<bool>(output);
}

std::int32_t currentDimension() {
    return ReplayWorldDimensionId;
}

bool exporterApplyingCamera() {
    return ReplayExporter::get().state() == ReplayExporter::State::Exporting;
}

bool finite(const bedrocktools::sdk::Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

float dot(const bedrocktools::sdk::Vec3& left, const bedrocktools::sdk::Vec3& right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

std::optional<bedrocktools::sdk::Vec3> normalized(const bedrocktools::sdk::Vec3& value) {
    const float lengthSquared = dot(value, value);
    if (!finite(value) || !std::isfinite(lengthSquared) || lengthSquared <= 0.000001f) return std::nullopt;
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    return bedrocktools::sdk::Vec3{
        value.x * inverseLength,
        value.y * inverseLength,
        value.z * inverseLength
    };
}

struct CameraBasis {
    bedrocktools::sdk::Vec3 right;
    bedrocktools::sdk::Vec3 up;
    bedrocktools::sdk::Vec3 forward;
};

CameraBasis cameraBasis(float yaw, float pitch, float roll) {
    const float yawRadians = yaw * RadiansPerDegree;
    const float pitchRadians = pitch * RadiansPerDegree;
    const float sinYaw = std::sin(yawRadians);
    const float cosYaw = std::cos(yawRadians);
    const float sinPitch = std::sin(pitchRadians);
    const float cosPitch = std::cos(pitchRadians);
    CameraBasis result{
        {-cosYaw, 0.0f, -sinYaw},
        {-sinYaw * sinPitch, cosPitch, cosYaw * sinPitch},
        {-sinYaw * cosPitch, -sinPitch, cosYaw * cosPitch}
    };
    const float rollRadians = roll * RadiansPerDegree;
    const float cosine = std::cos(rollRadians);
    const float sine = std::sin(rollRadians);
    const auto right = result.right;
    result.right = {
        right.x * cosine + result.up.x * sine,
        right.y * cosine + result.up.y * sine,
        right.z * cosine + result.up.z * sine
    };
    result.up = {
        result.up.x * cosine - right.x * sine,
        result.up.y * cosine - right.y * sine,
        result.up.z * cosine - right.z * sine
    };
    return result;
}

std::array<float, 16> viewMatrix(const bedrocktools::sdk::Vec3& position, const CameraBasis& basis) {
    return {
        basis.right.x, basis.up.x, -basis.forward.x, 0.0f,
        basis.right.y, basis.up.y, -basis.forward.y, 0.0f,
        basis.right.z, basis.up.z, -basis.forward.z, 0.0f,
        -dot(basis.right, position), -dot(basis.up, position), dot(basis.forward, position), 1.0f
    };
}

std::array<float, 16> inverseViewMatrix(const bedrocktools::sdk::Vec3& position, const CameraBasis& basis) {
    return {
        basis.right.x, basis.right.y, basis.right.z, 0.0f,
        basis.up.x, basis.up.y, basis.up.z, 0.0f,
        -basis.forward.x, -basis.forward.y, -basis.forward.z, 0.0f,
        position.x, position.y, position.z, 1.0f
    };
}

float* matrixStackTop(void* stack, bool dirty) {
    if (!stack) return nullptr;
    const auto offset = bedrocktools::sdk::field<std::uint64_t>(
        stack, bedrocktools::sdk::offsets::CameraMatrixStack::mOffset
    );
    const auto count = bedrocktools::sdk::field<std::uint64_t>(
        stack, bedrocktools::sdk::offsets::CameraMatrixStack::mCount
    );
    if (offset > std::numeric_limits<std::uint64_t>::max() - count || offset + count == 0) return nullptr;
    auto** blocks = bedrocktools::sdk::field<void**>(
        stack, bedrocktools::sdk::offsets::CameraMatrixStack::mBlocks
    );
    if (!blocks) return nullptr;
    const auto index = offset + count - 1;
    auto* block = blocks[index >> 6U];
    if (!block) return nullptr;
    if (dirty) {
        bedrocktools::sdk::field<bool>(stack, bedrocktools::sdk::offsets::CameraMatrixStack::mDirty) = true;
    }
    return reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(block) + (index & 0x3FU) * 64U);
}

std::optional<CameraPose> observedCameraPose(void* renderer, void* camera, float fallbackFov) {
    if (!renderer || !camera) return std::nullopt;
    const auto position = bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(
        renderer, bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos
    );
    const auto* matrix = matrixStackTop(
        reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(camera)
            + bedrocktools::sdk::offsets::Camera::mViewMatrixStack),
        false
    );
    if (!matrix || !finite(position)) return std::nullopt;
    const auto forward = normalized({-matrix[2], -matrix[6], -matrix[10]});
    const auto right = normalized({matrix[0], matrix[4], matrix[8]});
    if (!forward || !right) return std::nullopt;
    CameraPose observed;
    observed.position = position;
    observed.yaw = std::atan2(-forward->x, forward->z) * DegreesPerRadian;
    observed.pitch = std::asin(std::clamp(-forward->y, -1.0f, 1.0f)) * DegreesPerRadian;
    const auto unrolled = cameraBasis(observed.yaw, observed.pitch, 0.0f);
    observed.roll = std::atan2(dot(*right, unrolled.up), dot(*right, unrolled.right)) * DegreesPerRadian;
    const float fov = bedrocktools::sdk::field<float>(camera, bedrocktools::sdk::offsets::Camera::mFov);
    observed.fov = std::isfinite(fov) && fov > 1.0f && fov < 179.0f ? fov : fallbackFov;
    observed.dimension = ReplayWorldDimensionId;
    return observed;
}

void writeNativeCameraPose(void* camera, const bedrocktools::sdk::Vec3& position, const CameraBasis& basis) {
    const auto view = viewMatrix(position, basis);
    const auto inverse = inverseViewMatrix(position, basis);
    auto* stack = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(camera)
        + bedrocktools::sdk::offsets::Camera::mViewMatrixStack);
    if (auto* top = matrixStackTop(stack, true)) std::copy(view.begin(), view.end(), top);
    std::copy(
        inverse.begin(), inverse.end(),
        reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(camera)
            + bedrocktools::sdk::offsets::Camera::mInverseViewMatrix)
    );
    bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(camera, bedrocktools::sdk::offsets::Camera::mRight)
        = basis.right;
    bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(camera, bedrocktools::sdk::offsets::Camera::mUp)
        = basis.up;
    bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(camera, bedrocktools::sdk::offsets::Camera::mForward)
        = basis.forward;
    bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(camera, bedrocktools::sdk::offsets::Camera::mPosition)
        = position;
}

void applyRendererPose(void* renderer, const CameraPose& pose, bool writeWorldSpaceCamera = true) {
    if (!renderer) return;
    const auto basis = cameraBasis(pose.yaw, pose.pitch, pose.roll);
    if (writeWorldSpaceCamera) {
        writeNativeCameraPose(reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(renderer)
            + bedrocktools::sdk::offsets::LevelRendererPlayer::mWorldSpaceCamera), pose.position, basis);
    }
    const bedrocktools::sdk::Vec3 localPosition{};
    const auto localView = viewMatrix(localPosition, basis);
    std::copy(
        localView.begin(), localView.end(),
        reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(renderer)
            + bedrocktools::sdk::offsets::LevelRendererPlayer::mViewMatrix)
    );
    bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(
        renderer, bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos
    ) = pose.position;
    bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(
        renderer, bedrocktools::sdk::offsets::LevelRendererPlayer::mCameraTargetPos
    ) = pose.position;

    bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(renderer,
        bedrocktools::sdk::offsets::LevelRendererPlayer::mWorldToCameraTranslation)
        = {-pose.position.x, -pose.position.y, -pose.position.z};
    const auto* projection = reinterpret_cast<const float*>(reinterpret_cast<std::uintptr_t>(renderer)
        + bedrocktools::sdk::offsets::LevelRendererPlayer::mProjectionMatrix);
    const auto clip = cameraClipMatrix(projection, localView.data());
    const auto setFrustum = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::FrustumCullerSetFromMatrix);
    if (setFrustum) {
        using SetFrustumFn = void(*)(void*, const float*);
        reinterpret_cast<SetFrustumFn>(setFrustum)(reinterpret_cast<void*>(
            reinterpret_cast<std::uintptr_t>(renderer)
                + bedrocktools::sdk::offsets::LevelRendererPlayer::mFrustumCuller), clip.data());
        bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(renderer,
            bedrocktools::sdk::offsets::LevelRendererPlayer::mFrustumOrigin) = pose.position;
    }
    bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(
        renderer, bedrocktools::sdk::offsets::LevelRendererPlayer::mCameraPosition
    ) = pose.position;
    bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(
        renderer, bedrocktools::sdk::offsets::LevelRendererPlayer::mCameraPositionInterpolated
    ) = pose.position;
    bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(
        renderer, bedrocktools::sdk::offsets::LevelRendererPlayer::mCameraPositionPrevious
    ) = pose.position;
    bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(
        renderer, bedrocktools::sdk::offsets::LevelRendererPlayer::mCameraForward
    ) = basis.forward;
    bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(
        renderer, bedrocktools::sdk::offsets::LevelRendererPlayer::mCameraForwardNext
    ) = basis.forward;
    bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(
        renderer, bedrocktools::sdk::offsets::LevelRendererPlayer::mCameraUp
    ) = basis.up;
    bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(
        renderer, bedrocktools::sdk::offsets::LevelRendererPlayer::mCameraUpNext
    ) = basis.up;
}

void applyCameraPose(void* renderer, void* camera, const CameraPose& pose) {
    if (!renderer || !camera) return;

    writeNativeCameraPose(camera, {}, cameraBasis(pose.yaw, pose.pitch, pose.roll));
    applyRendererPose(renderer, pose);
}

}

std::filesystem::path cinematicCameraPath(const std::filesystem::path& replayPath) {
    auto result = replayPath;
    result.replace_extension(".replaycamera");
    return result;
}

std::vector<std::uint32_t> cinematicTimeKeyframes(const std::filesystem::path& replayPath) {
    std::ifstream input(cinematicCameraPath(replayPath), std::ios::binary);
    std::array<char, 8> magic{};
    std::uint32_t version{};
    std::uint32_t count{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!input || magic != CameraMagic || !readValue(input, version) || !readValue(input, count)
        || count > MaximumKeyframes) {
        return {};
    }
    if (version == SingleTimeKeyframeVersion) {
        std::uint32_t timeKeyframe{NoTimeKeyframe};
        if (!readValue(input, timeKeyframe) || timeKeyframe == NoTimeKeyframe) return {};
        return {timeKeyframe};
    }
    if (version != CameraVersion) return {};
    std::uint32_t timeCount{};
    if (!readValue(input, timeCount) || timeCount > MaximumTimeKeyframes) return {};
    std::vector<std::uint32_t> result(timeCount);
    for (auto& tick : result) {
        if (!readValue(input, tick)) return {};
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

CinematicCamera& CinematicCamera::get() {
    static CinematicCamera camera;
    return camera;
}

bool CinematicCamera::beginSession(const std::filesystem::path& replayPath, std::uint32_t durationTicks) {
    std::scoped_lock lock(mMutex);
    mPath.clear();
    mReplayPath = replayPath;
    mDurationTicks = durationTicks;
    mSelected.reset();
    mTimeKeyframes.clear();
    mSelectedTimeKeyframe.reset();
    mPreviewMode = PreviewMode::Replay;
    mResumeRequested = false;
    mObservedCamera.reset();
    mRenderedCamera.reset();
    mEditorMotion.reset();
    mEditorCamera.reset();
    mPlayerPerspective = PlayerPerspective::Free;
    mSelectedPlayer.reset();
    mPlayerCamera.reset();
    mPlayerRuntimeId = 0;
    mPlayerFramePrepared = false;
    mSessionActive = !replayPath.empty();
    if (!mSessionActive) {
        mStatus = "Camera editor has no replay";
        return false;
    }
    return loadLocked();
}

void CinematicCamera::endSession() {
    std::scoped_lock lock(mMutex);
    mPath.clear();
    mReplayPath.clear();
    mDurationTicks = 0;
    mSelected.reset();
    mTimeKeyframes.clear();
    mSelectedTimeKeyframe.reset();
    mPreviewMode = PreviewMode::Replay;
    mResumeRequested = false;
    mObservedCamera.reset();
    mRenderedCamera.reset();
    mEditorMotion.reset();
    mEditorCamera.reset();
    mPlayerPerspective = PlayerPerspective::Free;
    mSelectedPlayer.reset();
    mPlayerCamera.reset();
    mPlayerRuntimeId = 0;
    mPlayerFramePrepared = false;
    mSessionActive = false;
    mStatus = "Camera editor ready";
}

bool CinematicCamera::sessionActive() const {
    std::scoped_lock lock(mMutex);
    return mSessionActive;
}

std::string CinematicCamera::status() const {
    std::scoped_lock lock(mMutex);
    return mStatus;
}

std::vector<CameraKeyframe> CinematicCamera::keyframes() const {
    std::scoped_lock lock(mMutex);
    return mPath.keyframes();
}

std::optional<std::size_t> CinematicCamera::selectedIndex() const {
    std::scoped_lock lock(mMutex);
    return mSelected;
}

void CinematicCamera::select(std::optional<std::size_t> index) {
    std::scoped_lock lock(mMutex);
    if (index && *index >= mPath.keyframes().size()) index.reset();
    mSelected = index;
}

bool CinematicCamera::captureCurrentKeyframe() {
    CameraKeyframe keyframe;
    keyframe.tick = ReplayPlayer::get().currentTick();
    {
        std::scoped_lock lock(mMutex);
        if (!mSessionActive) return false;
        if (!mObservedCamera) {
            mStatus = "Camera is not available yet";
            return false;
        }
        keyframe.position = mObservedCamera->position;
        keyframe.yaw = mObservedCamera->yaw;
        keyframe.pitch = mObservedCamera->pitch;
        keyframe.roll = mObservedCamera->roll;
        keyframe.fov = mObservedCamera->fov;
        keyframe.dimension = mObservedCamera->dimension;
        keyframe.tick = std::min(keyframe.tick, mDurationTicks);
        mSelected = mPath.upsert(keyframe);
        if (!saveLocked()) return false;
        mStatus = "Camera keyframe saved";
    }
    return true;
}

bool CinematicCamera::updateSelected(CameraKeyframe keyframe) {
    std::scoped_lock lock(mMutex);
    if (!mSessionActive || !mSelected) return false;
    keyframe.tick = std::min(keyframe.tick, mDurationTicks);
    std::size_t newIndex{};
    if (!mPath.update(*mSelected, keyframe, &newIndex)) return false;
    mSelected = newIndex;
    if (!saveLocked()) return false;
    mStatus = "Camera keyframe updated";
    return true;
}

bool CinematicCamera::deleteSelected() {
    std::scoped_lock lock(mMutex);
    if (!mSessionActive || !mSelected || !mPath.erase(*mSelected)) return false;
    if (mPath.keyframes().empty()) mSelected.reset();
    else mSelected = std::min(*mSelected, mPath.keyframes().size() - 1);
    if (!saveLocked()) return false;
    mStatus = "Camera keyframe deleted";
    return true;
}

std::vector<std::uint32_t> CinematicCamera::timeKeyframes() const {
    std::scoped_lock lock(mMutex);
    return mTimeKeyframes;
}

std::optional<std::size_t> CinematicCamera::selectedTimeKeyframeIndex() const {
    std::scoped_lock lock(mMutex);
    return mSelectedTimeKeyframe;
}

void CinematicCamera::selectTimeKeyframe(std::optional<std::size_t> index) {
    std::scoped_lock lock(mMutex);
    if (index && *index >= mTimeKeyframes.size()) index.reset();
    mSelectedTimeKeyframe = index;
}

bool CinematicCamera::captureCurrentTimeKeyframe() {
    const auto currentTick = ReplayPlayer::get().currentTick();
    std::scoped_lock lock(mMutex);
    if (!mSessionActive || mDurationTicks == 0) return false;
    const auto tick = std::min(currentTick, mDurationTicks);
    const auto found = std::lower_bound(mTimeKeyframes.begin(), mTimeKeyframes.end(), tick);
    if (found != mTimeKeyframes.end() && *found == tick) {
        mSelectedTimeKeyframe = static_cast<std::size_t>(found - mTimeKeyframes.begin());
        mStatus = "Time keyframe already exists here";
        return true;
    }
    const auto inserted = mTimeKeyframes.insert(found, tick);
    mSelectedTimeKeyframe = static_cast<std::size_t>(inserted - mTimeKeyframes.begin());
    if (!saveLocked()) return false;
    mStatus = "Time keyframe added";
    return true;
}

bool CinematicCamera::deleteSelectedTimeKeyframe() {
    std::scoped_lock lock(mMutex);
    if (!mSessionActive || !mSelectedTimeKeyframe
        || *mSelectedTimeKeyframe >= mTimeKeyframes.size()) return false;
    mTimeKeyframes.erase(mTimeKeyframes.begin() + static_cast<std::ptrdiff_t>(*mSelectedTimeKeyframe));
    if (mTimeKeyframes.empty()) mSelectedTimeKeyframe.reset();
    else mSelectedTimeKeyframe = std::min(*mSelectedTimeKeyframe, mTimeKeyframes.size() - 1);
    if (!saveLocked()) return false;
    mStatus = "Time keyframe deleted";
    return true;
}

CinematicCamera::PreviewMode CinematicCamera::previewMode() const {
    std::scoped_lock lock(mMutex);
    return mPreviewMode;
}

void CinematicCamera::enterReplayPreview() {
    std::scoped_lock lock(mMutex);
    if (mPreviewMode == PreviewMode::Cinematic) {
        mEditorMotion.reset();
        mEditorCamera = mRenderedCamera;
    }
    mPreviewMode = PreviewMode::Replay;
    mResumeRequested = false;
    mPlayerFramePrepared = false;
    mStatus = mPlayerPerspective == PlayerPerspective::Free
        ? "Replay preview: free camera enabled" : "Replay preview: recorded player view";
}

bool CinematicCamera::toggleCinematicPreview() {
    std::uint32_t firstTick{};
    std::uint32_t lastTick{};
    bool wasCinematic{};
    {
        std::scoped_lock lock(mMutex);
        if (!mSessionActive || mPath.keyframes().empty()) {
            mStatus = "Add a camera keyframe first";
            return false;
        }
        wasCinematic = mPreviewMode == PreviewMode::Cinematic;
        mPreviewMode = PreviewMode::Cinematic;
        firstTick = mPath.keyframes().front().tick;
        lastTick = mPath.keyframes().back().tick;
    }
    auto& replay = ReplayPlayer::get();
    if (wasCinematic && replay.state() == ReplayPlayer::State::Playing) {
        replay.pause();
        std::scoped_lock lock(mMutex);
        mResumeRequested = false;
        mStatus = "Cinematic preview paused";
        return true;
    }
    if (replay.state() == ReplayPlayer::State::Playing) {
        std::scoped_lock lock(mMutex);
        mResumeRequested = false;
        mStatus = "Cinematic preview playing";
        return true;
    }
    const auto current = replay.currentTick();
    if (replay.state() == ReplayPlayer::State::Finished || current < firstTick || current >= lastTick) {
        if (!replay.seek(firstTick)) return false;
        std::scoped_lock lock(mMutex);
        mResumeRequested = true;
        mStatus = "Starting cinematic preview";
        return true;
    }
    const bool resumed = replay.resume();
    std::scoped_lock lock(mMutex);
    mResumeRequested = false;
    mStatus = resumed ? "Cinematic preview playing" : "Cinematic preview is waiting for the replay";
    return resumed;
}

bool CinematicCamera::seekCinematic(std::uint32_t tick) {
    {
        std::scoped_lock lock(mMutex);
        if (!mSessionActive || mPath.keyframes().empty()) return false;
        mPreviewMode = PreviewMode::Cinematic;
        tick = std::min(tick, mDurationTicks);
        mStatus = "Seeking cinematic preview";
    }
    return ReplayPlayer::get().seek(tick);
}

bool CinematicCamera::cinematicPlaying() const {
    std::scoped_lock lock(mMutex);
    return mPreviewMode == PreviewMode::Cinematic
        && ReplayPlayer::get().state() == ReplayPlayer::State::Playing;
}

bool CinematicCamera::blocksManualInput() const {
    std::scoped_lock lock(mMutex);
    return mSessionActive && ReplayPlayer::get().renderReady()
        && ((mPreviewMode == PreviewMode::Cinematic && !mPath.keyframes().empty())
        || mPlayerPerspective != PlayerPerspective::Free);
}

PlayerPerspective CinematicCamera::playerPerspective() const {
    std::scoped_lock lock(mMutex);
    return mPlayerPerspective;
}

std::optional<PlayerViewId> CinematicCamera::selectedPlayer() const {
    std::scoped_lock lock(mMutex);
    return mSelectedPlayer;
}

void CinematicCamera::selectPlayerView(const PlayerViewId& id, PlayerPerspective perspective) {
    if (perspective == PlayerPerspective::Free) {
        enterFreeCamera();
        return;
    }
    std::scoped_lock lock(mMutex);
    if (!mSessionActive) return;
    mPreviewMode = PreviewMode::Replay;
    mResumeRequested = false;
    mSelectedPlayer = id;
    mPlayerPerspective = perspective;
    mPlayerCamera = mRenderedCamera;
    mPlayerRuntimeId = 0;
    mPlayerFramePrepared = false;
    mEditorMotion.reset();
    mEditorCamera.reset();
    mStatus = "Recorded player view";
}

void CinematicCamera::enterFreeCamera() {
    std::scoped_lock lock(mMutex);
    mPreviewMode = PreviewMode::Replay;
    mResumeRequested = false;
    mPlayerPerspective = PlayerPerspective::Free;
    mPlayerCamera.reset();
    mPlayerRuntimeId = 0;
    mPlayerFramePrepared = false;
    mEditorMotion.reset();
    mEditorCamera = mRenderedCamera;
    mStatus = "Replay preview: free camera enabled";
}

void CinematicCamera::beginRenderFrame() {
    ReplayPlayer::get().beginPlayerViewFrame();
    std::scoped_lock lock(mMutex);
    mPlayerFramePrepared = false;
    mPlayerRuntimeId = 0;
}

std::uint64_t CinematicCamera::firstPersonRenderTarget() const {
    std::scoped_lock lock(mMutex);
    return shouldFollowLocked(exporterApplyingCamera()) && mPlayerPerspective == PlayerPerspective::FirstPerson
        ? mPlayerRuntimeId : 0;
}

std::uint64_t CinematicCamera::firstPersonRenderTarget(const bedrocktools::sdk::Vec3* origin) const {
    std::scoped_lock lock(mMutex);
    if (!origin || !finite(*origin) || !mRenderedCamera || !shouldFollowLocked(exporterApplyingCamera())
        || mPlayerPerspective != PlayerPerspective::FirstPerson) return 0;
    const bedrocktools::sdk::Vec3 delta{
        origin->x - mRenderedCamera->position.x,
        origin->y - mRenderedCamera->position.y,
        origin->z - mRenderedCamera->position.z
    };
    return dot(delta, delta) <= 0.0625f ? mPlayerRuntimeId : 0;
}

bool CinematicCamera::shouldFollowLocked(bool exporting) const {
    return mSessionActive && ReplayPlayer::get().renderReady() && !exporting && mPreviewMode == PreviewMode::Replay
        && mPlayerPerspective != PlayerPerspective::Free && mSelectedPlayer.has_value();
}

std::optional<CameraPose> CinematicCamera::playerCameraLocked() {
    if (mPlayerFramePrepared) return mPlayerCamera;
    mPlayerFramePrepared = true;
    mPlayerRuntimeId = 0;
    if (!mSelectedPlayer) return std::nullopt;
    auto& replay = ReplayPlayer::get();
    const auto view = replay.playerView(*mSelectedPlayer);
    if (!view) {
        mStatus = "Selected player is not present at this replay time";
        return mPlayerCamera;
    }
    CameraPose pose{view->eye, view->yaw, view->pitch, 0, mLastNativeFov, currentDimension()};
    {
        const auto forward = cameraBasis(view->yaw, view->pitch, 0).forward;
        const float distance = mPlayerPerspective == PlayerPerspective::FirstPerson ? 0.35f
            : mPlayerPerspective == PlayerPerspective::ThirdPersonBack ? -4.0f : 4.0f;
        const bedrocktools::sdk::Vec3 desired{
            view->eye.x + forward.x * distance,
            view->eye.y + forward.y * distance,
            view->eye.z + forward.z * distance
        };
        pose.position = replay.clipPlayerCamera(view->eye, desired);
        if (mPlayerPerspective == PlayerPerspective::ThirdPersonFront) {
            pose.yaw = std::remainder(view->yaw + 180.0f, 360.0f);
            pose.pitch = -view->pitch;
        }
    }
    mPlayerRuntimeId = view->runtimeId;
    mPlayerCamera = pose;
    mObservedCamera = pose;
    mStatus = "Recorded player view";
    return pose;
}

CinematicCamera::MarkerView CinematicCamera::markerView() const {
    std::scoped_lock lock(mMutex);
    if (!mSessionActive) return {};
    return {mRenderedCamera, mPath.keyframes(), mSelected};
}

void CinematicCamera::onPlayerTick(bedrocktools::sdk::Player* player) {
    bool resume = false;
    bool stopAtEnd = false;
    {
        std::scoped_lock lock(mMutex);
        if (mResumeRequested && !ReplayPlayer::get().seeking()
            && ReplayPlayer::get().state() == ReplayPlayer::State::Paused) {
            mResumeRequested = false;
            resume = true;
        }
        if (!ReplayExporter::get().active() && mSessionActive
            && mPreviewMode == PreviewMode::Cinematic && !mPath.keyframes().empty()
            && ReplayPlayer::get().state() == ReplayPlayer::State::Playing
            && ReplayPlayer::get().currentTick() >= mPath.keyframes().back().tick) {
            stopAtEnd = true;
        }
    }
    if (resume) ReplayPlayer::get().resume();
    if (stopAtEnd) {
        ReplayPlayer::get().pause();
        std::scoped_lock lock(mMutex);
        mStatus = "Cinematic preview complete";
    }
    if (blocksManualInput() || ReplayUi::get().advancedViewportActive()) {
        if (auto* input = bedrocktools::sdk::moveInputComponent(player)) *input = MoveInputComponent{};
    }
}

void CinematicCamera::applyCamera(void* renderer, void* camera) {
    if (!renderer || !camera || !ReplayPlayer::get().rendererActive(renderer)) return;
    const bool exporting = exporterApplyingCamera();

    const auto movement = ReplayUi::get().advancedCameraMovement();
    std::optional<CameraPose> pose;
    {
        std::scoped_lock lock(mMutex);
        if (shouldApplyLocked(exporting)) {
            mEditorMotion.reset();
            mEditorCamera.reset();
            pose = mPath.sample(ReplayPlayer::get().renderTick(), currentDimension());
        } else if (shouldFollowLocked(exporting)) {
            mEditorMotion.reset();
            pose = playerCameraLocked();
            if (!pose) pose = mRenderedCamera;
        } else {
            auto observed = observedCameraPose(renderer, camera, mLastNativeFov);
            if (mSessionActive && (movement || mEditorCamera) && (observed || mEditorCamera)) {
                auto editor = observed ? *observed : *mEditorCamera;

                const auto seed = mEditorCamera ? mEditorCamera->position : editor.position;
                editor.position = mEditorMotion.advance(seed, editor.yaw,
                    movement.value_or(bedrocktools::sdk::Vec3{}), EditorCameraMotion::Clock::now());
                mEditorCamera = editor;
                pose = editor;
                observed = editor;
            }
            mObservedCamera = observed;
        }
        mRenderedCamera = pose ? pose : mObservedCamera;
    }
    if (pose) applyCameraPose(renderer, camera, *pose);
}

void CinematicCamera::applyRendererCamera(void* renderer, void* renderCameraComponent) {
    if (!renderer || !ReplayPlayer::get().rendererActive(renderer)) return;
    std::optional<CameraPose> pose;
    {
        std::scoped_lock lock(mMutex);
        if (shouldApplyLocked(exporterApplyingCamera())) {
            pose = mPath.sample(ReplayPlayer::get().renderTick(), currentDimension());
        } else if (shouldFollowLocked(exporterApplyingCamera())) {
            pose = playerCameraLocked();
            if (!pose) pose = mRenderedCamera;
        } else if (mSessionActive && mEditorCamera) {

            pose = mEditorCamera;
        }
        if (pose) mRenderedCamera = pose;
    }
    if (pose) {

        applyRendererPose(renderer, *pose, renderCameraComponent == nullptr);
        if (renderCameraComponent) {
            bedrocktools::sdk::field<bedrocktools::sdk::Vec3>(renderCameraComponent,
                bedrocktools::sdk::offsets::RenderCameraComponent::mPosition) = pose->position;
        }
    }
}

void CinematicCamera::applyLevelCamera(void* levelRenderer) {
    if (!levelRenderer || !ReplayPlayer::get().rendererActive(levelRenderer, true)) return;
    auto* renderer = bedrocktools::sdk::field<void*>(levelRenderer,
        bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    applyRendererCamera(renderer);
}

void CinematicCamera::releaseEditorCamera() {
    std::scoped_lock lock(mMutex);
    mPlayerPerspective = PlayerPerspective::Free;
    mSelectedPlayer.reset();
    mPlayerCamera.reset();
    mPlayerRuntimeId = 0;
    mPlayerFramePrepared = false;
    mEditorMotion.reset();
    mEditorCamera.reset();
}

float CinematicCamera::overrideFov(float nativeFov) {
    observeNativeFov(nativeFov);
    const bool exporting = exporterApplyingCamera();
    std::scoped_lock lock(mMutex);
    if (!shouldApplyLocked(exporting)) return nativeFov;
    const auto pose = mPath.sample(ReplayPlayer::get().renderTick(), currentDimension());
    return pose ? std::clamp(pose->fov, 1.0f, 179.0f) : nativeFov;
}

void CinematicCamera::observeNativeFov(float nativeFov) {
    if (!std::isfinite(nativeFov) || nativeFov < 1.0f || nativeFov > 179.0f) return;
    std::scoped_lock lock(mMutex);
    if (!shouldApplyLocked(exporterApplyingCamera())) mLastNativeFov = nativeFov;
}

bool CinematicCamera::loadLocked() {
    const auto path = cinematicCameraPath(mReplayPath);
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        mStatus = "No camera path yet";
        return true;
    }
    std::ifstream input(path, std::ios::binary);
    std::array<char, 8> magic{};
    std::uint32_t version{};
    std::uint32_t count{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!input || magic != CameraMagic || !readValue(input, version)
        || (version != LegacyCameraVersion && version != SingleTimeKeyframeVersion && version != CameraVersion)
        || !readValue(input, count) || count > MaximumKeyframes) {
        mStatus = "Camera path is invalid or unsupported";
        return false;
    }
    if (version == SingleTimeKeyframeVersion) {
        std::uint32_t timeKeyframe{NoTimeKeyframe};
        if (!readValue(input, timeKeyframe)) {
            mStatus = "Camera path is truncated";
            return false;
        }
        if (timeKeyframe != NoTimeKeyframe && mDurationTicks > 0) {
            mTimeKeyframes.push_back(std::min(timeKeyframe, mDurationTicks));
        }
    } else if (version == CameraVersion) {
        std::uint32_t timeCount{};
        if (!readValue(input, timeCount) || timeCount > MaximumTimeKeyframes) {
            mStatus = "Camera path is invalid or unsupported";
            return false;
        }
        mTimeKeyframes.resize(timeCount);
        for (auto& tick : mTimeKeyframes) {
            if (!readValue(input, tick)) {
                mStatus = "Camera path is truncated";
                return false;
            }
            tick = std::min(tick, mDurationTicks);
        }
        std::sort(mTimeKeyframes.begin(), mTimeKeyframes.end());
        mTimeKeyframes.erase(std::unique(mTimeKeyframes.begin(), mTimeKeyframes.end()), mTimeKeyframes.end());
    }
    std::vector<CameraKeyframe> keyframes;
    keyframes.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        CameraKeyframe keyframe;
        std::uint8_t interpolation{};
        std::array<std::uint8_t, 3> reserved{};
        if (!readValue(input, keyframe.tick) || !readValue(input, keyframe.dimension)
            || !readValue(input, keyframe.position.x) || !readValue(input, keyframe.position.y)
            || !readValue(input, keyframe.position.z) || !readValue(input, keyframe.yaw)
            || !readValue(input, keyframe.pitch) || !readValue(input, keyframe.roll)
            || !readValue(input, keyframe.fov) || !readValue(input, interpolation)
            || !readValue(input, reserved) || !readValue(input, keyframe.bezierControl1.x)
            || !readValue(input, keyframe.bezierControl1.y) || !readValue(input, keyframe.bezierControl2.x)
            || !readValue(input, keyframe.bezierControl2.y)) {
            mStatus = "Camera path is truncated";
            return false;
        }
        keyframe.tick = std::min(keyframe.tick, mDurationTicks);
        keyframe.interpolation = static_cast<CameraInterpolation>(interpolation);
        keyframes.push_back(keyframe);
    }
    mPath.setKeyframes(std::move(keyframes));
    if (!mPath.keyframes().empty()) mSelected = 0;
    if (!mTimeKeyframes.empty()) mSelectedTimeKeyframe = 0;
    mStatus = "Loaded " + std::to_string(mPath.keyframes().size()) + " camera keyframes";
    return true;
}

bool CinematicCamera::saveLocked() {
    const auto path = cinematicCameraPath(mReplayPath);
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    const auto count = static_cast<std::uint32_t>(mPath.keyframes().size());
    const auto timeCount = static_cast<std::uint32_t>(mTimeKeyframes.size());
    output.write(CameraMagic.data(), static_cast<std::streamsize>(CameraMagic.size()));
    if (!output || !writeValue(output, CameraVersion) || !writeValue(output, count)
        || !writeValue(output, timeCount)) {
        mStatus = "Unable to save camera path";
        return false;
    }
    for (const auto tick : mTimeKeyframes) {
        if (!writeValue(output, tick)) {
            output.close();
            std::error_code cleanupError;
            std::filesystem::remove(temporary, cleanupError);
            mStatus = "Unable to save camera path";
            return false;
        }
    }
    for (const auto& keyframe : mPath.keyframes()) {
        const auto interpolation = static_cast<std::uint8_t>(keyframe.interpolation);
        const std::array<std::uint8_t, 3> reserved{};
        if (!writeValue(output, keyframe.tick) || !writeValue(output, keyframe.dimension)
            || !writeValue(output, keyframe.position.x) || !writeValue(output, keyframe.position.y)
            || !writeValue(output, keyframe.position.z) || !writeValue(output, keyframe.yaw)
            || !writeValue(output, keyframe.pitch) || !writeValue(output, keyframe.roll)
            || !writeValue(output, keyframe.fov) || !writeValue(output, interpolation)
            || !writeValue(output, reserved) || !writeValue(output, keyframe.bezierControl1.x)
            || !writeValue(output, keyframe.bezierControl1.y) || !writeValue(output, keyframe.bezierControl2.x)
            || !writeValue(output, keyframe.bezierControl2.y)) {
            output.close();
            std::error_code cleanupError;
            std::filesystem::remove(temporary, cleanupError);
            mStatus = "Unable to save camera path";
            return false;
        }
    }
    output.close();
    if (!output) {
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        mStatus = "Unable to save camera path";
        return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        mStatus = "Unable to replace camera path";
        return false;
    }
    return true;
}

bool CinematicCamera::shouldApplyLocked(bool exporting) const {
    return mSessionActive && ReplayPlayer::get().renderReady() && !mPath.keyframes().empty()
        && (mPreviewMode == PreviewMode::Cinematic || exporting);
}

}

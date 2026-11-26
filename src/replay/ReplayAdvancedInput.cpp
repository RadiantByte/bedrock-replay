#include "ReplayUi.hpp"
#include "CinematicCamera.hpp"
#include "ReplayPlayer.hpp"
#include "ReplayExporter.hpp"
#include "runtime/InputBridge.hpp"
#include <bedrocktools/sdk/input/MoveInput.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace bedrocktools::replay {
namespace {
bool inside(float x, float y, float left, float top, float right, float bottom) {
    return x >= left && x <= right && y >= top && y <= bottom;
}

std::pair<float, float> joystickMove(float x, float y, float left, float top, float right, float bottom) {
    const float cx = (left + right) * 0.5f;
    const float cy = (top + bottom) * 0.5f;
    const float radius = std::max(1.0f, (right - left) * 0.42f);
    const float dx = (x - cx) / radius;
    const float dy = (cy - y) / radius;
    const float distance = std::hypot(dx, dy);
    const float amount = std::max(1.0f, distance);
    return {dx / amount, dy / amount};
}
}

void ReplayUi::releaseAdvancedScene() {
    if (mAdvancedSceneFramebuffer) glDeleteFramebuffers(1, &mAdvancedSceneFramebuffer);
    if (mAdvancedSceneTexture) glDeleteTextures(1, &mAdvancedSceneTexture);
    mAdvancedSceneFramebuffer = mAdvancedSceneTexture = 0;
    mAdvancedSceneWidth = mAdvancedSceneHeight = 0;
    mAdvancedSceneSourceWidth = mAdvancedSceneSourceHeight = 0;
    mAdvancedSceneSurface = nullptr;
    mAdvancedSceneDirectCopy = mAdvancedSceneCopyChecked = false;
}

void ReplayUi::captureAdvancedScene(int width, int height) {
    if (width <= 0 || height <= 0) return;
    auto* surface = reinterpret_cast<void*>(eglGetCurrentSurface(EGL_DRAW));
    if (surface == EGL_NO_SURFACE) return;
    GLint readBuffer = 0, drawBuffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readBuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawBuffer);
    const auto withTexture = [&](auto&& operation) {
        GLint activeTexture = 0, oldTexture = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
        glBindTexture(GL_TEXTURE_2D, mAdvancedSceneTexture);
        operation();
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture));
        glActiveTexture(static_cast<GLenum>(activeTexture));
    };
    if (!mAdvancedSceneTexture || mAdvancedSceneSourceWidth != width || mAdvancedSceneSourceHeight != height
        || mAdvancedSceneSurface != surface) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        GLint samples = 0;
        glGetIntegerv(GL_SAMPLES, &samples);
        const float scale = samples > 0 ? 1.0f : std::min({1.0f, 1280.0f / width, 720.0f / height});
        mAdvancedSceneWidth = std::max(1, static_cast<int>(std::lround(width * scale)));
        mAdvancedSceneHeight = std::max(1, static_cast<int>(std::lround(height * scale)));
        if (!mAdvancedSceneTexture) glGenTextures(1, &mAdvancedSceneTexture);
        if (mAdvancedSceneTexture) {
            withTexture([&] {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mAdvancedSceneWidth, mAdvancedSceneHeight,
                    0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            });
            if (!mAdvancedSceneFramebuffer) glGenFramebuffers(1, &mAdvancedSceneFramebuffer);
            mAdvancedSceneDirectCopy = !mAdvancedSceneFramebuffer;
            if (mAdvancedSceneFramebuffer) {
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mAdvancedSceneFramebuffer);
                glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                    GL_TEXTURE_2D, mAdvancedSceneTexture, 0);
                mAdvancedSceneDirectCopy = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE;
            }
            mAdvancedSceneSourceWidth = width;
            mAdvancedSceneSourceHeight = height;
            mAdvancedSceneSurface = surface;
            mAdvancedSceneCopyChecked = false;
        }
    }
    if (mAdvancedSceneTexture) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        if (!mAdvancedSceneDirectCopy) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mAdvancedSceneFramebuffer);
            const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
            if (scissor) glDisable(GL_SCISSOR_TEST);
            if (!mAdvancedSceneCopyChecked) {
                for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i) {}
            }
            const GLenum filter = mAdvancedSceneWidth == width && mAdvancedSceneHeight == height ? GL_NEAREST : GL_LINEAR;
            glBlitFramebuffer(0, 0, width, height, 0, 0, mAdvancedSceneWidth, mAdvancedSceneHeight,
                GL_COLOR_BUFFER_BIT, filter);
            if (!mAdvancedSceneCopyChecked) {
                mAdvancedSceneDirectCopy = glGetError() != GL_NO_ERROR;
                mAdvancedSceneCopyChecked = true;
            }
            if (scissor) glEnable(GL_SCISSOR_TEST);
        }
        if (mAdvancedSceneDirectCopy) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawBuffer));
            withTexture([&] {
                if (mAdvancedSceneWidth != width || mAdvancedSceneHeight != height) {
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                    mAdvancedSceneWidth = width;
                    mAdvancedSceneHeight = height;
                }
                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
            });
        }
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readBuffer));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawBuffer));
}

bool ReplayUi::onAdvancedTouch(const pl::input::TouchEvent& event) {
    if (!advancedViewportActive()) return false;
    const bool down = event.action == 0;
    const bool up = event.action == 1 || event.action == 3;
    const bool move = event.action == 2;
    std::scoped_lock lock(mMutex);
    if (event.pointerId == mAdvancedToolPointer) {
        if (up) {
            const bool swiped = mAdvancedToolSwiping;
            mAdvancedToolPointer = -1;
            mAdvancedToolSwiping = false;
            return swiped;
        }
        if (move) {
            const float delta = event.y - mAdvancedToolLastY;
            if (!mAdvancedToolSwiping && mAdvancedToolTab != 4 && std::abs(delta) > 6.0f
                && mReplayUiPointer == event.pointerId) {
                PendingInput release;
                release.type = PendingInput::Type::Touch;
                release.a = 1;
                release.b = event.pointerId;
                release.x = release.y = -100.0f;
                mPendingInput.emplace_back(std::move(release));
                mReplayUiPointer = -1;
                mAdvancedToolSwiping = true;
            }
            if (mAdvancedToolSwiping) {
                mAdvancedToolScrollDelta -= delta;
                mAdvancedToolLastY = event.y;
                return true;
            }
        }
        return false;
    }
    if (down && mAdvancedToolPointer < 0 && inside(event.x, event.y, mAdvancedToolsRect.minX, mAdvancedToolsRect.minY,
        mAdvancedToolsRect.maxX, mAdvancedToolsRect.maxY)) {
        mAdvancedToolPointer = event.pointerId;
        mAdvancedToolLastY = event.y;
        mAdvancedToolSwiping = false;
        return false;
    }
    if (event.pointerId == mAdvancedMovePointer) {
        if (up) {
            mAdvancedMovePointer = -1;
            mAdvancedMoveX = mAdvancedMoveY = 0;
        } else if (move) {
            const auto [x, y] = joystickMove(event.x, event.y, mAdvancedMoveRect.minX,
                mAdvancedMoveRect.minY, mAdvancedMoveRect.maxX, mAdvancedMoveRect.maxY);
            mAdvancedMoveX = x;
            mAdvancedMoveY = y;
        }
        return true;
    }
    if (event.pointerId == mAdvancedLiftPointer) {
        if (up) {
            mAdvancedLiftPointer = -1;
            mAdvancedLift = 0;
        } else if (move) {
            mAdvancedLift = inside(event.x, event.y, mAdvancedAscendRect.minX,
                mAdvancedAscendRect.minY, mAdvancedAscendRect.maxX, mAdvancedAscendRect.maxY) ? 1.f
                : inside(event.x, event.y, mAdvancedDescendRect.minX,
                    mAdvancedDescendRect.minY, mAdvancedDescendRect.maxX, mAdvancedDescendRect.maxY) ? -1.f : 0.f;
        }
        return true;
    }
    if (event.pointerId == mAdvancedLookPointer) {
        if (up) {
            mAdvancedLookPointer = -1;
        } else if (move) {
            const float unit = std::max(1.0f, (mAdvancedViewportRect.maxY - mAdvancedViewportRect.minY) / 420.0f);
            mAdvancedLookX += std::clamp((event.x - mAdvancedLastLookX) / unit, -60.0f, 60.0f);
            mAdvancedLookY += std::clamp((event.y - mAdvancedLastLookY) / unit, -60.0f, 60.0f);
            mAdvancedLastLookX = event.x;
            mAdvancedLastLookY = event.y;
        }
        return true;
    }
    if (!down || !inside(event.x, event.y, mAdvancedViewportRect.minX, mAdvancedViewportRect.minY,
        mAdvancedViewportRect.maxX, mAdvancedViewportRect.maxY)) return false;
    if (CinematicCamera::get().blocksManualInput()) return true;
    if (mAdvancedMovePointer < 0 && inside(event.x, event.y, mAdvancedMoveRect.minX,
        mAdvancedMoveRect.minY, mAdvancedMoveRect.maxX, mAdvancedMoveRect.maxY)) {
        mAdvancedMovePointer = event.pointerId;
        const auto [x, y] = joystickMove(event.x, event.y, mAdvancedMoveRect.minX,
            mAdvancedMoveRect.minY, mAdvancedMoveRect.maxX, mAdvancedMoveRect.maxY);
        mAdvancedMoveX = x;
        mAdvancedMoveY = y;
        return true;
    }
    if (mAdvancedLiftPointer < 0 && inside(event.x, event.y, mAdvancedAscendRect.minX,
        mAdvancedAscendRect.minY, mAdvancedAscendRect.maxX, mAdvancedAscendRect.maxY)) {
        mAdvancedLiftPointer = event.pointerId;
        mAdvancedLift = 1.f;
        return true;
    }
    if (mAdvancedLiftPointer < 0 && inside(event.x, event.y, mAdvancedDescendRect.minX,
        mAdvancedDescendRect.minY, mAdvancedDescendRect.maxX, mAdvancedDescendRect.maxY)) {
        mAdvancedLiftPointer = event.pointerId;
        mAdvancedLift = -1.f;
        return true;
    }
    if (mAdvancedLookPointer < 0 && inside(event.x, event.y, mAdvancedLookRect.minX,
        mAdvancedLookRect.minY, mAdvancedLookRect.maxX, mAdvancedLookRect.maxY)) {
        mAdvancedLookPointer = event.pointerId;
        mAdvancedLastLookX = event.x;
        mAdvancedLastLookY = event.y;
        return true;
    }
    return true;
}

std::optional<bedrocktools::sdk::Vec3> ReplayUi::advancedCameraMovement() {
    if (!advancedViewportActive() || CinematicCamera::get().blocksManualInput()
        || ReplayExporter::get().active()) return std::nullopt;
    const bool suspended = mModalVisible.load(std::memory_order_acquire) || ReplayPlayer::get().seeking();
    std::scoped_lock lock(mMutex);

    if (suspended) return bedrocktools::sdk::Vec3{};
    return bedrocktools::sdk::Vec3{mAdvancedMoveX, mAdvancedLift, mAdvancedMoveY};
}

void ReplayUi::applyAdvancedCameraInput(bedrocktools::sdk::Player* player) {
    if (!player || !advancedViewportActive() || CinematicCamera::get().blocksManualInput()
        || ReplayExporter::get().active()) {
        mAdvancedLookPlayer.store(nullptr, std::memory_order_release);
        return;
    }
    mAdvancedLookPlayer.store(player, std::memory_order_release);

    if (auto* input = bedrocktools::sdk::moveInputComponent(player)) *input = MoveInputComponent{};
    applyAdvancedLookInput(player);
}

void ReplayUi::clearAdvancedLookPlayer() {
    mAdvancedLookPlayer.store(nullptr, std::memory_order_release);
    std::scoped_lock lock(mMutex);
    mAdvancedLookX = mAdvancedLookY = 0.0f;
}

void ReplayUi::applyAdvancedLookInput(bedrocktools::sdk::Player* player) {

    if (!advancedViewportActive() || mModalVisible.load(std::memory_order_acquire)
        || CinematicCamera::get().blocksManualInput()
        || ReplayExporter::get().active()) {
        mAdvancedLookPlayer.store(nullptr, std::memory_order_release);
        std::scoped_lock lock(mMutex);
        mAdvancedLookX = mAdvancedLookY = 0.0f;
        return;
    }
    if (!player) player = mAdvancedLookPlayer.load(std::memory_order_acquire);
    if (!player) return;
    const auto address = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LocalPlayerApplyTurnDelta);
    if (!address) return;
    float lookX, lookY;
    {
        std::scoped_lock lock(mMutex);
        lookX = std::exchange(mAdvancedLookX, 0.0f);
        lookY = std::exchange(mAdvancedLookY, 0.0f);
    }
    if (lookX != 0 || lookY != 0) {
        using ApplyTurnDeltaFn = void(*)(void*, bedrocktools::sdk::Vec2*);
        const float amount = 0.17f * mAdvancedLookSensitivity.load(std::memory_order_acquire);
        bedrocktools::sdk::Vec2 delta{-lookY * amount, lookX * amount};
        reinterpret_cast<ApplyTurnDeltaFn>(address)(player, &delta);
    }
}
}

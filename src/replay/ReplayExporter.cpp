#include "ReplayExporter.hpp"

#include "CinematicCamera.hpp"
#include "ReplayPlayer.hpp"
#include "ReplayRecorder.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <system_error>
#include <vector>

#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace bedrocktools::replay {

namespace {
constexpr std::int64_t FinishTimeoutNs = 10000000000ll;

bool isPlayerActive(ReplayPlayer::State state) {
    return state == ReplayPlayer::State::StartingWorld
        || state == ReplayPlayer::State::LoadingSnapshot
        || state == ReplayPlayer::State::ResettingSnapshot
        || state == ReplayPlayer::State::Playing
        || state == ReplayPlayer::State::Paused
        || state == ReplayPlayer::State::Finished
        || state == ReplayPlayer::State::Stopping;
}
}

struct ReplayExporter::Impl {
#if defined(__ANDROID__)
    struct PendingSample {
        std::vector<std::uint8_t> bytes;
        AMediaCodecBufferInfo info{};
    };

    AMediaCodec* codec{};
    AMediaMuxer* muxer{};
    ANativeWindow* inputWindow{};
    int fd{-1};
    ssize_t track{-1};
    bool muxerStarted{};
    bool eosSignaled{};
    bool eosReceived{};

    EGLDisplay display{EGL_NO_DISPLAY};
    EGLContext gameContext{EGL_NO_CONTEXT};
    EGLSurface gameDrawSurface{EGL_NO_SURFACE};
    EGLSurface gameReadSurface{EGL_NO_SURFACE};
    EGLContext encoderContext{EGL_NO_CONTEXT};
    EGLSurface encoderSurface{EGL_NO_SURFACE};
    PFNEGLPRESENTATIONTIMEANDROIDPROC presentationTime{};

    GLuint captureTexture{};
    GLuint captureFramebuffer{};
    GLuint program{};
    GLuint vertexArray{};
    GLint textureUniform{-1};
    GLint uvScaleUniform{-1};
    GLint uvOffsetUniform{-1};
    int width{};
    int height{};
    int sourceWidth{};
    int sourceHeight{};

    std::chrono::steady_clock::time_point finishStart{};
    std::uint64_t writtenSamples{};
    std::vector<PendingSample> pendingSamples;
#endif
};

ReplayExporter& ReplayExporter::get() {
    static ReplayExporter exporter;
    return exporter;
}

ReplayExporter::ReplayExporter() : mImpl(std::make_unique<Impl>()) {}
ReplayExporter::~ReplayExporter() {
    std::scoped_lock lock(mMutex);
    releaseLocked(false);
}

std::filesystem::path ReplayExporter::makeOutputPathLocked(const std::filesystem::path& replayPath) const {
    auto directory = ReplayRecorder::get().libraryDirectory().parent_path() / "replay_exports";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto stem = replayPath.stem().string();
    if (stem.empty()) stem = "replay";
    return directory / (stem + "_" + std::to_string(now) + ".mp4");
}

bool ReplayExporter::begin(const std::filesystem::path& replayPath) {
    return begin(replayPath, mSettings);
}

bool ReplayExporter::begin(const std::filesystem::path& replayPath, Settings settings) {
    const auto timeKeyframes = cinematicTimeKeyframes(replayPath);
    {
        std::scoped_lock lock(mMutex);
        if (active()) return false;
        if (timeKeyframes.size() < 2 || timeKeyframes.front() >= timeKeyframes.back()) {
            mStatus = "Add at least two time keyframes before exporting";
            mState = State::Failed;
            return false;
        }
        if (isPlayerActive(ReplayPlayer::get().state())) {
            mStatus = "Stop the current replay before exporting";
            mState = State::Failed;
            return false;
        }
        releaseLocked(false);
        mReplayPath = replayPath;
        settings.width = std::clamp(settings.width & ~1, 320, 3840);
        settings.height = std::clamp(settings.height & ~1, 320, 3840);
        settings.frameRate = std::clamp(settings.frameRate, 24, 60);
        settings.bitrate = std::clamp<std::int32_t>(settings.bitrate, 1000000, 50000000);
        mSettings = settings;
        mStartTick = timeKeyframes.front();
        mEndTick = timeKeyframes.back();
        mCapturedFrames = 0;
        mTotalFrames = 0;
        mLastProgressPercent = -1;
        mStartSeekRequested = false;
        mOutputPath = makeOutputPathLocked(replayPath);
        mReturnToBrowser = false;
        mRestorePlaybackSpeed = false;
        mStatus = "Starting export between the time keyframes";
        mState = State::WaitingForReplay;
    }
    if (!ReplayPlayer::get().open(replayPath)) {
        std::scoped_lock lock(mMutex);
        failLocked("Unable to start replay for export: " + ReplayPlayer::get().status());
        return false;
    }
    ReplayPlayer::get().setPlaybackSpeed(1.0f);
    return true;
}

bool ReplayExporter::beginCurrent(Settings settings) {
    const auto replayPath = ReplayPlayer::get().replayPath();
    const auto playerState = ReplayPlayer::get().state();
    const float previousPlaybackSpeed = ReplayPlayer::get().playbackSpeed();
    const auto timeKeyframes = CinematicCamera::get().timeKeyframes();
    if (replayPath.empty() || (playerState != ReplayPlayer::State::Playing
            && playerState != ReplayPlayer::State::Paused
            && playerState != ReplayPlayer::State::Finished)) return false;
    {
        std::scoped_lock lock(mMutex);
        if (active()) return false;
        if (timeKeyframes.size() < 2 || timeKeyframes.front() >= timeKeyframes.back()) {
            mStatus = "Add at least two time keyframes before exporting";
            mState = State::Failed;
            return false;
        }
        releaseLocked(false);
        mReplayPath = replayPath;
        settings.width = std::clamp(settings.width & ~1, 320, 3840);
        settings.height = std::clamp(settings.height & ~1, 320, 3840);
        settings.frameRate = std::clamp(settings.frameRate, 24, 60);
        settings.bitrate = std::clamp<std::int32_t>(settings.bitrate, 1000000, 50000000);
        mSettings = settings;
        mStartTick = timeKeyframes.front();
        mEndTick = timeKeyframes.back();
        mCapturedFrames = 0;
        mTotalFrames = 0;
        mLastProgressPercent = -1;
        mStartSeekRequested = true;
        mOutputPath = makeOutputPathLocked(replayPath);
        mReturnToBrowser = false;
        mPreviousPlaybackSpeed = previousPlaybackSpeed;
        mRestorePlaybackSpeed = true;
        mStatus = "Preparing the selected time-keyframe range";
        mState = State::WaitingForReplay;
    }
    ReplayPlayer::get().setPlaybackSpeed(1.0f);
    if (!ReplayPlayer::get().rewindForExport(mStartTick)) {
        std::scoped_lock lock(mMutex);
        failLocked("Unable to reset replay for export");
        return false;
    }
    return true;
}

void ReplayExporter::cancel() {
    std::scoped_lock lock(mMutex);
    if (!active()) return;
    mStatus = "Cancelling export";
    mState = State::Cancelling;
}

ReplayExporter::State ReplayExporter::state() const {
    std::scoped_lock lock(mMutex);
    return mState;
}

bool ReplayExporter::active() const {
    std::scoped_lock lock(mMutex);
    return mState == State::WaitingForReplay || mState == State::Exporting || mState == State::Finishing || mState == State::Cancelling;
}

std::string ReplayExporter::status() const {
    std::scoped_lock lock(mMutex);
    return mStatus;
}

std::filesystem::path ReplayExporter::outputPath() const {
    std::scoped_lock lock(mMutex);
    return mOutputPath;
}

ReplayExporter::Settings ReplayExporter::settings() const {
    std::scoped_lock lock(mMutex);
    return mSettings;
}

std::uint32_t ReplayExporter::startTick() const {
    std::scoped_lock lock(mMutex);
    return mStartTick;
}

std::uint32_t ReplayExporter::endTick() const {
    std::scoped_lock lock(mMutex);
    return mEndTick;
}

float ReplayExporter::progress() const {
    std::scoped_lock lock(mMutex);
    if (mState == State::Finishing || mState == State::Completed) return 1.0f;
    if (mTotalFrames == 0) return 0.0f;
    return std::clamp(
        static_cast<float>(mCapturedFrames) / static_cast<float>(mTotalFrames),
        0.0f,
        1.0f
    );
}

bool ReplayExporter::consumeReturnToBrowser() {
    std::scoped_lock lock(mMutex);
    if (!mReturnToBrowser) return false;
    mReturnToBrowser = false;
    return true;
}

#if defined(__ANDROID__)
namespace {
using MediaCodecCreateInputSurfaceFn = media_status_t (*)(AMediaCodec*, ANativeWindow**);
using MediaCodecSignalEndOfInputStreamFn = media_status_t (*)(AMediaCodec*);

MediaCodecCreateInputSurfaceFn resolveCreateInputSurface() {
    auto name = "AMediaCodec_createInputSurface";
    return reinterpret_cast<MediaCodecCreateInputSurfaceFn>(dlsym(RTLD_DEFAULT, name));
}

MediaCodecSignalEndOfInputStreamFn resolveSignalEndOfInputStream() {
    auto name = "AMediaCodec_signalEndOfInputStream";
    return reinterpret_cast<MediaCodecSignalEndOfInputStreamFn>(dlsym(RTLD_DEFAULT, name));
}

#ifndef EGL_RECORDABLE_ANDROID
#define EGL_RECORDABLE_ANDROID 0x3142
#endif
#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#endif
constexpr std::int32_t ColorFormatSurface = 0x7F000789;

GLuint compileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint createCopyProgram() {
    static constexpr const char* Vertex = R"(#version 300 es
out vec2 vUv;
void main() {
    const vec2 positions[4] = vec2[4](vec2(-1.0,-1.0), vec2(1.0,-1.0), vec2(-1.0,1.0), vec2(1.0,1.0));
    const vec2 uvs[4] = vec2[4](vec2(0.0,0.0), vec2(1.0,0.0), vec2(0.0,1.0), vec2(1.0,1.0));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
    vUv = uvs[gl_VertexID];
}
)";
    static constexpr const char* Fragment = R"(#version 300 es
precision mediump float;
in vec2 vUv;
uniform sampler2D uTexture;
uniform vec2 uUvScale;
uniform vec2 uUvOffset;
out vec4 fragColor;
void main() { fragColor = texture(uTexture, vUv * uUvScale + uUvOffset); }
)";
    const GLuint vs = compileShader(GL_VERTEX_SHADER, Vertex);
    if (!vs) return 0;
    const GLuint fs = compileShader(GL_FRAGMENT_SHADER, Fragment);
    if (!fs) {
        glDeleteShader(vs);
        return 0;
    }
    const GLuint program = glCreateProgram();
    if (!program) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

bool createCaptureFramebuffer(GLuint texture, GLuint& framebuffer) {
    GLint oldDrawFramebuffer = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFramebuffer);
    while (glGetError() != GL_NO_ERROR) {}
    GLuint created = 0;
    glGenFramebuffers(1, &created);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, created);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    const bool complete = created != 0
        && glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE
        && glGetError() == GL_NO_ERROR;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(oldDrawFramebuffer));
    if (!complete) {
        if (created) glDeleteFramebuffers(1, &created);
        return false;
    }
    framebuffer = created;
    return true;
}
}
#endif

bool ReplayExporter::initializeEncoderLocked(int sourceWidth, int sourceHeight) {
#if !defined(__ANDROID__)
    (void)sourceWidth;
    (void)sourceHeight;
    failLocked("Video export requires Android MediaCodec");
    return false;
#else
    sourceWidth &= ~1;
    sourceHeight &= ~1;
    const int width = mSettings.width;
    const int height = mSettings.height;
    if (sourceWidth < 2 || sourceHeight < 2 || width < 2 || height < 2 || mOutputPath.empty()) {
        failLocked("Invalid export surface size");
        return false;
    }

    auto& impl = *mImpl;
    impl.display = eglGetCurrentDisplay();
    impl.gameContext = eglGetCurrentContext();
    impl.gameDrawSurface = eglGetCurrentSurface(EGL_DRAW);
    impl.gameReadSurface = eglGetCurrentSurface(EGL_READ);
    if (impl.display == EGL_NO_DISPLAY || impl.gameContext == EGL_NO_CONTEXT
        || impl.gameDrawSurface == EGL_NO_SURFACE) {
        failLocked("No active EGL context for export");
        return false;
    }

    impl.fd = ::open(mOutputPath.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0644);
    if (impl.fd < 0) {
        failLocked("Unable to create MP4 output");
        return false;
    }
    impl.muxer = AMediaMuxer_new(impl.fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    if (!impl.muxer) {
        failLocked("Unable to create MP4 muxer");
        return false;
    }

    impl.codec = AMediaCodec_createEncoderByType("video/avc");
    if (!impl.codec) {
        failLocked("H.264 encoder is unavailable");
        return false;
    }
    AMediaFormat* format = AMediaFormat_new();
    if (!format) {
        failLocked("Unable to create encoder format");
        return false;
    }
    AMediaFormat_setString(format, "mime", "video/avc");
    AMediaFormat_setInt32(format, "width", width);
    AMediaFormat_setInt32(format, "height", height);
    AMediaFormat_setInt32(format, "bitrate", mSettings.bitrate);
    AMediaFormat_setInt32(format, "frame-rate", mSettings.frameRate);
    AMediaFormat_setInt32(format, "i-frame-interval", 1);
    AMediaFormat_setInt32(format, "color-format", ColorFormatSurface);
    const media_status_t configured = AMediaCodec_configure(impl.codec, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(format);
    if (configured != AMEDIA_OK) {
        failLocked("Unable to configure H.264 encoder");
        return false;
    }
    const auto createInputSurface = resolveCreateInputSurface();
    if (!createInputSurface) {
        failLocked("MP4 export requires Android 8.0 or newer");
        return false;
    }
    if (createInputSurface(impl.codec, &impl.inputWindow) != AMEDIA_OK || !impl.inputWindow) {
        failLocked("Unable to create encoder input surface");
        return false;
    }
    if (AMediaCodec_start(impl.codec) != AMEDIA_OK) {
        failLocked("Unable to start H.264 encoder");
        return false;
    }

    EGLint count = 0;
    EGLConfig config{};
    const EGLint configAttributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RECORDABLE_ANDROID, EGL_TRUE,
        EGL_NONE
    };
    if (!eglChooseConfig(impl.display, configAttributes, &config, 1, &count) || count < 1) {
        failLocked("No recordable EGL configuration");
        return false;
    }
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    impl.encoderContext = eglCreateContext(impl.display, config, impl.gameContext, contextAttributes);
    if (impl.encoderContext == EGL_NO_CONTEXT) {
        failLocked("Unable to create encoder EGL context");
        return false;
    }
    impl.encoderSurface = eglCreateWindowSurface(impl.display, config, impl.inputWindow, nullptr);
    if (impl.encoderSurface == EGL_NO_SURFACE) {
        failLocked("Unable to create encoder EGL surface");
        return false;
    }
    impl.presentationTime = reinterpret_cast<PFNEGLPRESENTATIONTIMEANDROIDPROC>(eglGetProcAddress("eglPresentationTimeANDROID"));
    if (!impl.presentationTime) {
        failLocked("EGL presentation timestamps are unavailable");
        return false;
    }

    glGenTextures(1, &impl.captureTexture);
    if (!impl.captureTexture) {
        failLocked("Unable to allocate capture texture");
        return false;
    }
    GLint oldActive = GL_TEXTURE0;
    GLint oldTexture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    while (glGetError() != GL_NO_ERROR) {}
    glBindTexture(GL_TEXTURE_2D, impl.captureTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sourceWidth, sourceHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    const GLenum textureError = glGetError();
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture));
    glActiveTexture(static_cast<GLenum>(oldActive));
    if (textureError != GL_NO_ERROR) {
        failLocked("Unable to allocate export capture storage");
        return false;
    }
    if (!createCaptureFramebuffer(impl.captureTexture, impl.captureFramebuffer)) {
        failLocked("Unable to create export capture framebuffer");
        return false;
    }

    const EGLSurface oldDraw = eglGetCurrentSurface(EGL_DRAW);
    const EGLSurface oldRead = eglGetCurrentSurface(EGL_READ);
    const EGLContext oldContext = eglGetCurrentContext();
    if (!eglMakeCurrent(impl.display, impl.encoderSurface, impl.encoderSurface, impl.encoderContext)) {
        failLocked("Unable to activate encoder EGL context");
        return false;
    }
    impl.program = createCopyProgram();
    if (impl.program) {
        impl.textureUniform = glGetUniformLocation(impl.program, "uTexture");
        impl.uvScaleUniform = glGetUniformLocation(impl.program, "uUvScale");
        impl.uvOffsetUniform = glGetUniformLocation(impl.program, "uUvOffset");
        glGenVertexArrays(1, &impl.vertexArray);
    }
    eglMakeCurrent(impl.display, oldDraw, oldRead, oldContext);
    if (!impl.program || !impl.vertexArray) {
        failLocked("Unable to create export copy shader");
        return false;
    }

    impl.width = width;
    impl.height = height;
    impl.sourceWidth = sourceWidth;
    impl.sourceHeight = sourceHeight;
    impl.writtenSamples = 0;
    impl.pendingSamples.clear();

    const auto rangeTicks = static_cast<std::uint64_t>(mEndTick - mStartTick);
    const auto frameRate = static_cast<std::uint64_t>(std::max(1, mSettings.frameRate));
    mTotalFrames = std::max<std::uint64_t>(1, (rangeTicks * frameRate + 10u) / 20u);
    mCapturedFrames = 0;
    mLastProgressPercent = -1;
    if (!ReplayPlayer::get().beginExportClock(mStartTick)) {
        failLocked("Unable to start the fixed export clock");
        return false;
    }
    mState = State::Exporting;
    mStatus = "Saving the selected time-keyframe range";
    return true;
#endif
}

bool ReplayExporter::drainLocked(bool waitForOutput) {
#if !defined(__ANDROID__)
    (void)waitForOutput;
    return false;
#else
    auto& impl = *mImpl;
    if (!impl.codec) return false;
    for (;;) {
        AMediaCodecBufferInfo info{};
        const ssize_t index = AMediaCodec_dequeueOutputBuffer(impl.codec, &info, waitForOutput ? 10000 : 0);
        if (index >= 0) {
            std::uint8_t* buffer = AMediaCodec_getOutputBuffer(impl.codec, static_cast<std::size_t>(index), nullptr);
            const bool hasPayload = info.size > 0 && (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0;
            if (hasPayload && (!buffer || info.offset < 0)) {
                AMediaCodec_releaseOutputBuffer(impl.codec, static_cast<std::size_t>(index), false);
                failLocked("H.264 encoder returned an invalid output buffer");
                return false;
            }
            if (hasPayload) {
                if (!impl.muxerStarted || impl.track < 0) {
                    Impl::PendingSample sample;
                    sample.bytes.assign(buffer + info.offset, buffer + info.offset + info.size);
                    sample.info = info;
                    sample.info.offset = 0;
                    impl.pendingSamples.push_back(std::move(sample));
                } else if (AMediaMuxer_writeSampleData(
                        impl.muxer,
                        static_cast<std::size_t>(impl.track),
                        buffer,
                        &info
                    ) != AMEDIA_OK) {
                    AMediaCodec_releaseOutputBuffer(impl.codec, static_cast<std::size_t>(index), false);
                    failLocked("MP4 muxer write failed");
                    return false;
                } else {
                    ++impl.writtenSamples;
                }
            }
            if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) impl.eosReceived = true;
            AMediaCodec_releaseOutputBuffer(impl.codec, static_cast<std::size_t>(index), false);
            if (impl.eosReceived) return true;
            continue;
        }
        if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            if (impl.muxerStarted) {
                failLocked("Encoder output format changed twice");
                return false;
            }
            AMediaFormat* outputFormat = AMediaCodec_getOutputFormat(impl.codec);
            if (!outputFormat) {
                failLocked("Unable to read encoder output format");
                return false;
            }
            impl.track = AMediaMuxer_addTrack(impl.muxer, outputFormat);
            AMediaFormat_delete(outputFormat);
            if (impl.track < 0 || AMediaMuxer_start(impl.muxer) != AMEDIA_OK) {
                failLocked("Unable to start MP4 muxer");
                return false;
            }
            impl.muxerStarted = true;
            for (auto& sample : impl.pendingSamples) {
                if (AMediaMuxer_writeSampleData(
                        impl.muxer,
                        static_cast<std::size_t>(impl.track),
                        sample.bytes.data(),
                        &sample.info
                    ) != AMEDIA_OK) {
                    impl.pendingSamples.clear();
                    failLocked("MP4 muxer write failed");
                    return false;
                }
                ++impl.writtenSamples;
            }
            impl.pendingSamples.clear();
            continue;
        }
        return true;
    }
#endif
}

bool ReplayExporter::captureFrameLocked() {
#if !defined(__ANDROID__)
    return false;
#else
    auto& impl = *mImpl;
    if (!impl.codec || !impl.program || impl.encoderContext == EGL_NO_CONTEXT || impl.encoderSurface == EGL_NO_SURFACE) return false;

    const auto frameRate = static_cast<std::uint64_t>(std::max(1, mSettings.frameRate));
    const auto targetFrame = mCapturedFrames;

    const EGLDisplay display = eglGetCurrentDisplay();
    const EGLContext gameContext = eglGetCurrentContext();
    const EGLSurface gameDraw = eglGetCurrentSurface(EGL_DRAW);
    const EGLSurface gameRead = eglGetCurrentSurface(EGL_READ);

    if (display != impl.display || gameContext == EGL_NO_CONTEXT
        || gameDraw != impl.gameDrawSurface || gameRead != impl.gameReadSurface) {
        drainLocked(false);
        return mState != State::Failed;
    }
    if (gameContext != impl.gameContext) {
        if (glIsTexture(impl.captureTexture) != GL_TRUE) {
            drainLocked(false);
            return mState != State::Failed;
        }
        impl.captureFramebuffer = 0;
        if (!createCaptureFramebuffer(impl.captureTexture, impl.captureFramebuffer)) {
            failLocked("Unable to recreate export capture framebuffer");
            return false;
        }
        impl.gameContext = gameContext;
    }

    GLint oldActive = GL_TEXTURE0;
    GLint oldTexture = 0;
    GLint oldReadFramebuffer = 0;
    GLint oldDrawFramebuffer = 0;
    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFramebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFramebuffer);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    while (glGetError() != GL_NO_ERROR) {}
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, impl.captureFramebuffer);
    glBlitFramebuffer(
        0, 0, impl.sourceWidth, impl.sourceHeight,
        0, 0, impl.sourceWidth, impl.sourceHeight,
        GL_COLOR_BUFFER_BIT,
        GL_NEAREST
    );
    GLenum copyError = glGetError();
    if (copyError != GL_NO_ERROR) {
        while (glGetError() != GL_NO_ERROR) {}
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(oldReadFramebuffer));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(oldDrawFramebuffer));
        glBindTexture(GL_TEXTURE_2D, impl.captureTexture);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, impl.sourceWidth, impl.sourceHeight);
        copyError = glGetError();
    }
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture));
    glActiveTexture(static_cast<GLenum>(oldActive));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(oldReadFramebuffer));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(oldDrawFramebuffer));
    if (scissorEnabled == GL_TRUE) glEnable(GL_SCISSOR_TEST);
    if (copyError != GL_NO_ERROR) {
        failLocked("Unable to copy the Minecraft backbuffer for export");
        return false;
    }

    glFinish();

    if (!eglMakeCurrent(impl.display, impl.encoderSurface, impl.encoderSurface, impl.encoderContext)) {
        failLocked("Unable to render export frame");
        return false;
    }
    while (glGetError() != GL_NO_ERROR) {}
    glViewport(0, 0, impl.width, impl.height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindVertexArray(impl.vertexArray);
    glUseProgram(impl.program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, impl.captureTexture);
    glUniform1i(impl.textureUniform, 0);
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    const float sourceAspect = static_cast<float>(impl.sourceWidth) / static_cast<float>(impl.sourceHeight);
    const float outputAspect = static_cast<float>(impl.width) / static_cast<float>(impl.height);
    if (sourceAspect > outputAspect) scaleX = outputAspect / sourceAspect;
    else if (sourceAspect < outputAspect) scaleY = sourceAspect / outputAspect;
    glUniform2f(impl.uvScaleUniform, scaleX, scaleY);
    glUniform2f(impl.uvOffsetUniform, (1.0f - scaleX) * 0.5f, (1.0f - scaleY) * 0.5f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const GLenum drawError = glGetError();
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glBindVertexArray(0);
    const auto presentationNs = static_cast<std::int64_t>((targetFrame * 1000000000ull) / frameRate);
    impl.presentationTime(impl.display, impl.encoderSurface, presentationNs);
    const EGLBoolean swapped = eglSwapBuffers(impl.display, impl.encoderSurface);
    const EGLBoolean restored = eglMakeCurrent(impl.display, gameDraw, gameRead, gameContext);
    if (drawError != GL_NO_ERROR || swapped != EGL_TRUE || restored != EGL_TRUE) {
        failLocked("Unable to submit export frame");
        return false;
    }
    ++mCapturedFrames;
    return drainLocked(false) && mState != State::Failed;
#endif
}

void ReplayExporter::startFinishingLocked() {
    ReplayPlayer::get().endExportClock();
#if defined(__ANDROID__)
    auto& impl = *mImpl;
    if (!impl.eosSignaled && impl.codec) {
        if (!drainLocked(false) || mState == State::Failed) return;
        const auto signalEndOfInputStream = resolveSignalEndOfInputStream();
        if (!signalEndOfInputStream) {
            failLocked("MP4 export requires Android 8.0 or newer");
            return;
        }
        if (signalEndOfInputStream(impl.codec) != AMEDIA_OK) {
            failLocked("Unable to finish H.264 stream");
            return;
        }
        impl.eosSignaled = true;
        impl.finishStart = std::chrono::steady_clock::now();
    }
#endif
    mState = State::Finishing;
    mStatus = "Finishing MP4";
}

void ReplayExporter::completeLocked() {
#if defined(__ANDROID__)
    if (!mImpl->muxerStarted || mImpl->writtenSamples == 0) {
        failLocked("H.264 encoder produced no playable video frames");
        return;
    }
#endif
    const auto output = mOutputPath;
    const bool restoreSpeed = mRestorePlaybackSpeed;
    const float previousSpeed = mPreviousPlaybackSpeed;
    mRestorePlaybackSpeed = false;
    releaseLocked(false);
    mOutputPath = output;
    mState = State::Completed;
    mStatus = "Exported " + output.filename().string();
    ReplayPlayer::get().pause();
    if (restoreSpeed) ReplayPlayer::get().setPlaybackSpeed(previousSpeed);
}

void ReplayExporter::failLocked(std::string message) {
    const bool restoreSpeed = mRestorePlaybackSpeed;
    const float previousSpeed = mPreviousPlaybackSpeed;
    mRestorePlaybackSpeed = false;
    ReplayPlayer::get().endExportClock();
    releaseLocked(true);
    mState = State::Failed;
    mStatus = std::move(message);
    ReplayPlayer::get().pause();
    if (restoreSpeed) ReplayPlayer::get().setPlaybackSpeed(previousSpeed);
}

void ReplayExporter::releaseLocked(bool removeOutput) {
#if defined(__ANDROID__)
    auto& impl = *mImpl;
    if (impl.codec) {
        AMediaCodec_stop(impl.codec);
        AMediaCodec_delete(impl.codec);
        impl.codec = nullptr;
    }
    if (impl.muxer) {
        if (impl.muxerStarted) AMediaMuxer_stop(impl.muxer);
        AMediaMuxer_delete(impl.muxer);
        impl.muxer = nullptr;
    }
    impl.muxerStarted = false;
    impl.track = -1;

    if (impl.display != EGL_NO_DISPLAY) {
        const EGLContext oldContext = eglGetCurrentContext();
        const EGLSurface oldDraw = eglGetCurrentSurface(EGL_DRAW);
        const EGLSurface oldRead = eglGetCurrentSurface(EGL_READ);
        if (impl.captureFramebuffer && oldContext == impl.gameContext) {
            glDeleteFramebuffers(1, &impl.captureFramebuffer);
            impl.captureFramebuffer = 0;
        }
        if (impl.encoderContext != EGL_NO_CONTEXT && impl.encoderSurface != EGL_NO_SURFACE
            && eglMakeCurrent(impl.display, impl.encoderSurface, impl.encoderSurface, impl.encoderContext)) {
            if (impl.vertexArray) glDeleteVertexArrays(1, &impl.vertexArray);
            impl.vertexArray = 0;
            if (impl.program) glDeleteProgram(impl.program);
            impl.program = 0;
            if (impl.captureTexture) glDeleteTextures(1, &impl.captureTexture);
            impl.captureTexture = 0;
            eglMakeCurrent(impl.display, oldDraw, oldRead, oldContext);
        }
        impl.captureFramebuffer = 0;
        impl.vertexArray = 0;
        impl.captureTexture = 0;
        if (impl.encoderSurface != EGL_NO_SURFACE) eglDestroySurface(impl.display, impl.encoderSurface);
        if (impl.encoderContext != EGL_NO_CONTEXT) eglDestroyContext(impl.display, impl.encoderContext);
    }
    impl.encoderSurface = EGL_NO_SURFACE;
    impl.encoderContext = EGL_NO_CONTEXT;
    impl.display = EGL_NO_DISPLAY;
    impl.gameContext = EGL_NO_CONTEXT;
    impl.gameDrawSurface = EGL_NO_SURFACE;
    impl.gameReadSurface = EGL_NO_SURFACE;
    impl.presentationTime = nullptr;
    impl.textureUniform = -1;
    impl.uvScaleUniform = -1;
    impl.uvOffsetUniform = -1;
    impl.width = impl.height = 0;
    impl.sourceWidth = impl.sourceHeight = 0;
    impl.writtenSamples = 0;
    impl.pendingSamples.clear();
    impl.eosSignaled = false;
    impl.eosReceived = false;
    if (impl.inputWindow) {
        ANativeWindow_release(impl.inputWindow);
        impl.inputWindow = nullptr;
    }
    if (impl.fd >= 0) {
        ::close(impl.fd);
        impl.fd = -1;
    }
#endif
    if (removeOutput && !mOutputPath.empty()) {
        std::error_code ec;
        std::filesystem::remove(mOutputPath, ec);
    }
}

void ReplayExporter::frame() {
    std::scoped_lock lock(mMutex);
    if (mState == State::Idle || mState == State::Completed || mState == State::Failed) return;
    if (mState == State::Cancelling) {
        const bool restoreSpeed = mRestorePlaybackSpeed;
        const float previousSpeed = mPreviousPlaybackSpeed;
        mRestorePlaybackSpeed = false;
        ReplayPlayer::get().endExportClock();
        releaseLocked(true);
        mState = State::Failed;
        mStatus = "Export cancelled";
        ReplayPlayer::get().pause();
        if (restoreSpeed) ReplayPlayer::get().setPlaybackSpeed(previousSpeed);
        return;
    }

    const auto playerState = ReplayPlayer::get().state();
    if (playerState == ReplayPlayer::State::Failed) {
        failLocked("Replay failed during export: " + ReplayPlayer::get().status());
        return;
    }

    if (mState == State::WaitingForReplay) {
        if (ReplayPlayer::get().seeking()) return;
        if (!mStartSeekRequested && playerState == ReplayPlayer::State::Playing
            && ReplayPlayer::get().currentTick() < mStartTick) {
            mStartSeekRequested = true;
            if (!ReplayPlayer::get().seek(mStartTick)) {
                failLocked("Unable to seek to the first time keyframe");
            }
            return;
        }
        if (playerState != ReplayPlayer::State::Playing) return;
#if defined(__ANDROID__)
        const EGLDisplay display = eglGetCurrentDisplay();
        const EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
        EGLint width = 0;
        EGLint height = 0;
        if (display == EGL_NO_DISPLAY || eglGetCurrentContext() == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE
            || !eglQuerySurface(display, surface, EGL_WIDTH, &width)
            || !eglQuerySurface(display, surface, EGL_HEIGHT, &height)) {
            failLocked("Unable to query Minecraft render surface");
            return;
        }
        initializeEncoderLocked(width, height);
#else
        initializeEncoderLocked(0, 0);
#endif
        return;
    }

    if (mState == State::Exporting) {
        if (ReplayPlayer::get().seeking()) return;
        if (playerState == ReplayPlayer::State::Finished) {
            startFinishingLocked();
            return;
        }
        if (playerState != ReplayPlayer::State::Playing) return;
        if (!ReplayPlayer::get().exportFrameReady()) {
            drainLocked(false);
            return;
        }
        const int percent = static_cast<int>((mCapturedFrames * 100u) / std::max<std::uint64_t>(1, mTotalFrames));
        if (percent != mLastProgressPercent) {
            mLastProgressPercent = percent;
            mStatus = "Saving replay " + std::to_string(percent) + "%";
        }
        if (!captureFrameLocked() || mState == State::Failed) return;
        if (mCapturedFrames >= mTotalFrames) {
            ReplayPlayer::get().endExportClock();
            ReplayPlayer::get().pause();
            startFinishingLocked();
            return;
        }
        const double nextRenderTick = static_cast<double>(mStartTick)
            + static_cast<double>(mCapturedFrames * 20u) / static_cast<double>(std::max(1, mSettings.frameRate));
        if (!ReplayPlayer::get().queueExportFrame(nextRenderTick)) {
            failLocked("Unable to advance the fixed export clock");
        }
        return;
    }

    if (mState == State::Finishing) {
#if defined(__ANDROID__)
        drainLocked(true);
        if (mState == State::Failed) return;
        if (mImpl->eosReceived) {
            completeLocked();
            return;
        }
        if (std::chrono::steady_clock::now() - mImpl->finishStart > std::chrono::nanoseconds(FinishTimeoutNs)) {
            failLocked("Timed out finishing MP4");
        }
#else
        failLocked("Video export requires Android MediaCodec");
#endif
    }
}

}

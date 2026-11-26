#pragma once

#include <filesystem>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace bedrocktools::replay {

class ReplayExporter {
public:
    enum class State { Idle, WaitingForReplay, Exporting, Finishing, Completed, Failed, Cancelling };

    struct Settings {
        int width{1920};
        int height{1080};
        int frameRate{30};
        std::int32_t bitrate{12000000};
    };

    static ReplayExporter& get();
    ~ReplayExporter();

    bool begin(const std::filesystem::path& replayPath);
    bool begin(const std::filesystem::path& replayPath, Settings settings);
    bool beginCurrent(Settings settings);
    void cancel();
    void frame();

    State state() const;
    bool active() const;
    std::string status() const;
    std::filesystem::path outputPath() const;
    Settings settings() const;
    std::uint32_t startTick() const;
    std::uint32_t endTick() const;
    float progress() const;
    bool consumeReturnToBrowser();

private:
    struct Impl;

    ReplayExporter();
    bool initializeEncoderLocked(int sourceWidth, int sourceHeight);
    bool captureFrameLocked();
    bool drainLocked(bool waitForOutput);
    void startFinishingLocked();
    void completeLocked();
    void failLocked(std::string message);
    void releaseLocked(bool removeOutput);
    std::filesystem::path makeOutputPathLocked(const std::filesystem::path& replayPath) const;

    mutable std::recursive_mutex mMutex;
    std::unique_ptr<Impl> mImpl;
    State mState{State::Idle};
    std::string mStatus{"Exporter ready"};
    std::filesystem::path mReplayPath;
    std::filesystem::path mOutputPath;
    Settings mSettings;
    std::uint32_t mStartTick{};
    std::uint32_t mEndTick{};
    std::uint64_t mCapturedFrames{};
    std::uint64_t mTotalFrames{};
    int mLastProgressPercent{-1};
    bool mStartSeekRequested{};
    bool mReturnToBrowser{};
    float mPreviousPlaybackSpeed{1.0f};
    bool mRestorePlaybackSpeed{};
};

}

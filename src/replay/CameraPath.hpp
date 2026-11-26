#pragma once

#include <bedrocktools/sdk/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bedrocktools::replay {

enum class CameraInterpolation : std::uint8_t {
    Linear,
    Smooth,
    Ease,
    Hold,
    Hermite,
    CubicBezier
};

const char* cameraInterpolationName(CameraInterpolation interpolation);

struct CameraKeyframe {
    std::uint32_t tick{};
    bedrocktools::sdk::Vec3 position{};
    float yaw{};
    float pitch{};
    float roll{};
    float fov{70.0f};
    std::int32_t dimension{};
    CameraInterpolation interpolation{CameraInterpolation::Smooth};
    bedrocktools::sdk::Vec2 bezierControl1{0.42f, 0.0f};
    bedrocktools::sdk::Vec2 bezierControl2{0.58f, 1.0f};
};

struct CameraPose {
    bedrocktools::sdk::Vec3 position{};
    float yaw{};
    float pitch{};
    float roll{};
    float fov{70.0f};
    std::int32_t dimension{};
};

class CameraPath {
public:
    const std::vector<CameraKeyframe>& keyframes() const noexcept;
    void setKeyframes(std::vector<CameraKeyframe> keyframes);
    std::size_t upsert(CameraKeyframe keyframe);
    bool update(std::size_t index, CameraKeyframe keyframe, std::size_t* newIndex = nullptr);
    bool erase(std::size_t index);
    void clear();
    std::optional<CameraPose> sample(double tick, std::int32_t currentDimension) const;

private:
    std::vector<CameraKeyframe> mKeyframes;
};

}

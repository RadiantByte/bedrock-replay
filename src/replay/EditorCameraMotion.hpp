#pragma once

#include <bedrocktools/sdk/Types.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>

namespace bedrocktools::replay {

class EditorCameraMotion {
public:
    using Clock = std::chrono::steady_clock;

    void reset() { mPosition.reset(); }

    bedrocktools::sdk::Vec3 advance(const bedrocktools::sdk::Vec3& nativePosition,
        float yaw, const bedrocktools::sdk::Vec3& input, Clock::time_point now) {
        if (!mPosition) {
            mPosition = nativePosition;
            mLastUpdate = now;
            return *mPosition;
        }

        const float seconds = std::clamp(std::chrono::duration<float>(now - mLastUpdate).count(), 0.0f, 0.05f);
        mLastUpdate = now;
        if (input.x == 0 && input.y == 0 && input.z == 0) return *mPosition;
        constexpr float radiansPerDegree = 3.14159265358979323846f / 180.0f;
        constexpr float blocksPerSecond = 10.0f;
        const float sine = std::sin(yaw * radiansPerDegree);
        const float cosine = std::cos(yaw * radiansPerDegree);
        const float distance = blocksPerSecond * seconds;
        mPosition->x += (-cosine * input.x - sine * input.z) * distance;
        mPosition->y += input.y * distance;
        mPosition->z += (-sine * input.x + cosine * input.z) * distance;
        return *mPosition;
    }

private:
    std::optional<bedrocktools::sdk::Vec3> mPosition;
    Clock::time_point mLastUpdate{};
};

}

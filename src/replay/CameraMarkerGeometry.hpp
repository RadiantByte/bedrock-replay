#pragma once

#include "CameraPath.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

namespace bedrocktools::replay::markers {

struct Basis { bedrocktools::sdk::Vec3 right, up, forward; };

inline Basis basis(const CameraPose& pose) {
    constexpr float radians = 3.14159265358979323846f / 180.0f;
    const float sy = std::sin(pose.yaw * radians), cy = std::cos(pose.yaw * radians);
    const float sp = std::sin(pose.pitch * radians), cp = std::cos(pose.pitch * radians);
    Basis b{{-cy, 0, -sy}, {-sy * sp, cp, cy * sp}, {-sy * cp, -sp, cy * cp}};
    const float sr = std::sin(pose.roll * radians), cr = std::cos(pose.roll * radians);
    const auto r = b.right, u = b.up;
    b.right = {r.x * cr + u.x * sr, r.y * cr + u.y * sr, r.z * cr + u.z * sr};
    b.up = {u.x * cr - r.x * sr, u.y * cr - r.y * sr, u.z * cr - r.z * sr};
    return b;
}

using WorldLine = std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>;

inline std::array<WorldLine, 8> cameraGlyph(const CameraKeyframe& key) {
    const auto b = basis({key.position, key.yaw, key.pitch, key.roll, key.fov, key.dimension});
    constexpr float depth = 0.55f;
    constexpr float radians = 3.14159265358979323846f / 180.0f;
    const float halfHeight = std::clamp(depth * std::tan(std::clamp(key.fov, 5.f, 170.f) * .5f * radians), .12f, .65f);
    const float halfWidth = halfHeight * (16.f / 9.f);
    const auto corner = [&](float x, float y) {
        return bedrocktools::sdk::Vec3{
            key.position.x + b.forward.x * depth + b.right.x * x * halfWidth + b.up.x * y * halfHeight,
            key.position.y + b.forward.y * depth + b.right.y * x * halfWidth + b.up.y * y * halfHeight,
            key.position.z + b.forward.z * depth + b.right.z * x * halfWidth + b.up.z * y * halfHeight};
    };
    const std::array corners{corner(-1, 1), corner(1, 1), corner(1, -1), corner(-1, -1)};
    return {{{key.position, corners[0]}, {key.position, corners[1]},
        {key.position, corners[2]}, {key.position, corners[3]},
        {corners[0], corners[1]}, {corners[1], corners[2]},
        {corners[2], corners[3]}, {corners[3], corners[0]}}};
}

struct ScreenLine { double x1, y1, x2, y2; };

class Projection {
public:
    Projection(const CameraPose& view, float sourceAspect, float cropX = 0, float cropY = 0)
        : mView(view), mBasis(basis(view)) {
        constexpr double radians = 3.14159265358979323846 / 180.0;
        const double focal = 1.0 / std::tan(view.fov * .5 * radians);
        mScaleX = focal / (sourceAspect * (1.0 - 2.0 * cropX));
        mScaleY = focal / (1.0 - 2.0 * cropY);
    }

    std::optional<ScreenLine> line(const WorldLine& world) const {
        const auto a = project(world.first), b = project(world.second);
        if (!finite(a) || !finite(b)) return std::nullopt;

        double first = 0, last = 1;
        const std::array da{a.w - .05, a.w + a.x, a.w - a.x, a.w + a.y, a.w - a.y};
        const std::array db{b.w - .05, b.w + b.x, b.w - b.x, b.w + b.y, b.w - b.y};
        for (std::size_t i = 0; i < da.size(); ++i) {
            if (da[i] < 0 && db[i] < 0) return std::nullopt;
            if (da[i] < 0) first = std::max(first, da[i] / (da[i] - db[i]));
            if (db[i] < 0) last = std::min(last, da[i] / (da[i] - db[i]));
            if (first > last) return std::nullopt;
        }
        const auto lerp = [&](double t) {
            return Clip{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t};
        };
        const auto c = lerp(first), d = lerp(last);
        return ScreenLine{std::clamp(c.x / c.w * .5 + .5, 0.0, 1.0),
            std::clamp(.5 - c.y / c.w * .5, 0.0, 1.0),
            std::clamp(d.x / d.w * .5 + .5, 0.0, 1.0),
            std::clamp(.5 - d.y / d.w * .5, 0.0, 1.0)};
    }

private:
    struct Clip { double x, y, w; };
    static bool finite(Clip p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.w); }
    Clip project(bedrocktools::sdk::Vec3 p) const {
        const double x = double(p.x) - mView.position.x;
        const double y = double(p.y) - mView.position.y;
        const double z = double(p.z) - mView.position.z;
        const auto dot = [&](bedrocktools::sdk::Vec3 v) { return x * v.x + y * v.y + z * v.z; };
        return {dot(mBasis.right) * mScaleX, dot(mBasis.up) * mScaleY, dot(mBasis.forward)};
    }
    CameraPose mView;
    Basis mBasis;
    double mScaleX{}, mScaleY{};
};

}

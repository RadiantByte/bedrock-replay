#include "CameraPath.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bedrocktools::replay {
namespace {

float clampFinite(float value, float fallback, float minimum, float maximum) {
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

float shortestDelta(float from, float to) {
    float delta = std::fmod(to - from, 360.0f);
    if (delta > 180.0f) delta -= 360.0f;
    if (delta < -180.0f) delta += 360.0f;
    return delta;
}

float unwrap(float reference, float value) {
    return reference + shortestDelta(reference, value);
}

float lerp(float left, float right, double amount) {
    return static_cast<float>(left + (right - left) * amount);
}

float lerpAngle(float left, float right, double amount) {
    return static_cast<float>(left + shortestDelta(left, right) * amount);
}

float adjustedInterval(float distance, float tickSpan, float relation) {
    if (tickSpan <= 0.0f || distance <= 0.0f || relation <= 0.0f) return 0.0f;
    const float factor = std::clamp(distance * relation / tickSpan, 0.4f, 2.5f);
    return distance * relation / factor;
}

float catmull(float p0, float p1, float p2, float p3, float time1, float time2,
              float time3, double amount) {
    const float d1 = std::sqrt(std::abs(p1 - p0));
    const float d2 = std::sqrt(std::abs(p2 - p1));
    const float d3 = std::sqrt(std::abs(p3 - p2));
    const float average = (d1 + d2 + d3) / 3.0f;
    if (average <= 0.0f || time3 <= 0.0f) return p1;
    const float relation = (time3 / 3.0f) / average;
    const float t0 = 0.0f;
    const float t1 = adjustedInterval(d1, time1, relation);
    const float t2 = t1 + adjustedInterval(d2, time2 - time1, relation);
    const float t3 = t2 + adjustedInterval(d3, time3 - time2, relation);
    const float t = std::lerp(t1, t2, static_cast<float>(amount));
    const auto blend = [](float left, float right, float from, float to, float at) {
        const float value = from == to ? 0.5f : (at - from) / (to - from);
        return std::lerp(left, right, value);
    };
    const float a1 = blend(p0, p1, t0, t1, t);
    const float a2 = blend(p1, p2, t1, t2, t);
    const float a3 = blend(p2, p3, t2, t3, t);
    const float b1 = blend(a1, a2, t0, t2, t);
    const float b2 = blend(a2, a3, t1, t3, t);
    return blend(b1, b2, t1, t2, t);
}

bedrocktools::sdk::Vec3 catmull(const bedrocktools::sdk::Vec3& p0, const bedrocktools::sdk::Vec3& p1,
                                const bedrocktools::sdk::Vec3& p2, const bedrocktools::sdk::Vec3& p3,
                                float time1, float time2, float time3, double amount) {
    const auto distance = [](const auto& left, const auto& right) {
        const float x = right.x - left.x;
        const float y = right.y - left.y;
        const float z = right.z - left.z;
        return std::sqrt(std::sqrt(x * x + y * y + z * z));
    };
    const float d1 = distance(p0, p1);
    const float d2 = distance(p1, p2);
    const float d3 = distance(p2, p3);
    const float average = (d1 + d2 + d3) / 3.0f;
    if (average <= 0.0f || time3 <= 0.0f) return p1;
    const float relation = (time3 / 3.0f) / average;
    const float t0 = 0.0f;
    const float t1 = adjustedInterval(d1, time1, relation);
    const float t2 = t1 + adjustedInterval(d2, time2 - time1, relation);
    const float t3 = t2 + adjustedInterval(d3, time3 - time2, relation);
    const float t = std::lerp(t1, t2, static_cast<float>(amount));
    const auto blend = [](const auto& left, const auto& right, float from, float to, float at) {
        const float value = from == to ? 0.5f : (at - from) / (to - from);
        return bedrocktools::sdk::Vec3{
            std::lerp(left.x, right.x, value),
            std::lerp(left.y, right.y, value),
            std::lerp(left.z, right.z, value)
        };
    };
    const auto a1 = blend(p0, p1, t0, t1, t);
    const auto a2 = blend(p1, p2, t1, t2, t);
    const auto a3 = blend(p2, p3, t2, t3, t);
    const auto b1 = blend(a1, a2, t0, t2, t);
    const auto b2 = blend(a2, a3, t1, t3, t);
    return blend(b1, b2, t1, t2, t);
}

double cubicBezierCoordinate(double amount, double control1, double control2) {
    const double inverse = 1.0 - amount;
    return 3.0 * inverse * inverse * amount * control1
        + 3.0 * inverse * amount * amount * control2
        + amount * amount * amount;
}

double cubicBezierWeight(double amount, const bedrocktools::sdk::Vec2& control1,
                         const bedrocktools::sdk::Vec2& control2) {
    const double x1 = std::clamp(static_cast<double>(control1.x), 0.0, 1.0);
    const double x2 = std::clamp(static_cast<double>(control2.x), 0.0, 1.0);
    double low = 0.0;
    double high = 1.0;
    for (int iteration = 0; iteration < 24; ++iteration) {
        const double candidate = (low + high) * 0.5;
        if (cubicBezierCoordinate(candidate, x1, x2) < amount) low = candidate;
        else high = candidate;
    }
    return std::clamp(cubicBezierCoordinate((low + high) * 0.5, control1.y, control2.y), 0.0, 1.0);
}

CameraPose poseOf(const CameraKeyframe& keyframe) {
    return {
        keyframe.position,
        keyframe.yaw,
        keyframe.pitch,
        keyframe.roll,
        keyframe.fov,
        keyframe.dimension
    };
}

CameraPose interpolateLinear(const CameraKeyframe& left, const CameraKeyframe& right, double amount) {
    return {
        {
            lerp(left.position.x, right.position.x, amount),
            lerp(left.position.y, right.position.y, amount),
            lerp(left.position.z, right.position.z, amount)
        },
        lerpAngle(left.yaw, right.yaw, amount),
        lerpAngle(left.pitch, right.pitch, amount),
        lerpAngle(left.roll, right.roll, amount),
        lerp(left.fov, right.fov, amount),
        left.dimension
    };
}

CameraPose interpolateSmooth(const CameraKeyframe& before, const CameraKeyframe& left,
                             const CameraKeyframe& right, const CameraKeyframe& after,
                             double amount) {
    const float time1 = static_cast<float>(left.tick - before.tick);
    const float time2 = static_cast<float>(right.tick - before.tick);
    const float time3 = static_cast<float>(after.tick - before.tick);
    const float yaw0 = unwrap(left.yaw, before.yaw);
    const float yaw2 = unwrap(left.yaw, right.yaw);
    const float yaw3 = unwrap(yaw2, after.yaw);
    const float pitch0 = unwrap(left.pitch, before.pitch);
    const float pitch2 = unwrap(left.pitch, right.pitch);
    const float pitch3 = unwrap(pitch2, after.pitch);
    const float roll0 = unwrap(left.roll, before.roll);
    const float roll2 = unwrap(left.roll, right.roll);
    const float roll3 = unwrap(roll2, after.roll);
    return {
        catmull(before.position, left.position, right.position, after.position, time1, time2, time3, amount),
        shortestDelta(0.0f, catmull(yaw0, left.yaw, yaw2, yaw3, time1, time2, time3, amount)),
        shortestDelta(0.0f, catmull(pitch0, left.pitch, pitch2, pitch3, time1, time2, time3, amount)),
        shortestDelta(0.0f, catmull(roll0, left.roll, roll2, roll3, time1, time2, time3, amount)),
        catmull(before.fov, left.fov, right.fov, after.fov, time1, time2, time3, amount),
        left.dimension
    };
}

template <class Value>
double newtonValue(const std::vector<CameraKeyframe>& keys, std::size_t first, std::size_t last,
                   double tick, Value value, bool angular) {
    const std::size_t count = last - first + 1;
    std::vector<double> coefficients(count);
    double previous = static_cast<double>(value(keys[first]));
    coefficients[0] = previous;
    for (std::size_t index = 1; index < count; ++index) {
        double current = static_cast<double>(value(keys[first + index]));
        if (angular) current = previous + shortestDelta(static_cast<float>(previous), static_cast<float>(current));
        coefficients[index] = current;
        previous = current;
    }
    for (std::size_t order = 1; order < count; ++order) {
        for (std::size_t index = count - 1; index >= order; --index) {
            const double denominator = static_cast<double>(keys[first + index].tick)
                - static_cast<double>(keys[first + index - order].tick);
            if (std::abs(denominator) < std::numeric_limits<double>::epsilon()) return coefficients[0];
            coefficients[index] = (coefficients[index] - coefficients[index - 1]) / denominator;
            if (index == order) break;
        }
    }
    double result = coefficients[count - 1];
    for (std::size_t index = count - 1; index-- > 0;) {
        result = result * (tick - static_cast<double>(keys[first + index].tick)) + coefficients[index];
    }
    return result;
}

CameraPose interpolateHermite(const std::vector<CameraKeyframe>& keys, std::size_t segment, double tick) {
    std::size_t first = segment;
    while (first > 0 && keys[first - 1].dimension == keys[segment].dimension
        && keys[first - 1].interpolation == CameraInterpolation::Hermite) --first;
    std::size_t last = segment + 1;
    while (last + 1 < keys.size() && keys[last].dimension == keys[segment].dimension
        && keys[last].interpolation == CameraInterpolation::Hermite) ++last;
    auto value = [&](auto member) {
        return newtonValue(keys, first, last, tick, member, false);
    };
    return {
        {
            static_cast<float>(value([](const auto& key) { return key.position.x; })),
            static_cast<float>(value([](const auto& key) { return key.position.y; })),
            static_cast<float>(value([](const auto& key) { return key.position.z; }))
        },
        static_cast<float>(newtonValue(keys, first, last, tick, [](const auto& key) { return key.yaw; }, true)),
        static_cast<float>(newtonValue(keys, first, last, tick, [](const auto& key) { return key.pitch; }, true)),
        static_cast<float>(newtonValue(keys, first, last, tick, [](const auto& key) { return key.roll; }, true)),
        static_cast<float>(value([](const auto& key) { return key.fov; })),
        keys[segment].dimension
    };
}

bool finiteKeyframe(const CameraKeyframe& keyframe) {
    return std::isfinite(keyframe.position.x) && std::isfinite(keyframe.position.y)
        && std::isfinite(keyframe.position.z) && std::isfinite(keyframe.yaw)
        && std::isfinite(keyframe.pitch) && std::isfinite(keyframe.roll)
        && std::isfinite(keyframe.fov) && std::isfinite(keyframe.bezierControl1.x)
        && std::isfinite(keyframe.bezierControl1.y) && std::isfinite(keyframe.bezierControl2.x)
        && std::isfinite(keyframe.bezierControl2.y);
}

bool finitePose(const CameraPose& pose) {
    return std::isfinite(pose.position.x) && std::isfinite(pose.position.y)
        && std::isfinite(pose.position.z) && std::isfinite(pose.yaw)
        && std::isfinite(pose.pitch) && std::isfinite(pose.roll) && std::isfinite(pose.fov);
}

void sanitize(CameraKeyframe& keyframe) {
    keyframe.fov = clampFinite(keyframe.fov, 70.0f, 1.0f, 179.0f);
    if (static_cast<unsigned>(keyframe.interpolation) > static_cast<unsigned>(CameraInterpolation::CubicBezier)) {
        keyframe.interpolation = CameraInterpolation::Smooth;
    }
    keyframe.bezierControl1.x = clampFinite(keyframe.bezierControl1.x, 0.42f, 0.0f, 1.0f);
    keyframe.bezierControl1.y = clampFinite(keyframe.bezierControl1.y, 0.0f, -2.0f, 2.0f);
    keyframe.bezierControl2.x = clampFinite(keyframe.bezierControl2.x, 0.58f, 0.0f, 1.0f);
    keyframe.bezierControl2.y = clampFinite(keyframe.bezierControl2.y, 1.0f, -2.0f, 2.0f);
}

}

const char* cameraInterpolationName(CameraInterpolation interpolation) {
    switch (interpolation) {
        case CameraInterpolation::Linear: return "Linear";
        case CameraInterpolation::Smooth: return "Smooth";
        case CameraInterpolation::Ease: return "Ease";
        case CameraInterpolation::Hold: return "Hold";
        case CameraInterpolation::Hermite: return "Hermite";
        case CameraInterpolation::CubicBezier: return "Cubic Bezier";
    }
    return "Smooth";
}

const std::vector<CameraKeyframe>& CameraPath::keyframes() const noexcept {
    return mKeyframes;
}

void CameraPath::setKeyframes(std::vector<CameraKeyframe> keyframes) {
    keyframes.erase(std::remove_if(keyframes.begin(), keyframes.end(), [](const auto& keyframe) {
        return !finiteKeyframe(keyframe);
    }), keyframes.end());
    for (auto& keyframe : keyframes) sanitize(keyframe);
    std::stable_sort(keyframes.begin(), keyframes.end(), [](const auto& left, const auto& right) {
        return left.tick < right.tick;
    });
    std::vector<CameraKeyframe> unique;
    unique.reserve(keyframes.size());
    for (auto& keyframe : keyframes) {
        if (!unique.empty() && unique.back().tick == keyframe.tick) unique.back() = keyframe;
        else unique.push_back(keyframe);
    }
    mKeyframes = std::move(unique);
}

std::size_t CameraPath::upsert(CameraKeyframe keyframe) {
    sanitize(keyframe);
    auto found = std::lower_bound(mKeyframes.begin(), mKeyframes.end(), keyframe.tick,
        [](const auto& candidate, std::uint32_t tick) { return candidate.tick < tick; });
    if (found != mKeyframes.end() && found->tick == keyframe.tick) {
        *found = keyframe;
        return static_cast<std::size_t>(found - mKeyframes.begin());
    }
    return static_cast<std::size_t>(mKeyframes.insert(found, keyframe) - mKeyframes.begin());
}

bool CameraPath::update(std::size_t index, CameraKeyframe keyframe, std::size_t* newIndex) {
    if (index >= mKeyframes.size() || !finiteKeyframe(keyframe)) return false;
    mKeyframes.erase(mKeyframes.begin() + static_cast<std::ptrdiff_t>(index));
    const auto inserted = upsert(keyframe);
    if (newIndex) *newIndex = inserted;
    return true;
}

bool CameraPath::erase(std::size_t index) {
    if (index >= mKeyframes.size()) return false;
    mKeyframes.erase(mKeyframes.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

void CameraPath::clear() {
    mKeyframes.clear();
}

std::optional<CameraPose> CameraPath::sample(double tick, std::int32_t currentDimension) const {
    if (mKeyframes.empty() || !std::isfinite(tick)) return std::nullopt;
    if (tick <= static_cast<double>(mKeyframes.front().tick)) {
        return mKeyframes.front().dimension == currentDimension ? std::optional(poseOf(mKeyframes.front())) : std::nullopt;
    }
    if (tick >= static_cast<double>(mKeyframes.back().tick)) {
        return mKeyframes.back().dimension == currentDimension ? std::optional(poseOf(mKeyframes.back())) : std::nullopt;
    }
    const auto rightIterator = std::upper_bound(mKeyframes.begin(), mKeyframes.end(), tick,
        [](double value, const auto& keyframe) { return value < static_cast<double>(keyframe.tick); });
    const std::size_t rightIndex = static_cast<std::size_t>(rightIterator - mKeyframes.begin());
    const std::size_t leftIndex = rightIndex - 1;
    const auto& left = mKeyframes[leftIndex];
    const auto& right = mKeyframes[rightIndex];
    if (left.dimension != right.dimension) {
        if (currentDimension == left.dimension) return poseOf(left);
        if (currentDimension == right.dimension) return poseOf(right);
        return std::nullopt;
    }
    if (currentDimension != left.dimension) return std::nullopt;
    const double duration = static_cast<double>(right.tick - left.tick);
    const double amount = duration <= 0.0 ? 1.0 : std::clamp((tick - left.tick) / duration, 0.0, 1.0);
    if (left.interpolation == CameraInterpolation::Hold) return poseOf(left);
    if (left.interpolation == CameraInterpolation::Hermite) {
        auto pose = interpolateHermite(mKeyframes, leftIndex, tick);
        if (!finitePose(pose)) return interpolateLinear(left, right, amount);
        pose.yaw = shortestDelta(0.0f, pose.yaw);
        pose.pitch = shortestDelta(0.0f, pose.pitch);
        pose.roll = shortestDelta(0.0f, pose.roll);
        pose.fov = std::clamp(pose.fov, 1.0f, 179.0f);
        return pose;
    }
    if (left.interpolation == CameraInterpolation::Smooth) {
        const auto& before = leftIndex > 0 && mKeyframes[leftIndex - 1].dimension == left.dimension
                && mKeyframes[leftIndex - 1].interpolation != CameraInterpolation::Hold
            ? mKeyframes[leftIndex - 1] : left;
        const auto& after = rightIndex + 1 < mKeyframes.size() && mKeyframes[rightIndex + 1].dimension == left.dimension
                && right.interpolation != CameraInterpolation::Hold
            ? mKeyframes[rightIndex + 1] : right;
        auto pose = interpolateSmooth(before, left, right, after, amount);
        if (!finitePose(pose)) return interpolateLinear(left, right, amount);
        pose.fov = std::clamp(pose.fov, 1.0f, 179.0f);
        return pose;
    }
    double weight = amount;
    if (left.interpolation == CameraInterpolation::Ease) weight = amount * amount * (3.0 - 2.0 * amount);
    if (left.interpolation == CameraInterpolation::CubicBezier) {
        weight = cubicBezierWeight(amount, left.bezierControl1, left.bezierControl2);
    }
    return interpolateLinear(left, right, weight);
}

}

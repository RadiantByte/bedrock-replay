#pragma once

#include <bedrocktools/sdk/Types.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

struct ActorDataFlagComponent {
    std::array<std::uint64_t, 3> mFlags;
};

struct ActorDataDirtyFlagsComponent {
    std::array<std::uint64_t, 3> mFlags;
};

struct IsHorizontalPoseFlagComponent {};
struct ShouldUpdateBoundingBoxRequestComponent {};

struct MovementInterpolatorComponent {
    bedrocktools::sdk::Vec3 mPos;
    bedrocktools::sdk::Vec2 mRot;
    float mHeadYaw;
    int mPositionSteps;
    int mRotationSteps;
    int mHeadYawSteps;
    std::byte mTail[4];
};

struct ActorHeadRotationComponent {
    float mYHeadRot;
    float mYHeadRotO;
};

struct MobBodyRotationComponent {
    float mYBodyRot;
    float mYBodyRotO;
};

struct RenderPositionComponent {
    bedrocktools::sdk::Vec3 mValue;
};

struct RenderRotationComponent {
    bedrocktools::sdk::Vec2 mRot;
};

struct OffsetsComponent {
    float mHeightOffset;
    float mExplosionOffset;
    bedrocktools::sdk::Vec3 mHeadOffset;
    bedrocktools::sdk::Vec3 mDropOffset;
    bedrocktools::sdk::Vec3 mEyeOffset;
    bedrocktools::sdk::Vec3 mMouthOffset;
    bedrocktools::sdk::Vec3 mBreathingOffset;
};

static_assert(sizeof(OffsetsComponent) == 0x44);
static_assert(offsetof(OffsetsComponent, mEyeOffset) == 0x20);

static_assert(sizeof(MovementInterpolatorComponent) == 0x28);
static_assert(sizeof(ActorDataFlagComponent) == 0x18);
static_assert(sizeof(ActorDataDirtyFlagsComponent) == 0x18);
static_assert(offsetof(MovementInterpolatorComponent, mPos) == 0x0);
static_assert(offsetof(MovementInterpolatorComponent, mRot) == 0xC);
static_assert(offsetof(MovementInterpolatorComponent, mHeadYaw) == 0x14);
static_assert(offsetof(MovementInterpolatorComponent, mPositionSteps) == 0x18);
static_assert(offsetof(MovementInterpolatorComponent, mRotationSteps) == 0x1C);
static_assert(offsetof(MovementInterpolatorComponent, mHeadYawSteps) == 0x20);
static_assert(offsetof(MovementInterpolatorComponent, mTail) == 0x24);

#pragma once

#include <cstddef>
#include <cstdint>

namespace bedrocktools::sdk::offsets {

namespace Player {
inline constexpr std::size_t mName = 2856;
inline constexpr std::size_t mSkin = 2584;
inline constexpr std::size_t mInventory = 0x590;
}

namespace PlayerInventory {
inline constexpr std::size_t mSelectedSlot = 0x10;
}

namespace Actor {
inline constexpr std::size_t mEntityContext = 0x8;
inline constexpr std::size_t mStateVectorComponent = 0x228;
inline constexpr std::size_t mActorRotationComponent = 0x238;
inline constexpr std::size_t mLevel = 496;
inline constexpr std::size_t mDimension = 480;
inline constexpr std::size_t mHurtTime = 0x194;
inline constexpr std::size_t mCategories = 544;
}

namespace StateVectorComponent {
inline constexpr std::size_t mPos = 0x0;
inline constexpr std::size_t mPosPrev = 0xC;
inline constexpr std::size_t mPosDelta = 0x18;
}

namespace ActorRotationComponent {
inline constexpr std::size_t mRot = 0x0;
inline constexpr std::size_t mRotPrev = 0x8;
}



namespace DataItem {
inline constexpr std::size_t mType = 0x8;
inline constexpr std::size_t mValue = 0xC;
}

namespace BuiltInActorComponents {
inline constexpr std::size_t mAABBShapeComponent = 8;
}

namespace AABBShapeComponent {
inline constexpr std::size_t mAABB = 0;
}

namespace Level {
inline constexpr std::size_t mActorManager = 0x478;
inline constexpr std::size_t mHitResultWrapper = 464;
}

namespace HitResult {
inline constexpr std::size_t mStartPos = 0;
inline constexpr std::size_t mType = 24;
inline constexpr std::size_t mPos = 44;
}

namespace HitResultWrapper {
inline constexpr std::size_t mHitResult = 0;
}

namespace LevelChunk {
inline constexpr std::size_t mSubChunks = 0x180;
}

namespace SubChunk {
inline constexpr std::size_t Size = 0x68;
inline constexpr std::size_t mReadStorage = 0x30;
inline constexpr std::size_t mAbsoluteIndex = 0x61;
}

namespace Dimension {
inline constexpr std::size_t mBlockSource = 208;
inline constexpr std::size_t mWeather = 0x1B8;
}


namespace Weather {
inline constexpr std::size_t mOldRainLevel = 0x34;
inline constexpr std::size_t mRainLevel = 0x38;
inline constexpr std::size_t mTargetRainLevel = 0x3C;
inline constexpr std::size_t mOldLightningLevel = 0x40;
inline constexpr std::size_t mLightningLevel = 0x44;
inline constexpr std::size_t mTargetLightningLevel = 0x48;
}

}

#pragma once

#include <cstddef>
#include <cstdint>

namespace bedrocktools::sdk::offsets {

namespace LevelRendererPlayer {
inline constexpr std::size_t mFogColorRed = 0x424;
inline constexpr std::size_t mFogColorGreen = 0x428;
inline constexpr std::size_t mFogColorBlue = 0x42C;
inline constexpr std::size_t mBaseFogStart = 0x434;
inline constexpr std::size_t mBaseFogEnd = 0x438;
inline constexpr std::size_t mCurrentFogDensityMax = 0x464;
inline constexpr std::size_t mCamPos = 0x614;
inline constexpr std::size_t mCameraTargetPos = 0x620;
inline constexpr std::size_t mFrustumCuller = 0x62C;
inline constexpr std::size_t mFrustumOrigin = 0x6EC;
inline constexpr std::size_t mWorldSpaceCamera = 0x700;
inline constexpr std::size_t mViewMatrix = 0xEB0;
inline constexpr std::size_t mProjectionMatrix = 0xEF0;
inline constexpr std::size_t mWorldToCameraTranslation = 0xFBC;
inline constexpr std::size_t mCameraRoll = 0xFE8;
inline constexpr std::size_t mCameraRollPrevious = 0xFEC;
inline constexpr std::size_t mSelectionOverlayMaterial = 0x1030;
inline constexpr std::size_t mCameraPosition = 0x1148;
inline constexpr std::size_t mCameraPositionInterpolated = 0x1154;
inline constexpr std::size_t mCameraForward = 0x1160;
inline constexpr std::size_t mCameraUp = 0x116C;
inline constexpr std::size_t mCameraForwardNext = 0x1178;
inline constexpr std::size_t mCameraUpNext = 0x1184;
inline constexpr std::size_t mCameraPositionPrevious = 0x1190;
inline constexpr std::size_t mUpdateCameraFromSetupCamera = 0x694;
}

namespace Camera {
inline constexpr std::size_t mViewMatrixStack = 0x0;
inline constexpr std::size_t mInverseViewMatrix = 0xD8;
inline constexpr std::size_t mRight = 0x118;
inline constexpr std::size_t mUp = 0x124;
inline constexpr std::size_t mForward = 0x130;
inline constexpr std::size_t mPosition = 0x13C;
inline constexpr std::size_t mFov = 0x14C;
}

namespace CameraMatrixStack {
inline constexpr std::size_t mBlocks = 0x8;
inline constexpr std::size_t mOffset = 0x20;
inline constexpr std::size_t mCount = 0x28;
inline constexpr std::size_t mDirty = 0x40;
}

namespace RenderCameraComponent {
inline constexpr std::size_t mPosition = 0x38;
}



namespace ShaderConstant {
inline constexpr std::size_t mDirty = 0x29;
}




namespace MatrixStack {
inline constexpr std::size_t mBlocks = 0x50;
}


namespace ItemInHandRenderer {
inline constexpr std::size_t mClientInstance = 0x38;
}







namespace TextureUVCoordinateSet {
inline constexpr std::size_t Size = 0x58;
}



namespace LevelRenderer {
inline constexpr std::size_t mRenderChunkCoordinators = 0x28;
inline constexpr std::size_t mLevelRendererPlayer = 0x430;
}

namespace HashTable {
inline constexpr std::size_t mFirstNode = 0x10;
}

namespace HashNode {
inline constexpr std::size_t mNext = 0x0;
inline constexpr std::size_t mValuePointer = 0x18;
}

namespace RenderChunkCoordinator {
inline constexpr std::size_t MaxNodes = 64;
}

}

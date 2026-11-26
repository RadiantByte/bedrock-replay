#pragma once

#include <bedrocktools/sdk/input/MoveInput.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <array>
#include <cmath>
#include <cstddef>

struct MobWalkAnimationComponent {
    std::array<std::byte, 0x10> nativeFields;
    float movementThisTick;
};

static_assert(sizeof(MobWalkAnimationComponent) == 0x14);
static_assert(offsetof(MobWalkAnimationComponent, movementThisTick) == 0x10);

namespace bedrocktools::replay {

inline void supplyRecordedWalkDistance(sdk::Actor* actor, const sdk::Vec3& previous, const sdk::Vec3& current) {
    if (!actor) return;
    auto* context = static_cast<EntityContext*>(actor->entityContext());
    auto* walk = context->tryGetComponent<MobWalkAnimationComponent>();
    if (!walk) return;
    const float dx = current.x - previous.x;
    const float dz = current.z - previous.z;
    const float distance = std::sqrt(dx * dx + dz * dz);
    walk->movementThisTick = std::isfinite(distance) ? distance : 0.0f;
}

}

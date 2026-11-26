#pragma once

#if defined(ENTT_PACKED_PAGE) && ENTT_PACKED_PAGE != 128
#error ENTT_PACKED_PAGE must match the Minecraft component storage
#endif

#if defined(ENTT_SPARSE_PAGE) && ENTT_SPARSE_PAGE != 2048
#error ENTT_SPARSE_PAGE must match the Minecraft entity storage
#endif

#ifndef ENTT_PACKED_PAGE
#define ENTT_PACKED_PAGE 128
#endif

#ifndef ENTT_SPARSE_PAGE
#define ENTT_SPARSE_PAGE 2048
#endif

#include <entt/entt.hpp>

static_assert(ENTT_PACKED_PAGE == 128);
static_assert(ENTT_SPARSE_PAGE == 2048);

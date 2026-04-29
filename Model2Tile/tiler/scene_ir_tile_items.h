#pragma once

#include "../core/scene_ir.h"

#include <cstdint>
#include <vector>

namespace tiler
{
struct TileItem
{
    std::uint32_t id = 0;
    core::Aabb worldBounds;
    std::uint32_t triangleCount = 0;
};

std::vector<TileItem> BuildTileItemsFromSceneIR(const core::SceneIR& scene);
} // namespace tiler

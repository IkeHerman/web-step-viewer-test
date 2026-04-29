#include "scene_ir_tile_items.h"

#include <cstddef>

namespace tiler
{
std::vector<TileItem> BuildTileItemsFromSceneIR(const core::SceneIR& scene)
{
    std::vector<TileItem> items;
    items.reserve(scene.occurrences.size());

    for (std::size_t i = 0; i < scene.occurrences.size(); ++i)
    {
        const core::SceneOccurrence& source = scene.occurrences[i];
        TileItem item;
        item.id = static_cast<std::uint32_t>(i);
        item.worldBounds = source.worldBounds;
        item.triangleCount = source.triangleCount;
        items.push_back(item);
    }

    return items;
}
} // namespace tiler

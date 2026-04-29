#include "scene_ir_tile_items.h"

#include <cstddef>

namespace tiler
{
std::vector<TileItem> BuildTileItemsFromSceneIR(const core::SceneIR& scene)
{
    std::vector<TileItem> items;
    items.reserve(scene.instances.size());

    for (std::size_t i = 0; i < scene.instances.size(); ++i)
    {
        const core::SceneInstance& source = scene.instances[i];
        TileItem item;
        item.id = source.occurrenceIndex;
        item.worldBounds = source.worldBounds;
        if (source.prototypeId < scene.prototypes.size())
        {
            item.triangleCount = scene.prototypes[source.prototypeId].triangleCount;
        }
        items.push_back(item);
    }

    return items;
}
} // namespace tiler

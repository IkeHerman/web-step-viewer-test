#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "octree.h"
#include "core/scene_ir.h"

namespace TilesetEmit
{
    struct Options
    {
        std::string tilesetOutDir = "out";
        std::string contentSubdir = "tiles";   // where .b3dm goes relative to tileset.json
        std::string tileFilePrefix = "tile_";  // tile_0.b3dm, tile_1.b3dm...
        bool keepGlbFilesForDebug = false;
        double viewerTargetSse = 80.0;

        // Pre-baked high/low GLB links are read from SceneIR instances.
        double instanceMinSizeRatio = 1e-3;   // min (instance diagonal / tile diagonal); 0 = no cull
        const core::SceneIR* sceneIr = nullptr;
    };

    // Writes:
    // - tileset.json
    // - per-node .b3dm files (and optionally .glb files)
    // - optional manifest mapping tile->occurrences (see .cpp)
    bool EmitTilesetAndB3dm(
        const TileOctree& tree,
        const Options& opt);
}

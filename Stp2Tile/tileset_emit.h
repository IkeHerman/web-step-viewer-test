#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <XCAFDoc_ShapeTool.hxx>

#include "octree.h" // TileOctree, Occurrence

namespace TilesetEmit
{
    struct Options
    {
        std::string tilesetOutDir = "out";
        std::string contentSubdir = "tiles";   // where .b3dm goes relative to tileset.json
        std::string tileFilePrefix = "tile_";  // tile_0.b3dm, tile_1.b3dm...
        bool keepGlbFilesForDebug = false;

        bool useTightBounds = false;          // union of subtree item bounds (slower but better)
        bool contentOnlyAtLeaves = false;     // generally keep false with promotion rule
    };

    // Writes:
    // - tileset.json
    // - per-node .b3dm files (and optionally .glb files)
    // - optional manifest mapping tile->occurrences (see .cpp)
    bool EmitTilesetAndB3dm(
        const TileOctree& tree,
        const Handle(XCAFDoc_ShapeTool)& sourceShapeTool,
        const std::vector<Occurrence>& occurrences,
        const Options& opt);
}

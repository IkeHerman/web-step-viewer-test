#include "fbx_pipeline.h"

#include "adapters/fbx_to_scene_ir.h"
#include "importers/fbx_traversal.h"
#include "importers/step_pipeline_support.h"
#include "octree.h"
#include "tileset_emit.h"
#include "tiler/scene_ir_tile_items.h"

#include <filesystem>
#include <iostream>
#include <unordered_map>
#include <vector>

int RunFbxPipeline(const CliOptions& cli)
{
    std::vector<importers::FbxOccurrence> occurrences;
    std::unordered_map<std::string, std::string> prototypeHighLodUrisByKey;
    core::Aabb globalBounds;
    const std::string lodUriPrefix = "instance_lods";
    const std::filesystem::path lodDir = std::filesystem::path(cli.outDir) / lodUriPrefix;
    if (!importers::CollectFbxOccurrencesAndBakeLods(
            cli.inputPath,
            lodDir.string(),
            lodUriPrefix,
            occurrences,
            prototypeHighLodUrisByKey,
            globalBounds,
            true))
    {
        return 1;
    }
    if (occurrences.empty())
    {
        std::cerr << "[FbxPipeline] no renderable occurrences found in " << cli.inputPath << "\n";
        return 2;
    }

    const core::SceneIR sceneIr = adapters::BuildSceneIRFromFbxOccurrences(
        cli.inputPath.string(),
        occurrences,
        globalBounds,
        &prototypeHighLodUrisByKey,
        nullptr);
    if (!importers::ValidateSceneIrInstanceIds(sceneIr, true, true))
    {
        return 1;
    }

    const std::vector<tiler::TileItem> irTileItems = tiler::BuildTileItemsFromSceneIR(sceneIr);
    TileOctree::Config cfg;
    cfg.maxDepth = 20;
    cfg.maxItemsPerNode = 96;
    cfg.maxTrianglesPerNode = 30000;
    cfg.minNodeMaxSide = 1e-6;
    cfg.looseFactor = 1.8;

    TileOctree tree(cfg);
    tree.Build(irTileItems, sceneIr.worldBounds);

    TilesetEmit::Options opt;
    opt.tilesetOutDir = cli.outDir.string();
    opt.contentSubdir = cli.contentSubdir;
    opt.tileFilePrefix = cli.tilePrefix;
    opt.keepGlbFilesForDebug = cli.keepGlb;
    opt.viewerTargetSse = cli.viewerTargetSse;
    opt.instanceMinSizeRatio = cli.instanceMinSizeRatio;
    opt.sceneIr = &sceneIr;

    if (!TilesetEmit::EmitTilesetAndB3dm(tree, opt))
    {
        std::cerr << "[FbxPipeline] tile export failed\n";
        return 1;
    }
    std::cout << "[Stage] Tile export complete\n";
    return 0;
}

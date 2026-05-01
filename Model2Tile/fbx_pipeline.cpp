#include "fbx_pipeline.h"

#include "adapters/fbx_to_scene_ir.h"
#include "importers/fbx_instance_lod.h"
#include "importers/fbx_traversal.h"
#include "importers/step_pipeline_support.h"
#include "octree.h"
#include "tileset_emit.h"
#include "tiler/scene_ir_tile_items.h"

#include <filesystem>
#include <iostream>
#include <vector>

int RunFbxPipeline(const CliOptions& cli)
{
    std::vector<importers::FbxOccurrence> occurrences;
    core::Aabb globalBounds;
    if (!importers::CollectFbxOccurrences(cli.inputPath, occurrences, globalBounds, cli.verbose))
    {
        return 1;
    }
    if (occurrences.empty())
    {
        std::cerr << "[FbxPipeline] no renderable occurrences found in " << cli.inputPath << "\n";
        return 2;
    }

    std::vector<std::string> instanceHighGlbUris;
    const std::string lodUriPrefix = "instance_lods";
    const std::filesystem::path lodDir = std::filesystem::path(cli.outDir) / lodUriPrefix;
    if (!importers::BakeFbxInstanceLods(
            occurrences,
            lodDir.string(),
            lodUriPrefix,
            instanceHighGlbUris))
    {
        return 1;
    }

    const core::SceneIR sceneIr = adapters::BuildSceneIRFromFbxOccurrences(
        cli.inputPath.string(),
        occurrences,
        globalBounds,
        &instanceHighGlbUris,
        nullptr);
    if (!importers::ValidateSceneIrInstanceIds(sceneIr, cli.verbose, true))
    {
        return 1;
    }

    const std::vector<tiler::TileItem> irTileItems = tiler::BuildTileItemsFromSceneIR(sceneIr);
    TileOctree::Config cfg;
    cfg.maxDepth = 10;
    cfg.maxItemsPerNode = 96;
    cfg.maxTrianglesPerNode = 30000;
    cfg.minNodeMaxSide = 1e-6;
    cfg.looseFactor = 1.8;
    cfg.verbose = cli.verbose;

    TileOctree tree(cfg);
    tree.Build(irTileItems, sceneIr.worldBounds);

    TilesetEmit::Options opt;
    opt.tilesetOutDir = cli.outDir.string();
    opt.contentSubdir = cli.contentSubdir;
    opt.tileFilePrefix = cli.tilePrefix;
    opt.keepGlbFilesForDebug = cli.keepGlb;
    opt.debugAppearance = cli.verbose;
    opt.useTightBounds = cli.useTightBounds;
    opt.contentOnlyAtLeaves = cli.contentOnlyAtLeaves;
    opt.disableGlbopt = cli.disableGlbopt;
    opt.viewerTargetSse = cli.viewerTargetSse;
    opt.instanceMinSizeRatio = cli.instanceMinSizeRatio;
    opt.sceneIr = &sceneIr;

    if (!TilesetEmit::EmitTilesetAndB3dm(tree, opt))
    {
        std::cerr << "[FbxPipeline] tile export failed\n";
        return 1;
    }
    return 0;
}

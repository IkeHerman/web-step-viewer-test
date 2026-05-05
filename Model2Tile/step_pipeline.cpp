#include <iostream>
#include <string>
#include <vector>
#include <iomanip>
#include <filesystem>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <exception>
#include <fstream>
#include <memory>
#include <sstream>
#include <unordered_map>

#include "importers/step_occurrence.h"
#include "octree.h"
#include "importers/step_glb_export.h"
#include "glbopt.h"
#include "tileset_emit.h"
#include "importers/step_instance_lod.h"
#include "stpsani.h"
#include "app/cli_options.h"
#include "step_pipeline.h"
#include "adapters/step_to_scene_ir.h"
#include "tiler/scene_ir_tile_items.h"
#include "importers/step_pipeline_support.h"
#include "importers/step_traversal.h"

using NonRenderableLeafInfo = importers::NonRenderableLeafInfo;

static void PrintBounds(const core::Aabb& box)
{
    if (!box.valid)
    {
        std::cout << "  AABB=(void)";
        return;
    }

    std::cout << "  AABB=("
              << box.xmin << "," << box.ymin << "," << box.zmin << ")-("
              << box.xmax << "," << box.ymax << "," << box.zmax << ")";
}

static double BoundsMaxSideLength(const core::Aabb& box)
{
    if (!box.valid)
    {
        return 0.0;
    }

    const double sx = box.xmax - box.xmin;
    const double sy = box.ymax - box.ymin;
    const double sz = box.zmax - box.zmin;
    return std::max(sx, std::max(sy, sz));
}

static core::Aabb ToAabb(const Bnd_Box& box)
{
    core::Aabb out;
    if (box.IsVoid())
    {
        out.valid = false;
        return out;
    }

    box.Get(out.xmin, out.ymin, out.zmin, out.xmax, out.ymax, out.zmax);
    out.valid = true;
    return out;
}

static std::string BuildOccurrenceColorSignature(const Occurrence& occ)
{
    if (!occ.Appearance)
    {
        return "no-appearance";
    }

    std::ostringstream ss;
    const CachedColorSet& c = occ.Appearance->ResolvedShapeColors;
    ss << "shape(" << c.HasGen << "," << c.HasSurf << "," << c.HasCurv << ")";
    if (c.HasGen)  { ss << "|gen:" << c.Gen.Red() << "," << c.Gen.Green() << "," << c.Gen.Blue(); }
    if (c.HasSurf) { ss << "|surf:" << c.Surf.Red() << "," << c.Surf.Green() << "," << c.Surf.Blue(); }
    if (c.HasCurv) { ss << "|curv:" << c.Curv.Red() << "," << c.Curv.Green() << "," << c.Curv.Blue(); }

    std::size_t faceColorEntries = 0;
    for (const CachedFaceAppearance& fa : occ.Appearance->Faces)
    {
        if (fa.Colors.HasGen || fa.Colors.HasSurf || fa.Colors.HasCurv)
        {
            ++faceColorEntries;
        }
    }
    ss << "|faceColorEntries:" << faceColorEntries;
    return ss.str();
}

int RunStepPipeline(const CliOptions& cli)
{
    try
    {
        ConfigureFidelityArtifactOutput(cli.fidelityArtifactsDir.string());

        const std::filesystem::path inputPath = cli.inputPath;
        std::vector<std::filesystem::path> stepFiles = importers::CollectStepFiles(inputPath);

        if (stepFiles.empty())
        {
            std::cerr << "No .stp/.step files found at: " << inputPath << "\n";
            return 2;
        }

        std::cout << "[Stage] Begin solid traversal\n";

        std::vector<Occurrence> occurrences;
        Bnd_Box globalBounds;
        globalBounds.SetVoid();

        std::size_t traversedLeafLabels = 0;
        std::size_t labelsWithNoRenderableGeometry = 0;
        std::size_t labelsWithShellFallback = 0;
        std::size_t nonAssemblyLabelsWithComponents = 0;
        std::vector<importers::NonRenderableLeafInfo> nonRenderableLeaves;
        std::uint64_t totalTriangles = 0;
        std::size_t totalRoots = 0;
        if (!importers::CollectStepOccurrencesFromFiles(
                stepFiles,
                cli.verbose,
                occurrences,
                globalBounds,
                traversedLeafLabels,
                labelsWithNoRenderableGeometry,
                labelsWithShellFallback,
                nonAssemblyLabelsWithComponents,
                nonRenderableLeaves,
                totalTriangles,
                totalRoots))
        {
            return 1;
        }

        std::cout << "[Stage] Solid traversal complete\n";
        std::cout << "Found " << totalRoots << " free roots\n";
        std::cout << "Traversed " << traversedLeafLabels << " non-assembly labels\n";
        std::cout << "Built " << occurrences.size() << " occurrences\n";
        std::cout << "Estimated solids tris=" << totalTriangles << "\n";
        std::cout << "Labels with shell fallback: " << labelsWithShellFallback << "\n";
        std::cout << "Labels with no renderable geometry: " << labelsWithNoRenderableGeometry << "\n";
        std::cout << "Non-assembly labels with components (probe): " << nonAssemblyLabelsWithComponents
                  << "\n";
        if (!nonRenderableLeaves.empty())
        {
            std::cout << "Non-renderable leaves:\n";
            const std::size_t maxToPrint = 20;
            const std::size_t printCount = std::min(maxToPrint, nonRenderableLeaves.size());

            for (std::size_t i = 0; i < printCount; ++i)
            {
                const NonRenderableLeafInfo& info = nonRenderableLeaves[i];
                std::cout << "  - label=" << info.Label;
                if (!info.Name.empty())
                {
                    std::cout << " name=\"" << info.Name << "\"";
                }
                std::cout << " type=" << static_cast<int>(info.Type) << " shells=" << info.ShellCount
                          << " faces=" << info.FaceCount << "\n";
            }

            if (nonRenderableLeaves.size() > printCount)
            {
                std::cout << "  ... (" << (nonRenderableLeaves.size() - printCount) << " more)\n";
            }
        }
        std::cout << "Global bounds: ";
        PrintBounds(ToAabb(globalBounds));
        std::cout << "\n\n";

        {
            std::size_t shapeColorCount = 0;
            std::size_t shapeMaterialCount = 0;
            std::size_t faceColorCount = 0;
            std::size_t faceMaterialCount = 0;
            std::size_t occurrencesWithAnyFaceStyle = 0;
            std::size_t totalFaceSlots = 0;

            for (const Occurrence& occ : occurrences)
            {
                if (!occ.Appearance)
                {
                    continue;
                }

                const CachedColorSet& sc = occ.Appearance->ResolvedShapeColors;
                if (sc.HasGen || sc.HasSurf || sc.HasCurv)
                {
                    ++shapeColorCount;
                }
                if (!occ.Appearance->ResolvedShapeMaterial.IsNull())
                {
                    ++shapeMaterialCount;
                }

                bool hasAnyFaceStyle = false;
                totalFaceSlots += occ.Appearance->Faces.size();
                for (const CachedFaceAppearance& fa : occ.Appearance->Faces)
                {
                    if (fa.Colors.HasGen || fa.Colors.HasSurf || fa.Colors.HasCurv)
                    {
                        ++faceColorCount;
                        hasAnyFaceStyle = true;
                    }
                    if (!fa.VisMaterial.IsNull())
                    {
                        ++faceMaterialCount;
                        hasAnyFaceStyle = true;
                    }
                }

                if (hasAnyFaceStyle)
                {
                    ++occurrencesWithAnyFaceStyle;
                }
            }

            std::cout << "[AppearanceProbe] occurrences=" << occurrences.size()
                      << " shapeColor=" << shapeColorCount
                      << " shapeMaterial=" << shapeMaterialCount
                      << " faceColorEntries=" << faceColorCount
                      << " faceMaterialEntries=" << faceMaterialCount
                      << " occWithFaceStyle=" << occurrencesWithAnyFaceStyle
                      << " faceSlots=" << totalFaceSlots
                      << "\n\n";

            std::unordered_map<std::string, std::size_t> colorSignatureCounts;
            colorSignatureCounts.reserve(occurrences.size());
            for (const Occurrence& occ : occurrences)
            {
                ++colorSignatureCounts[BuildOccurrenceColorSignature(occ)];
            }
            std::vector<std::pair<std::string, std::size_t>> rankedColorSigs(
                colorSignatureCounts.begin(), colorSignatureCounts.end());
            std::sort(
                rankedColorSigs.begin(),
                rankedColorSigs.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });

            std::cout << "[AppearanceProbe:TopColorSignatures] unique=" << rankedColorSigs.size()
                      << "\n";
            const std::size_t topN = std::min<std::size_t>(10, rankedColorSigs.size());
            for (std::size_t i = 0; i < topN; ++i)
            {
                std::cout << "  - count=" << rankedColorSigs[i].second
                          << " signature=" << rankedColorSigs[i].first << "\n";
            }
            std::cout << "\n";
        }

        std::cout << "[Stage] Begin octree build\n";

        const core::Aabb globalBoundsAabb = ToAabb(globalBounds);
        const double rootMaxSide = BoundsMaxSideLength(globalBoundsAabb);

        TileOctree::Config cfg;
        cfg.maxDepth = 20;
        cfg.maxItemsPerNode = 96;
        cfg.maxTrianglesPerNode = 30000;
        cfg.minNodeMaxSide = std::max(1e-6, rootMaxSide * 1e-3);
        cfg.looseFactor = 1.8;

        std::unordered_map<std::string, std::string> prototypeHighLodUrisByKey;
        const std::string lodUriPrefix = "instance_lods";
        const std::filesystem::path lodDir =
            std::filesystem::path(cli.outDir) / lodUriPrefix;
        if (!importers::BakeStepInstanceLods(
                occurrences,
                globalBounds,
                cli.viewerTargetSse,
                lodDir.string(),
                lodUriPrefix,
                prototypeHighLodUrisByKey))
        {
            return 1;
        }

        const core::SceneIR sceneIr = adapters::BuildSceneIRFromStepOccurrences(
            cli.inputPath.string(),
            occurrences,
            globalBoundsAabb,
            &prototypeHighLodUrisByKey,
            nullptr);
        if (!importers::ValidateSceneIrInstanceIds(sceneIr, true, true))
        {
            return 1;
        }
        if (sceneIr.shapeColorOccurrences == 0 && !occurrences.empty())
        {
            std::cerr << "[SceneIR] Warning: no shape color occurrences preserved\n";
        }
        if (sceneIr.faceColorEntries == 0 && !occurrences.empty())
        {
            std::cerr << "[SceneIR] Warning: no face color entries preserved\n";
        }
        if (sceneIr.metadataOccurrences == 0 && !occurrences.empty())
        {
            std::cerr << "[SceneIR] Warning: no metadata occurrences preserved\n";
        }
        importers::WriteStepFidelityArtifacts(cli.fidelityArtifactsDir, occurrences, sceneIr);
        const std::vector<tiler::TileItem> irTileItems = tiler::BuildTileItemsFromSceneIR(sceneIr);
        TileOctree tree(cfg);
        tree.Build(irTileItems, sceneIr.worldBounds);

        std::cout << "[SceneIR] format=" << sceneIr.sourceFormat
                  << " prototypes=" << sceneIr.prototypes.size()
                  << " instances=" << sceneIr.instances.size()
                  << " tileItems=" << irTileItems.size()
                  << " totalTriangles=" << sceneIr.totalTriangles
                  << " explicitRefInstances=" << sceneIr.explicitReferenceInstances
                  << " qualifiedDedupInstances=" << sceneIr.qualifiedDedupInstances
                  << "\n";

        std::cout << "[Stage] Begin tile export\n";

        TilesetEmit::Options opt;
        opt.tilesetOutDir = cli.outDir.string();
        opt.contentSubdir = cli.contentSubdir;
        opt.tileFilePrefix = cli.tilePrefix;
        opt.keepGlbFilesForDebug = cli.keepGlb;
        opt.viewerTargetSse = cli.viewerTargetSse;
        opt.instanceMinSizeRatio = cli.instanceMinSizeRatio;
        opt.sceneIr = &sceneIr;

        std::cout << "[Config] outDir=" << std::filesystem::absolute(cli.outDir)
                  << " contentSubdir=" << opt.contentSubdir
                  << " tilePrefix=" << opt.tileFilePrefix
                  << " keepGlb=" << (opt.keepGlbFilesForDebug ? "true" : "false")
                  << " viewerTargetSse=" << opt.viewerTargetSse
                  << " instanceMinSizeRatio=" << opt.instanceMinSizeRatio << "\n";

        const bool emitOk = TilesetEmit::EmitTilesetAndB3dm(tree, opt);
        if (!emitOk)
        {
            std::cerr << "[Error] Tile export failed\n";
            return 1;
        }

        std::cout << "[Stage] Tile export complete\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Fatal exception: " << e.what() << "\n";
        return 1;
    }
    catch (...)
    {
        std::cerr << "Fatal exception: unknown\n";
        return 1;
    }
}

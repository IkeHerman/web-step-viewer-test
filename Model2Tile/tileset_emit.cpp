#include "tileset_emit.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <map>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <system_error>
#include <cmath>

#include "b3dm.h"
#include "glbopt.h"

namespace
{
    static const core::SceneInstance* FindInstanceById(
        const core::SceneIR& sceneIr,
        std::uint32_t id)
    {
        if (id < sceneIr.instances.size())
        {
            const core::SceneInstance& direct = sceneIr.instances[static_cast<std::size_t>(id)];
            if (direct.id == id)
            {
                return &direct;
            }
        }
        for (const core::SceneInstance& instance : sceneIr.instances)
        {
            if (instance.id == id)
            {
                return &instance;
            }
        }
        return nullptr;
    }

    static void GetMinMax(const core::Aabb& b,
                          double& xmin, double& ymin, double& zmin,
                          double& xmax, double& ymax, double& zmax)
    {
        xmin = b.xmin;
        ymin = b.ymin;
        zmin = b.zmin;
        xmax = b.xmax;
        ymax = b.ymax;
        zmax = b.zmax;
    }

    static double DiagonalLength(const core::Aabb& b)
    {
        if (!b.valid)
        {
            return 0.0;
        }

        double xmin, ymin, zmin, xmax, ymax, zmax;
        GetMinMax(b, xmin, ymin, zmin, xmax, ymax, zmax);

        const double dx = xmax - xmin;
        const double dy = ymax - ymin;
        const double dz = zmax - zmin;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    static std::array<double, 12> ToTilesBoxGltfSpace(const core::Aabb& b)
    {
        double xmin, ymin, zmin, xmax, ymax, zmax;
        GetMinMax(b, xmin, ymin, zmin, xmax, ymax, zmax);

        const double cx = (xmin + xmax) * 0.5;
        const double cy = (ymin + ymax) * 0.5;
        const double cz = (zmin + zmax) * 0.5;

        const double hxLen = (xmax - xmin) * 0.5;
        const double hyLen = (ymax - ymin) * 0.5;
        const double hzLen = (zmax - zmin) * 0.5;

        return {
            cx, -cz, cy,
            hxLen, 0.0, 0.0,
            0.0, 0.0, hyLen,
            0.0, -hzLen, 0.0
        };
    }

    static bool HasAnyChild(const TileOctree::Node& node)
    {
        for (const auto& c : node.children)
        {
            if (c)
            {
                return true;
            }
        }
        return false;
    }

    static void CollectFilteredInstanceLodPaths(
        const std::vector<std::uint32_t>& itemIndices,
        const core::Aabb& tileBounds,
        double minSizeRatio,
        const core::SceneIR& sceneIr,
        const std::string& tilesetOutDir,
        std::vector<std::string>& outPaths)
    {
        outPaths.clear();
        if (!tileBounds.valid)
        {
            return;
        }

        const double tileD = std::max(1e-12, DiagonalLength(tileBounds));

        for (const std::uint32_t idx : itemIndices)
        {
            const core::SceneInstance* matchedInstance = FindInstanceById(sceneIr, idx);
            if (!matchedInstance)
            {
                continue;
            }

            const std::string& p = matchedInstance->highLodGlbUri;
            if (p.empty())
            {
                continue;
            }

            const core::Aabb& ob = matchedInstance->worldBounds;
            if (!ob.valid)
            {
                continue;
            }

            const double occD = DiagonalLength(ob);
            if (minSizeRatio > 0.0 && occD / tileD < minSizeRatio)
            {
                continue;
            }

            std::filesystem::path absPath = std::filesystem::path(tilesetOutDir) / p;
            outPaths.push_back(absPath.string());
        }
    }

    static bool IsFinite(double v)
    {
        return std::isfinite(v);
    }

    static bool ValidateBounds(const core::Aabb& b, bool expectNonEmpty, const std::string& label)
    {
        if (!b.valid)
        {
            if (expectNonEmpty)
            {
                std::cout << "[TilesetEmit] invalid bounds (void) for " << label << "\n";
                return false;
            }
            return true;
        }

        double xmin, ymin, zmin, xmax, ymax, zmax;
        GetMinMax(b, xmin, ymin, zmin, xmax, ymax, zmax);

        if (!IsFinite(xmin) || !IsFinite(ymin) || !IsFinite(zmin) ||
            !IsFinite(xmax) || !IsFinite(ymax) || !IsFinite(zmax))
        {
            std::cout << "[TilesetEmit] invalid bounds (non-finite) for " << label << "\n";
            return false;
        }

        const double dx = xmax - xmin;
        const double dy = ymax - ymin;
        const double dz = zmax - zmin;
        const double radius = 0.5 * std::sqrt(dx * dx + dy * dy + dz * dz);

        if (expectNonEmpty && radius <= 1e-9)
        {
            std::cout << "[TilesetEmit] invalid bounds (radius too small) for " << label
                      << ", radius=" << radius << "\n";
            return false;
        }

        return true;
    }

    static bool ValidateTileGeometryStats(
        const glbopt::Stats& stats,
        bool expectNonEmpty,
        const std::string& label)
    {
        if (!expectNonEmpty)
        {
            return true;
        }

        if (stats.PrimitiveCountMergedOut == 0)
        {
            std::cout << "[TilesetEmit] rejected " << label
                      << ": no merged primitives were emitted\n";
            return false;
        }

        if (stats.OutputVertexCount < 3)
        {
            std::cout << "[TilesetEmit] rejected " << label
                      << ": output vertex count too small (" << stats.OutputVertexCount << ")\n";
            return false;
        }

        if (stats.OutputPrimitiveElementCount < 3)
        {
            std::cout << "[TilesetEmit] rejected " << label
                      << ": output index count too small (" << stats.OutputPrimitiveElementCount << ")\n";
            return false;
        }

        if ((stats.OutputPrimitiveElementCount % 3u) != 0u)
        {
            std::cout << "[TilesetEmit] rejected " << label
                      << ": output index count is not triangle-aligned ("
                      << stats.OutputPrimitiveElementCount << ")\n";
            return false;
        }

        return true;
    }

    static bool ValidateProxyReduction(
        const glbopt::Stats& stats,
        bool expectNonEmpty,
        const std::string& label)
    {
        if (!expectNonEmpty || stats.InputVertexCount == 0)
        {
            return true;
        }

        const double ratio =
            static_cast<double>(stats.OutputVertexCount) /
            static_cast<double>(stats.InputVertexCount);

        if (ratio <= 1e-4)
        {
            std::cout << "[TilesetEmit] rejected " << label
                      << ": proxy collapsed too aggressively (vertex ratio=" << ratio << ")\n";
            return false;
        }

        return true;
    }

    static bool ValidateAppearanceCardinality(
        const glbopt::Stats& stats,
        bool expectNonEmpty,
        const std::string& label)
    {
        if (!expectNonEmpty)
        {
            return true;
        }
        if (stats.MaterialCountInput > 0 && stats.MaterialCountCanonical == 0)
        {
            std::cout << "[TilesetEmit] rejected " << label
                      << ": material cardinality collapsed to zero\n";
            return false;
        }
        return true;
    }

    static void AccumulateTightBounds(
        const TileOctree::Node& node,
        const core::SceneIR& sceneIr,
        core::Aabb& out)
    {
        for (std::uint32_t idx : node.items)
        {
            const core::SceneInstance* instance = FindInstanceById(sceneIr, idx);
            if (!instance)
            {
                continue;
            }

            const core::Aabb& ob = instance->worldBounds;
            if (!ob.valid)
            {
                continue;
            }
            if (!out.valid)
            {
                out = ob;
            }
            else
            {
                out.xmin = std::min(out.xmin, ob.xmin);
                out.ymin = std::min(out.ymin, ob.ymin);
                out.zmin = std::min(out.zmin, ob.zmin);
                out.xmax = std::max(out.xmax, ob.xmax);
                out.ymax = std::max(out.ymax, ob.ymax);
                out.zmax = std::max(out.zmax, ob.zmax);
            }
        }

        for (const auto& c : node.children)
        {
            if (!c)
            {
                continue;
            }

            AccumulateTightBounds(*c, sceneIr, out);
        }
    }

    static core::Aabb ComputeTightBounds(const TileOctree::Node& node, const core::SceneIR& sceneIr)
    {
        core::Aabb b;
        b.valid = false;
        AccumulateTightBounds(node, sceneIr, b);

        return b;
    }

    static std::string JoinUri(const std::string& subdir, const std::string& name)
    {
        if (subdir.empty())
        {
            return name;
        }

        if (subdir.back() == '/' || subdir.back() == '\\')
        {
            return subdir + name;
        }

        return subdir + "/" + name;
    }

    static void Indent(std::ostringstream& ss, int depth)
    {
        for (int i = 0; i < depth; ++i)
        {
            ss << "  ";
        }
    }

    static int ComputeMaxDepth(const TileOctree::Node& node)
    {
        int maxChildDepth = 0;

        for (const auto& childPtr : node.children)
        {
            if (!childPtr)
            {
                continue;
            }

            const int childDepth = ComputeMaxDepth(*childPtr);
            if (childDepth > maxChildDepth)
            {
                maxChildDepth = childDepth;
            }
        }

        return 1 + maxChildDepth;
    }

    static float Clamp01(float v)
    {
        if (v < 0.0f)
        {
            return 0.0f;
        }

        if (v > 1.0f)
        {
            return 1.0f;
        }

        return v;
    }

    static float LerpClamped(float a, float b, float t)
    {
        const float u = Clamp01(t);
        return a + (b - a) * u;
    }

    static double ClampDouble(double v, double lo, double hi)
    {
        return std::max(lo, std::min(v, hi));
    }

    static double ComputeNodeBoxWeldStep(double nodeDiag, double rootDiag)
    {
        const double safeNodeDiag = std::max(0.0, nodeDiag);
        const double rootStepMin = std::max(1e-12, rootDiag * 1e-9);

        // Position weld step: fraction of the node AABB diagonal (scale-invariant).
        // 10x coarser than the prior 2e-5 fraction for more aggressive vertex welding.
        constexpr double kWeldFractionOfNodeDiag = 2e-4;

        const double stepMin = std::max(rootStepMin, safeNodeDiag * 1e-9);
        const double stepMax = std::max(stepMin, safeNodeDiag * 2e-3);

        return ClampDouble(
            safeNodeDiag * kWeldFractionOfNodeDiag,
            stepMin,
            stepMax);
    }

    struct NodeTuning
    {
        float RelativeSize = 0.0f;
        float NormalizedDepth = 0.0f;
        float Importance = 0.0f;

        float ProxyPositionStep = 1e-5f;
        float LeafPositionStep = 1e-5f;

        // glbopt attribute quantization: scale with (node diagonal / root diagonal) like
        // tessellation and geometricError, so small tiles use proportionally finer steps.
        float ProxyNormalStep = 1e-4f;
        float ProxyTexcoordStep = 1e-5f;
        float LeafNormalStep = 1e-4f;
        float LeafTexcoordStep = 1e-6f;

        float ProxySimplifyRatio = 0.2f;
        float ProxySimplifyError = 1e-2f;

        float LeafSimplifyRatio = 0.96f;
        float LeafSimplifyError = 2e-3f;

        double GeometricError = 0.0;
    };

    // SSE-inspired importance:
    // 1. Larger nodes relative to root stay less aggressive.
    // 2. Deeper nodes get somewhat more aggressive.
    // 3. We use this only to choose bake-time proxy settings.
    static float ComputeNodeImportance(
        const core::Aabb& nodeBounds,
        const core::Aabb& rootBounds,
        int depthFromRoot,
        int maxDepth)
    {
        const double rootDiag = std::max(1e-9, DiagonalLength(rootBounds));
        const double nodeDiag = std::max(0.0, DiagonalLength(nodeBounds));

        const float relativeSize =
            Clamp01(static_cast<float>(nodeDiag / rootDiag));

        const float normalizedDepth =
            Clamp01(static_cast<float>(depthFromRoot) /
                    static_cast<float>(std::max(1, maxDepth)));

        // Weight size more heavily than depth:
        // big nodes matter even if deep; tiny nodes can be very aggressive.
        const float importance =
            Clamp01(relativeSize * 0.75f + (1.0f - normalizedDepth) * 0.25f);

        return importance;
    }

    static NodeTuning BuildNodeTuning(
        const core::Aabb& nodeBounds,
        const core::Aabb& rootBounds,
        int depthFromRoot,
        int maxDepth,
        bool isLeaf,
        double viewerTargetSse)
    {
        NodeTuning tuning;

        const double rootDiag = std::max(1e-9, DiagonalLength(rootBounds));
        const double nodeDiag = std::max(0.0, DiagonalLength(nodeBounds));

        tuning.RelativeSize =
            Clamp01(static_cast<float>(nodeDiag / rootDiag));
        tuning.NormalizedDepth =
            Clamp01(static_cast<float>(depthFromRoot) /
                    static_cast<float>(std::max(1, maxDepth)));
        tuning.Importance =
            ComputeNodeImportance(nodeBounds, rootBounds, depthFromRoot, maxDepth);

        const float aggressiveness =
            Clamp01((1.0f - tuning.RelativeSize) * 0.65f + tuning.NormalizedDepth * 0.35f);

        const double weldStep = ComputeNodeBoxWeldStep(nodeDiag, rootDiag);
        tuning.ProxyPositionStep = static_cast<float>(weldStep);
        tuning.LeafPositionStep = static_cast<float>(weldStep);

        const double relToRoot =
            ClampDouble(nodeDiag / rootDiag, 1e-12, 1.0e6);
        // 10x coarser than prior (1.5e-4 / 1.0e-4) for more aggressive normal quantization.
        tuning.LeafNormalStep = static_cast<float>(
            ClampDouble(1.5e-3 * relToRoot, 3e-5, 2e-2));
        tuning.ProxyNormalStep = static_cast<float>(
            ClampDouble(1.0e-3 * relToRoot, 3e-5, 1e-2));
        tuning.LeafTexcoordStep = static_cast<float>(
            ClampDouble(3.0e-6 * relToRoot, 1e-9, 5e-4));
        tuning.ProxyTexcoordStep = static_cast<float>(
            ClampDouble(5.0e-6 * relToRoot, 1e-9, 1e-3));

        // Keep proxy reduction visible but less destructive than before.
        tuning.ProxySimplifyRatio =
            LerpClamped(0.42f, 0.68f, 1.0f - aggressiveness);

        // Mild leaf optimization with conservative retention.
        const float leafAggressiveness =
            Clamp01(tuning.NormalizedDepth * 0.5f + (1.0f - tuning.RelativeSize) * 0.5f);
        tuning.LeafSimplifyRatio =
            LerpClamped(0.99f, 0.95f, leafAggressiveness);

        // Geometric error model (octree-aligned):
        // We scale geometric error from node diagonal, not with a fixed per-level
        // multiplier. In an octree, linear size (and diagonal) tends to halve each
        // level, so this naturally gives parent ~= 2x child error in regular regions.
        // This avoids abrupt jumps from aggressive fixed ladders (e.g. 4x).
        //
        // SSE target scales this uniformly, preserving monotonic hierarchy behavior.
        if (!isLeaf)
        {
            constexpr double kGeomErrFraction = 0.25;
            constexpr double kGeomErrMinFraction = 0.08;
            constexpr double kGeomErrMaxFraction = 0.35;

            const double minErr = nodeDiag * kGeomErrMinFraction;
            const double maxErr = nodeDiag * kGeomErrMaxFraction;
            const double sseScale = ClampDouble(std::max(1.0, viewerTargetSse) / 80.0, 0.5, 2.0);
            const double rawErr = nodeDiag * kGeomErrFraction * sseScale;
            tuning.GeometricError = ClampDouble(rawErr, minErr, std::max(minErr, maxErr));
        }
        else
        {
            tuning.GeometricError = 0.0;
        }

        const double errorFromGeom = std::max(1e-8, tuning.GeometricError / std::max(1e-9, rootDiag));

        tuning.ProxySimplifyError = static_cast<float>(ClampDouble(
            std::max(0.0015, errorFromGeom * 0.40) * (0.85 + 0.30 * static_cast<double>(tuning.NormalizedDepth)),
            0.0015,
            0.02));

        tuning.LeafSimplifyError = static_cast<float>(ClampDouble(
            std::max(0.00035, errorFromGeom * 0.1),
            0.00035,
            0.003));

        return tuning;
    }

    /// Combine multiple instance GLBs into one buffer; weld + degenerate cleanup with node-scaled quantization.
    /// Merge uses bitwise TEXCOORD keys (`TexcoordStep` 0) so seams stay distinct unless UVs match exactly.
    static glbopt::Options BuildMergedInstanceGlbOptOptions(const NodeTuning& tuning, bool mergeFeedsLeafTile)
    {
        glbopt::Options o;

        const float leafOrProxyNormalStep =
            mergeFeedsLeafTile ? tuning.LeafNormalStep : tuning.ProxyNormalStep;

        o.DeduplicateMaterials = true;
        o.WeldPositions = true;
        o.WeldNormals = true;
        o.WeldTexcoord0 = true;
        o.WeldColor0 = true;

        // Proxy vs leaf share the same positional weld quantization from BuildNodeTuning.
        o.PositionStep = tuning.LeafPositionStep;
        o.NormalStep = leafOrProxyNormalStep;
        o.TexcoordStep = 0.0f;
        o.ColorStep = 1.0f / 255.0f;

        o.RemoveDegenerateByIndex = true;
        o.RemoveDegenerateByArea = true;
        o.DegenerateAreaEpsilonSq = 1e-20f;

        o.Simplify = false;
        o.OptimizeVertexCache = true;
        o.OptimizeOverdraw = false;
        o.OptimizeVertexFetch = true;
        return o;
    }

    static glbopt::Options BuildLeafGlbOptOptionsForNode(const NodeTuning& tuning)
    {
        glbopt::Options options;

        options.DeduplicateMaterials = true;
        options.WeldPositions = true;
        options.WeldNormals = true;
        options.WeldTexcoord0 = true;
        options.WeldColor0 = true;
        options.ForceDoubleSidedMaterials = false;

        options.PositionStep = tuning.LeafPositionStep;
        options.NormalStep = tuning.LeafNormalStep;
        // Bitwise TEXCOORD weld keys — welding still runs; UVs participate as exact floats.
        options.TexcoordStep = 0.0f;
        options.ColorStep = 1.0f / 255.0f;

        options.RemoveDegenerateByIndex = true;
        options.RemoveDegenerateByArea = true;

        // Mesh simplification off: weld/merge/degenerate cleanup only (easier to isolate bad triangles).
        options.Simplify = false;
        options.SimplifyRatio = tuning.LeafSimplifyRatio;
        options.SimplifyError = tuning.LeafSimplifyError;

        options.OptimizeVertexCache = true;
        options.OptimizeOverdraw = false;
        options.OptimizeVertexFetch = true;

        return options;
    }

    struct BakedNodeArtifact
    {
        std::uint32_t nodeId = 0;
        bool isLeaf = false;
        core::Aabb bounds;
        std::filesystem::path GlbPath;
        std::string ContentUri;
    };

    struct BakeState
    {
        std::uint32_t nextNodeId = 0;
        std::unordered_map<const TileOctree::Node*, BakedNodeArtifact> artifacts;
    };

    static void CollectDescendantLeafOptimizedGlbPaths(
        const TileOctree::Node& node,
        const BakeState& bakeState,
        std::vector<std::string>& outPaths)
    {
        if (!HasAnyChild(node))
        {
            const auto it = bakeState.artifacts.find(&node);
            if (it != bakeState.artifacts.end() && !it->second.GlbPath.empty())
            {
                outPaths.push_back(it->second.GlbPath.string());
            }
            return;
        }
        for (const auto& childPtr : node.children)
        {
            if (!childPtr)
            {
                continue;
            }
            CollectDescendantLeafOptimizedGlbPaths(*childPtr, bakeState, outPaths);
        }
    }

    static void ScaleGlboptAggression(glbopt::Options& o, float factor)
    {
        if (!(factor > 1.0f))
        {
            return;
        }
        o.PositionStep *= factor;
        o.NormalStep *= factor;
        if (o.TexcoordStep > 0.0f)
        {
            o.TexcoordStep *= factor;
        }
        o.ColorStep *= factor;
        o.SimplifyError *= factor;
        if (o.Simplify && o.SimplifyRatio < 1.0f)
        {
            const float r = o.SimplifyRatio;
            o.SimplifyRatio = std::max(0.01f, r - (1.0f - r) * (factor - 1.0f));
        }
    }

    static bool BakeLeafArtifacts(
        const TileOctree::Node& node,
        const core::SceneIR& sceneIr,
        const TilesetEmit::Options& opt,
        BakedNodeArtifact& artifact,
        int depthFromRoot,
        int maxDepth,
        const core::Aabb& rootBounds)
    {
        const std::string baseName = opt.tileFilePrefix + std::to_string(artifact.nodeId);
        const std::filesystem::path tilesDir =
            std::filesystem::path(opt.tilesetOutDir) / opt.contentSubdir;

        std::filesystem::create_directories(tilesDir);

        const std::filesystem::path rawFullGlbPath = tilesDir / (baseName + "_raw.glb");
        const std::filesystem::path fullGlbPath = tilesDir / (baseName + ".glb");
        const std::filesystem::path fullB3dmPath = tilesDir / (baseName + ".b3dm");

        std::vector<std::string> instPaths;
        CollectFilteredInstanceLodPaths(
            node.items,
            artifact.bounds,
            opt.instanceMinSizeRatio,
            sceneIr,
            opt.tilesetOutDir,
            instPaths);

        if (instPaths.empty())
        {
            artifact.GlbPath.clear();
            artifact.ContentUri.clear();
            return true;
        }

        const NodeTuning tuning = BuildNodeTuning(
            artifact.bounds,
            rootBounds,
            depthFromRoot,
            maxDepth,
            true,
            opt.viewerTargetSse);

        const glbopt::Options leafGlbOptions = BuildLeafGlbOptOptionsForNode(tuning);
        glbopt::Stats mergeStats;
        if (!glbopt::OptimizeGlbFiles(
                instPaths,
                rawFullGlbPath.string(),
                BuildMergedInstanceGlbOptOptions(tuning, true),
                mergeStats))
        {
            return false;
        }

        glbopt::Stats leafGlbStats;
        const bool expectStats = node.totalTriangles > 0;
        const bool bypassLeafGlbopt = opt.disableGlbopt;
        bool okOptimize = true;
        if (bypassLeafGlbopt)
        {
            std::error_code copyErr;
            std::filesystem::copy_file(
                rawFullGlbPath,
                fullGlbPath,
                std::filesystem::copy_options::overwrite_existing,
                copyErr);
            if (copyErr)
            {
                return false;
            }
        }
        else
        {
            okOptimize =
                glbopt::OptimizeGlbFile(
                    rawFullGlbPath.string(),
                    fullGlbPath.string(),
                    leafGlbOptions,
                    leafGlbStats);
            if (!okOptimize)
            {
                return false;
            }
            if (!ValidateTileGeometryStats(leafGlbStats, expectStats, baseName))
            {
                return false;
            }
            if (!ValidateAppearanceCardinality(leafGlbStats, expectStats, baseName))
            {
                return false;
            }
        }

        if (opt.debugAppearance)
        {
            if (bypassLeafGlbopt)
            {
                std::cout << "[LeafGlbOpt] tile=" << baseName
                          << " glbopt=bypass (disableGlbopt)\n";
            }
            else
            {
                const double diag = DiagonalLength(artifact.bounds);
                const std::size_t trisIn = leafGlbStats.InputPrimitiveElementCount / 3u;
                const std::size_t trisOut = leafGlbStats.OutputPrimitiveElementCount / 3u;
                double reductionPct = 0.0;
                if (trisIn > 0u)
                {
                    reductionPct = 100.0 *
                        (1.0 - (static_cast<double>(trisOut) / static_cast<double>(trisIn)));
                }
                reductionPct = ClampDouble(reductionPct, 0.0, 100.0);

                std::cout << "[LeafGlbOpt] tile=" << baseName
                          << " size=" << diag
                          << " posStep=" << leafGlbOptions.PositionStep
                          << " vertsIn=" << leafGlbStats.InputVertexCount
                          << " vertsOut=" << leafGlbStats.OutputVertexCount
                          << " mergedVerts=" << leafGlbStats.MergedVertexCount
                          << " trisIn=" << trisIn
                          << " trisOut=" << trisOut
                          << " triDelta%=" << reductionPct
                          << "\n";
            }
        }

        std::string sourceLabelSample;
        if (!node.items.empty())
        {
            const core::SceneInstance* instance = FindInstanceById(sceneIr, node.items.front());
            if (instance)
            {
                sourceLabelSample = instance->sourceLabel;
            }
        }
        const std::map<std::string, std::string> tileMetadata = {
            { "tileId", std::to_string(artifact.nodeId) },
            { "tileKind", "leaf" },
            { "sourceLabelSample", sourceLabelSample }
        };
        const bool okB3dm =
            B3dm::WrapGlbFileToB3dmFile(fullGlbPath.string(), fullB3dmPath.string(), tileMetadata);

        if (!okB3dm)
        {
            return false;
        }

        artifact.GlbPath = fullGlbPath;
        artifact.ContentUri = JoinUri(opt.contentSubdir, baseName + ".b3dm");

        if (!opt.keepGlbFilesForDebug)
        {
            std::error_code ec;
            std::filesystem::remove(rawFullGlbPath, ec);
        }

        return true;
    }

    static bool BakeInternalProxyArtifact(
        const TileOctree::Node& node,
        [[maybe_unused]] const core::SceneIR& sceneIr,
        const TilesetEmit::Options& opt,
        BakedNodeArtifact& artifact,
        int depthFromRoot,
        int maxDepth,
        const core::Aabb& rootBounds,
        const BakeState& bakeState)
    {
        const std::string baseName = opt.tileFilePrefix + std::to_string(artifact.nodeId);
        const std::filesystem::path tilesDir =
            std::filesystem::path(opt.tilesetOutDir) / opt.contentSubdir;

        std::filesystem::create_directories(tilesDir);

        const std::filesystem::path rawProxyGlbPath = tilesDir / (baseName + "_raw.glb");
        const std::filesystem::path proxyGlbPath = tilesDir / (baseName + ".glb");
        const std::filesystem::path proxyB3dmPath = tilesDir / (baseName + ".b3dm");

        const NodeTuning tuning = BuildNodeTuning(
            artifact.bounds,
            rootBounds,
            depthFromRoot,
            maxDepth,
            false,
            opt.viewerTargetSse);

        std::vector<std::string> leafGlbPaths;
        CollectDescendantLeafOptimizedGlbPaths(node, bakeState, leafGlbPaths);

        if (leafGlbPaths.empty())
        {
            artifact.GlbPath.clear();
            artifact.ContentUri.clear();
            return true;
        }

        glbopt::Stats mergeStats;
        if (!glbopt::OptimizeGlbFiles(
                leafGlbPaths,
                rawProxyGlbPath.string(),
                BuildMergedInstanceGlbOptOptions(tuning, false),
                mergeStats))
        {
            return false;
        }

        const NodeTuning leafTuningForProxyAggression = BuildNodeTuning(
            artifact.bounds,
            rootBounds,
            depthFromRoot,
            maxDepth,
            true,
            opt.viewerTargetSse);
        glbopt::Options proxyGlbOptions = BuildLeafGlbOptOptionsForNode(leafTuningForProxyAggression);
        ScaleGlboptAggression(proxyGlbOptions, 2.0f);

        glbopt::Stats proxyStats;
        bool okProxyBake = true;
        if (opt.disableGlbopt)
        {
            std::error_code copyErr;
            std::filesystem::copy_file(
                rawProxyGlbPath,
                proxyGlbPath,
                std::filesystem::copy_options::overwrite_existing,
                copyErr);
            if (copyErr)
            {
                return false;
            }
        }
        else
        {
            okProxyBake =
                glbopt::OptimizeGlbFile(rawProxyGlbPath.string(), proxyGlbPath.string(), proxyGlbOptions, proxyStats);
        }

        if (opt.debugAppearance)
        {
            if (opt.disableGlbopt)
            {
                std::cout << "[ProxyGlbOpt] tile=" << baseName
                          << " glbopt=bypass (disableGlbopt)\n";
            }
            else
            {
                const double diag = DiagonalLength(artifact.bounds);
                const std::size_t trisIn = proxyStats.InputPrimitiveElementCount / 3u;
                const std::size_t trisOut = proxyStats.OutputPrimitiveElementCount / 3u;
                double reductionPct = 0.0;
                if (trisIn > 0u)
                {
                    reductionPct = 100.0 *
                        (1.0 - (static_cast<double>(trisOut) / static_cast<double>(trisIn)));
                }
                reductionPct = ClampDouble(reductionPct, 0.0, 100.0);

                std::cout << "[ProxyGlbOpt] tile=" << baseName
                          << " size=" << diag
                          << " posStep=" << proxyGlbOptions.PositionStep
                          << " vertsIn=" << proxyStats.InputVertexCount
                          << " vertsOut=" << proxyStats.OutputVertexCount
                          << " mergedVerts=" << proxyStats.MergedVertexCount
                          << " trisIn=" << trisIn
                          << " trisOut=" << trisOut
                          << " triDelta%=" << reductionPct
                          << "\n";
            }
        }

        const bool expectStats = node.totalTriangles > 0;
        if (!okProxyBake)
        {
            return false;
        }

        if (!opt.disableGlbopt)
        {
            const bool geometryOk =
                ValidateTileGeometryStats(proxyStats, expectStats, baseName);
            const bool reductionOk =
                ValidateProxyReduction(proxyStats, expectStats, baseName);

            if (!geometryOk || !reductionOk)
            {
                return false;
            }

            if (!ValidateAppearanceCardinality(proxyStats, expectStats, baseName))
            {
                return false;
            }
        }

        const std::map<std::string, std::string> tileMetadata = {
            { "tileId", std::to_string(artifact.nodeId) },
            { "tileKind", "proxy" }
        };
        const bool okProxyB3dm =
            B3dm::WrapGlbFileToB3dmFile(proxyGlbPath.string(), proxyB3dmPath.string(), tileMetadata);

        if (!okProxyB3dm)
        {
            return false;
        }

        artifact.GlbPath = proxyGlbPath;
        artifact.ContentUri = JoinUri(opt.contentSubdir, baseName + ".b3dm");

        if (!opt.keepGlbFilesForDebug)
        {
            std::error_code ec;
            std::filesystem::remove(rawProxyGlbPath, ec);
        }

        return true;
    }

    static bool BakeArtifactsBottomUp(
        const TileOctree::Node& node,
        const core::SceneIR& sceneIr,
        const TilesetEmit::Options& opt,
        BakeState& bakeState,
        int depthFromRoot,
        int maxDepth,
        const core::Aabb& rootBounds)
    {
        for (const auto& childPtr : node.children)
        {
            if (!childPtr)
            {
                continue;
            }

            if (!BakeArtifactsBottomUp(
                    *childPtr,
                    sceneIr,
                    opt,
                    bakeState,
                    depthFromRoot + 1,
                    maxDepth,
                    rootBounds))
            {
                return false;
            }
        }

        BakedNodeArtifact artifact;
        artifact.nodeId = bakeState.nextNodeId++;
        artifact.isLeaf = !HasAnyChild(node);
        artifact.bounds = node.volume;

        if (opt.useTightBounds)
        {
            const core::Aabb tight = ComputeTightBounds(node, sceneIr);
            if (tight.valid)
            {
                artifact.bounds = tight;
            }
        }

        {
            const std::string tileLabel = opt.tileFilePrefix + std::to_string(artifact.nodeId);
            const bool expectNonEmpty = node.totalTriangles > 0;
            if (!ValidateBounds(artifact.bounds, expectNonEmpty, tileLabel))
            {
                return false;
            }
        }

        bool ok = false;
        if (artifact.isLeaf)
        {
            ok = BakeLeafArtifacts(
                node,
                sceneIr,
                opt,
                artifact,
                depthFromRoot,
                maxDepth,
                rootBounds);
        }
        else
        {
            if (opt.contentOnlyAtLeaves)
            {
                artifact.GlbPath.clear();
                artifact.ContentUri.clear();
                ok = true;
            }
            else
            {
                ok = BakeInternalProxyArtifact(
                    node,
                    sceneIr,
                    opt,
                    artifact,
                    depthFromRoot,
                    maxDepth,
                    rootBounds,
                    bakeState);
            }
        }

        if (!ok)
        {
            return false;
        }

        bakeState.artifacts[&node] = artifact;
        return true;
    }

    static void EmitJsonNode(
        std::ostringstream& ss,
        const TileOctree::Node& node,
        const BakeState& bakeState,
        int depth,
        int depthFromRoot,
        int maxDepth,
        double viewerTargetSse,
        const core::Aabb& rootBounds,
        double parentGeometricError,
        bool isRoot)
    {
        std::unordered_map<const TileOctree::Node*, BakedNodeArtifact>::const_iterator it =
            bakeState.artifacts.find(&node);

        if (it == bakeState.artifacts.end())
        {
            return;
        }

        const BakedNodeArtifact& artifact = it->second;

        Indent(ss, depth);
        ss << "{\n";

        Indent(ss, depth + 1);
        ss << "\"boundingVolume\":{\"box\":[";
        const std::array<double, 12> box = ToTilesBoxGltfSpace(artifact.bounds);
        for (int i = 0; i < 12; ++i)
        {
            if (i)
            {
                ss << ",";
            }
            ss << std::setprecision(15) << box[static_cast<std::size_t>(i)];
        }
        ss << "]}," << "\n";

        Indent(ss, depth + 1);
        double geometricError = 0.0;
        if (!artifact.isLeaf)
        {
            const NodeTuning tuning = BuildNodeTuning(
                artifact.bounds,
                rootBounds,
                depthFromRoot,
                maxDepth,
                false,
                viewerTargetSse);
            geometricError = tuning.GeometricError;
            if (!isRoot && parentGeometricError > 0.0)
            {
                const double maxAllowed = parentGeometricError * 0.97;
                geometricError = std::min(geometricError, maxAllowed);
            }
        }

        ss << "\"geometricError\":" << std::setprecision(15) << geometricError << ",\n";

        Indent(ss, depth + 1);
        ss << "\"refine\":\"REPLACE\"";

        if (!artifact.ContentUri.empty())
        {
            ss << ",\n";
            Indent(ss, depth + 1);
            ss << "\"content\":{\"uri\":\"" << artifact.ContentUri << "\"}";
        }

        std::vector<const TileOctree::Node*> children;
        children.reserve(8);
        for (const auto& childPtr : node.children)
        {
            if (childPtr)
            {
                children.push_back(childPtr.get());
            }
        }

        if (!children.empty())
        {
            ss << ",\n";
            Indent(ss, depth + 1);
            ss << "\"children\":[\n";

            for (std::size_t i = 0; i < children.size(); ++i)
            {
                EmitJsonNode(
                    ss,
                    *children[i],
                    bakeState,
                    depth + 2,
                    depthFromRoot + 1,
                    maxDepth,
                    viewerTargetSse,
                    rootBounds,
                    geometricError,
                    false);
                if (i + 1 < children.size())
                {
                    ss << ",";
                }
                ss << "\n";
            }

            Indent(ss, depth + 1);
            ss << "]";
        }

        ss << "\n";
        Indent(ss, depth);
        ss << "}";
    }
}

namespace TilesetEmit
{
    bool EmitTilesetAndB3dm(
        const TileOctree& tree,
        const Options& opt)
    {
        std::filesystem::create_directories(opt.tilesetOutDir);

        if (!opt.sceneIr)
        {
            std::cerr << "[TilesetEmit] SceneIR must be provided\n";
            return false;
        }

        const int maxDepth = ComputeMaxDepth(tree.Root());
        const core::Aabb rootBounds = tree.Root().volume;

        BakeState bakeState;
        if (!BakeArtifactsBottomUp(
                tree.Root(),
                *opt.sceneIr,
                opt,
                bakeState,
                0,
                maxDepth,
                rootBounds))
        {
            return false;
        }

        std::unordered_map<const TileOctree::Node*, BakedNodeArtifact>::const_iterator rootIt =
            bakeState.artifacts.find(&tree.Root());

        if (rootIt == bakeState.artifacts.end())
        {
            return false;
        }

        const NodeTuning rootTuning = BuildNodeTuning(
            rootIt->second.bounds,
            rootBounds,
            0,
            maxDepth,
            false,
            opt.viewerTargetSse);
        const double rootGeometricError = rootTuning.GeometricError;

        std::ostringstream ss;
        ss << "{\n";
        ss << "  \"asset\":{\"version\":\"1.0\"},\n";
        ss << "  \"geometricError\":" << std::setprecision(15) << rootGeometricError << ",\n";
        ss << "  \"root\":\n";

        EmitJsonNode(
            ss,
            tree.Root(),
            bakeState,
            2,
            0,
            maxDepth,
            opt.viewerTargetSse,
            rootBounds,
            rootGeometricError,
            true);

        ss << "\n}\n";

        const std::filesystem::path tilesetPath =
            std::filesystem::path(opt.tilesetOutDir) / "tileset.json";

        std::ofstream f(tilesetPath);
        if (!f)
        {
            return false;
        }

        f << ss.str();
        return true;
    }
}
#include "tileset_emit.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <map>
#include <ostream>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <system_error>
#include <cmath>
#include <utility>

#include "b3dm.h"
#include "glb_compose_instancing.h"
#include "glbopt.h"
#include "tiler/node_scale_helpers.h"

namespace
{
    constexpr double kProxyMergeMaxTrianglesRatioVsLeafHigh = 0.50;

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

    // #region agent log
    static void AgentNdjsonLogLeafGlboptFb(
        const std::uint32_t tileNodeId,
        const std::string& sourceFormat,
        const glbopt::Stats& s)
    {
        static int s_logged = 0;
        if (sourceFormat != "fbx" || s_logged >= 16)
        {
            return;
        }
        ++s_logged;
        std::ofstream f(
            "/Users/ikeherman/Desktop/Github/web-step-viewer-test/.cursor/debug-c5c908.log",
            std::ios::app);
        if (!f)
        {
            return;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
        const std::size_t inTris = s.InputPrimitiveElementCount / 3u;
        const std::size_t outTris = s.OutputPrimitiveElementCount / 3u;
        f << "{\"sessionId\":\"c5c908\",\"runId\":\"post-fix\",\"hypothesisId\":\"H-F\""
             ",\"location\":\"tileset_emit.cpp:BakeLeafArtifacts\""
             ",\"message\":\"leaf_glbopt_stats\""
             ",\"data\":{"
             "\"tileNodeId\":" << tileNodeId
             << ",\"inTris\":" << inTris
             << ",\"outTris\":" << outTris
             << ",\"removedSimplify\":" << s.TrianglesRemovedSimplify
             << ",\"dropDegArea\":" << s.DroppedDegenerateByArea
             << ",\"dropDegId\":" << s.DroppedDegenerateById
             << ",\"dropInvIdx\":" << s.DroppedTrianglesInvalidIndices
             << ",\"weldPhaseTrisRemoved\":" << s.TrianglesRemovedWeldPhase
             << ",\"simplifyTrisRemoved\":" << s.TrianglesRemovedSimplify
             << "},\"timestamp\":" << ms << "}\n";
    }
    // #endregion

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

    static void ScaleGlboptAggression(glbopt::Options& o, float factor);

    static void CollectFilteredLeafInstances(
        const std::vector<std::uint32_t>& itemIndices,
        const core::Aabb& tileBounds,
        double minSizeRatio,
        const core::SceneIR& sceneIr,
        std::vector<std::pair<std::uint32_t, core::Transform4d>>& outInstances)
    {
        outInstances.clear();
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

            if (matchedInstance->highLodGlbUri.empty())
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

            outInstances.emplace_back(matchedInstance->prototypeId, matchedInstance->worldTransform);
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

    /// True when glbopt produced a mesh we can emit as tile content (non-empty triangle list).
    static bool HasRenderableTriangleMesh(const glbopt::Stats& stats)
    {
        if (stats.PrimitiveCountMergedOut == 0)
        {
            return false;
        }
        if (stats.OutputPrimitiveElementCount < 3)
        {
            return false;
        }
        if ((stats.OutputPrimitiveElementCount % 3u) != 0u)
        {
            return false;
        }
        if (stats.OutputVertexCount < 3)
        {
            return false;
        }
        return true;
    }

    [[maybe_unused]] static bool ValidateProxyReduction(
        const glbopt::Stats& stats,
        bool expectNonEmpty,
        const std::string& label)
    {
        const std::uint64_t trisIn = stats.InputPrimitiveElementCount / 3u;
        if (!expectNonEmpty || trisIn == 0)
        {
            return true;
        }

        const std::uint64_t trisOut = stats.OutputPrimitiveElementCount / 3u;
        const double ratio = static_cast<double>(trisOut) / static_cast<double>(trisIn);

        if (ratio <= 1e-4)
        {
            std::cout << "[TilesetEmit] rejected " << label
                      << ": proxy collapsed too aggressively (triangle ratio=" << ratio << ")\n";
            return false;
        }

        return true;
    }

    static std::string GlboptTriCountsDebug(const glbopt::Stats& s)
    {
        std::ostringstream os;
        os << "trisOut=" << (s.OutputPrimitiveElementCount / 3u)
           << " trisIn=" << (s.InputPrimitiveElementCount / 3u)
           << " triPrimsOut=" << s.PrimitiveCountMergedOut;
        return os.str();
    }

    [[maybe_unused]] static bool ValidateAppearanceCardinality(
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

    static void Indent(std::ostream& out, int depth)
    {
        for (int i = 0; i < depth; ++i)
        {
            out << "  ";
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

    struct NodeTuning
    {
        float RelativeSize = 0.0f;
        float NormalizedDepth = 0.0f;
        float Importance = 0.0f;

        float ProxyPositionStep = 1e-5f;
        float LeafPositionStep = 1e-5f;

        // glbopt weld quantization: global epsilon (`node_scale_helpers`), not octree-scaled; Leaf_* mirrors Proxy_*.
        float ProxyNormalStep = 1e-4f;
        float ProxyTexcoordStep = 1e-5f;
        float LeafNormalStep = 1e-4f;
        float LeafTexcoordStep = 1e-6f;

        float ProxySimplifyRatio = 0.15f;
        float ProxySimplifyError = 1e-2f;

        float LeafSimplifyRatio = 0.85f;
        float LeafSimplifyError = 2e-3f;

        double GeometricError = 0.0;
    };

    // SSE-inspired importance:
    // 1. Larger nodes relative to root stay less aggressive.
    // 2. Deeper nodes get somewhat more aggressive.
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

        const double sseScale =
            model2tile::ClampViewerSseScaleTileDefault(viewerTargetSse);

        const double positionStepDiag = model2tile::GlobalVertexWeldPositionStep();
        const float positionStep =
            static_cast<float>(model2tile::ClampDouble(positionStepDiag, 1e-12, 1e9));
        const float normalStep =
            static_cast<float>(model2tile::ClampDouble(
                model2tile::ComputeWeldNormalStepFromPositionStep(positionStepDiag),
                1e-12,
                1.0));
        const float texcoordStep =
            static_cast<float>(model2tile::ClampDouble(
                model2tile::ComputeWeldTexcoordStepFromPositionStep(positionStepDiag),
                1e-12,
                1.0));

        // Lower ratios = fewer triangles retained (more aggressive LOD).
        const float simplifyRatio =
            LerpClamped(0.18f, 0.42f, 1.0f - aggressiveness);

        tuning.ProxyPositionStep = positionStep;
        tuning.LeafPositionStep = positionStep;
        tuning.ProxyNormalStep = normalStep;
        tuning.LeafNormalStep = normalStep;
        tuning.ProxyTexcoordStep = texcoordStep;
        tuning.LeafTexcoordStep = texcoordStep;
        tuning.ProxySimplifyRatio = std::max(0.02f, simplifyRatio * 0.45f);
        tuning.LeafSimplifyRatio = simplifyRatio;

        // Geometric error model (octree-aligned): same diagonal-based error for simplify tuning on
        // all nodes. Tileset JSON still uses 0 for leaf tiles (`geometricError` on content leaves).
        constexpr double kGeomErrFraction = 0.25;
        constexpr double kGeomErrMinFraction = 0.08;
        constexpr double kGeomErrMaxFraction = 0.35;

        const double geomErrForSimplify = model2tile::ClampDiagonalGeometricError(
            nodeDiag,
            sseScale,
            kGeomErrFraction,
            kGeomErrMinFraction,
            kGeomErrMaxFraction);

        tuning.GeometricError = isLeaf ? 0.0 : geomErrForSimplify;

        const double errorFromGeom =
            std::max(1e-8, geomErrForSimplify / std::max(1e-9, rootDiag));

        // Larger scale = tolerate more collapses → stronger simplification toward SimplifyRatio.
        constexpr double kSimplifyErrorScale = 17.0;
        const double simplifyErrorUnscaled =
            std::max(0.0022, errorFromGeom * 0.62) *
            (0.92 + 0.34 * static_cast<double>(tuning.NormalizedDepth));
        const float simplifyError = static_cast<float>(model2tile::ClampDouble(
            simplifyErrorUnscaled * kSimplifyErrorScale,
            0.0022 * kSimplifyErrorScale,
            0.034 * kSimplifyErrorScale));
        tuning.ProxySimplifyError = simplifyError * 5.0f;
        tuning.LeafSimplifyError = simplifyError;

        return tuning;
    }

    /// Weld quantization from `NodeTuning` (Proxy_* / Leaf_* steps are identical after `BuildNodeTuning`).
    static void ApplyNodeTuningWeldQuantization(glbopt::Options& o, const NodeTuning& tuning)
    {
        o.PositionStep = tuning.ProxyPositionStep;
        o.NormalStep = tuning.ProxyNormalStep;
        o.TexcoordStep = tuning.ProxyTexcoordStep;
        o.ColorStep = 1.0f / 255.0f;
    }

    /// Second-phase proxy merge: weld + dedup + degenerates + simplify + vertex layout on merged proxy.
    /// Uses proxy-specific simplify tuning so internal LODs can be substantially cheaper than leaf detail.
    static glbopt::Options BuildProxyCombineAfterLeafDownscaleOptions(const NodeTuning& tuning)
    {
        glbopt::Options o;
        o.DeduplicateMaterials = true;
        o.WeldPositions = true;
        o.WeldNormals = true;
        o.WeldTexcoord0 = true;
        o.WeldColor0 = true;
        o.ForceDoubleSidedMaterials = false;
        ApplyNodeTuningWeldQuantization(o, tuning);
        o.Simplify = true;
        o.SimplifyRatio = tuning.ProxySimplifyRatio;
        o.SimplifyError = tuning.ProxySimplifyError;
        o.SimplifyNormalWeight = 0.10f;
        o.SimplifyTexcoordWeight = 0.14f;
        o.SimplifyColorWeight = 0.04f;
        o.RemoveDegenerateByIndex = true;
        o.RemoveDegenerateByArea = true;
        o.DegenerateAreaEpsilonSq = 1e-20f;
        o.OptimizeVertexCache = true;
        o.OptimizeOverdraw = true;
        o.OverdrawThreshold = 1.08f;
        o.OptimizeVertexFetch = true;
        o.SimplifySloppyIfStuck = true;
        return o;
    }

    /// Leaf glbopt pass: weld, dedup, degenerate cleanup, simplify, vertex layout.
    static glbopt::Options BuildLeafGlbOptOptionsForNode(const NodeTuning& tuning)
    {
        glbopt::Options options;

        options.DeduplicateMaterials = true;
        options.WeldPositions = true;
        options.WeldNormals = true;
        options.WeldTexcoord0 = true;
        options.WeldColor0 = true;
        options.ForceDoubleSidedMaterials = false;

        ApplyNodeTuningWeldQuantization(options, tuning);

        options.RemoveDegenerateByIndex = true;
        options.RemoveDegenerateByArea = true;

        options.Simplify = true;
        options.SimplifyRatio = tuning.LeafSimplifyRatio;
        options.SimplifyError = tuning.LeafSimplifyError;
        options.SimplifyNormalWeight = 0.10f;
        options.SimplifyTexcoordWeight = 0.14f;
        options.SimplifyColorWeight = 0.04f;

        options.OptimizeVertexCache = true;
        options.OptimizeOverdraw = true;
        options.OverdrawThreshold = 1.08f;
        options.OptimizeVertexFetch = true;

        return options;
    }

    static glbopt::Options BuildLowForParentGlbOptOptions(const NodeTuning& parentTuning)
    {
        glbopt::Options options = BuildLeafGlbOptOptionsForNode(parentTuning);
        // Low-for-parent should be more aggressive than the node's high tile pass.
        ScaleGlboptAggression(options, 6.5f);
        options.SimplifyNormalWeight = 0.10f;
        options.SimplifyTexcoordWeight = 0.14f;
        options.SimplifyColorWeight = 0.04f;
        options.SimplifySloppyIfStuck = true;
        return options;
    }

    struct BakedNodeArtifact
    {
        std::uint32_t nodeId = 0;
        bool isLeaf = false;
        core::Aabb bounds;
        std::uint64_t HighTriangleCount = 0;
        std::uint64_t LowForParentTriangleCount = 0;
        std::uint64_t SubtreeLeafHighTriangleCount = 0;
        std::filesystem::path HighGlbPath;
        std::filesystem::path LowForParentGlbPath;
        std::filesystem::path GlbPath;
        std::string ContentUri;
    };

    struct BakeState
    {
        std::uint32_t nextNodeId = 0;
        std::unordered_map<const TileOctree::Node*, BakedNodeArtifact> artifacts;
    };

    static void CollectImmediateChildLowGlbPaths(
        const TileOctree::Node& node,
        const BakeState& bakeState,
        std::vector<std::string>& outPaths)
    {
        outPaths.clear();
        for (const auto& childPtr : node.children)
        {
            if (!childPtr)
            {
                continue;
            }
            const auto it = bakeState.artifacts.find(childPtr.get());
            if (it == bakeState.artifacts.end() || it->second.LowForParentGlbPath.empty())
            {
                continue;
            }
            outPaths.push_back(it->second.LowForParentGlbPath.string());
        }
    }

    static void ScaleGlboptAggression(glbopt::Options& o, float factor)
    {
        if (!(factor > 1.0f))
        {
            return;
        }
        // Weld quantization is fixed global epsilon; only simplify is scaled here.
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
        const core::Aabb* parentCellBounds,
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
        const std::filesystem::path lowGlbPath = tilesDir / (baseName + "_low.glb");
        const std::filesystem::path fullB3dmPath = tilesDir / (baseName + ".b3dm");

        std::vector<std::pair<std::uint32_t, core::Transform4d>> leafInstances;
        CollectFilteredLeafInstances(
            node.items,
            artifact.bounds,
            opt.instanceMinSizeRatio,
            sceneIr,
            leafInstances);

        if (leafInstances.empty())
        {
            artifact.GlbPath.clear();
            artifact.ContentUri.clear();
            return true;
        }

        std::string composeErr;
        glb_compose::InstancedLeafComposeStats composeStats;
        if (!glb_compose::ComposeInstancedLeafGlb(
                leafInstances,
                sceneIr,
                opt.tilesetOutDir,
                rawFullGlbPath.string(),
                composeErr,
                &composeStats))
        {
            std::cerr << "[TilesetEmit] instanced leaf compose failed: " << composeErr << "\n";
            return false;
        }

        const NodeTuning leafTuning = BuildNodeTuning(
            artifact.bounds,
            rootBounds,
            depthFromRoot,
            maxDepth,
            true,
            opt.viewerTargetSse);

        glbopt::Stats leafOptStats;
        glbopt::Options leafGlbOpt = BuildLeafGlbOptOptionsForNode(leafTuning);
        if (!glbopt::OptimizeGlbFile(
                rawFullGlbPath.string(),
                fullGlbPath.string(),
                leafGlbOpt,
                leafOptStats,
                "TileHigh"))
        {
            std::cerr << "[TilesetEmit] leaf high optimize failed tile=" << baseName << "\n";
            return false;
        }
        if (!HasRenderableTriangleMesh(leafOptStats))
        {
            std::cout << "[TilesetEmit] skipping tile=" << baseName
                      << " kind=leaf: mesh degenerated to empty/non-renderable"
                      << " (" << GlboptTriCountsDebug(leafOptStats) << ")\n";
            std::error_code ec;
            std::filesystem::remove(fullGlbPath, ec);
            std::filesystem::remove(rawFullGlbPath, ec);
            artifact.GlbPath.clear();
            artifact.ContentUri.clear();
            artifact.HighGlbPath.clear();
            artifact.LowForParentGlbPath.clear();
            artifact.HighTriangleCount = 0;
            artifact.LowForParentTriangleCount = 0;
            artifact.SubtreeLeafHighTriangleCount = 0;
            return true;
        }

        // #region agent log
        AgentNdjsonLogLeafGlboptFb(artifact.nodeId, sceneIr.sourceFormat, leafOptStats);
        // #endregion

        if (parentCellBounds && parentCellBounds->valid)
        {
            const NodeTuning parentTuning = BuildNodeTuning(
                *parentCellBounds,
                rootBounds,
                std::max(0, depthFromRoot - 1),
                maxDepth,
                false,
                opt.viewerTargetSse);
            glbopt::Options lowOptions = BuildLowForParentGlbOptOptions(parentTuning);
            glbopt::Stats lowStats{};
            if (!glbopt::OptimizeGlbFile(
                    fullGlbPath.string(),
                    lowGlbPath.string(),
                    lowOptions,
                    lowStats,
                    "TileLowForParent"))
            {
                std::cerr << "[TilesetEmit] leaf low optimize failed tile=" << baseName << "\n";
                return false;
            }
            artifact.LowForParentGlbPath = lowGlbPath;
            artifact.LowForParentTriangleCount =
                static_cast<std::uint64_t>(lowStats.OutputPrimitiveElementCount / 3u);
        }
        artifact.HighTriangleCount =
            static_cast<std::uint64_t>(leafOptStats.OutputPrimitiveElementCount / 3u);
        if (artifact.LowForParentTriangleCount == 0)
        {
            artifact.LowForParentTriangleCount = artifact.HighTriangleCount;
        }
        artifact.SubtreeLeafHighTriangleCount = artifact.HighTriangleCount;

        std::cout << "[TileExport] tile=" << baseName << " kind=leaf"
                  << " materialsCanonical=" << leafOptStats.MaterialCountCanonical
                  << " materialsInputSlots=" << composeStats.materials
                  << " tris=" << (leafOptStats.OutputPrimitiveElementCount / 3) << "\n";

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

        artifact.HighGlbPath = fullGlbPath;
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
        const core::Aabb* parentCellBounds,
        int depthFromRoot,
        int maxDepth,
        const core::Aabb& rootBounds,
        const BakeState& bakeState)
    {
        const std::string baseName = opt.tileFilePrefix + std::to_string(artifact.nodeId);
        const std::filesystem::path tilesDir =
            std::filesystem::path(opt.tilesetOutDir) / opt.contentSubdir;

        std::filesystem::create_directories(tilesDir);

        const std::filesystem::path proxyGlbPath = tilesDir / (baseName + ".glb");
        const std::filesystem::path lowGlbPath = tilesDir / (baseName + "_low.glb");
        const std::filesystem::path proxyB3dmPath = tilesDir / (baseName + ".b3dm");

        std::vector<std::string> childLowGlbPaths;
        CollectImmediateChildLowGlbPaths(node, bakeState, childLowGlbPaths);
        std::uint64_t childLeafHighTrisSum = 0;
        std::uint64_t childLowTrisSum = 0;
        for (const auto& childPtr : node.children)
        {
            if (!childPtr)
            {
                continue;
            }
            const auto it = bakeState.artifacts.find(childPtr.get());
            if (it == bakeState.artifacts.end())
            {
                continue;
            }
            childLeafHighTrisSum += it->second.SubtreeLeafHighTriangleCount;
            childLowTrisSum += it->second.LowForParentTriangleCount;
        }

        if (childLowGlbPaths.empty())
        {
            artifact.GlbPath.clear();
            artifact.ContentUri.clear();
            return true;
        }

        const NodeTuning highTuning = BuildNodeTuning(
            node.volume,
            rootBounds,
            depthFromRoot,
            maxDepth,
            false,
            opt.viewerTargetSse);

        glbopt::Options mergePassOpts = BuildProxyCombineAfterLeafDownscaleOptions(highTuning);
        mergePassOpts.ForceDoubleSidedMaterials = false;
        if (childLeafHighTrisSum > 0 &&
            childLeafHighTrisSum > opt.proxyMergeRatioMinLeafHighTris)
        {
            const std::uint64_t ratioBudget = std::max<std::uint64_t>(
                1u,
                static_cast<std::uint64_t>(
                    std::ceil(
                        static_cast<double>(childLeafHighTrisSum) *
                        kProxyMergeMaxTrianglesRatioVsLeafHigh)));
            mergePassOpts.MaxTriangles =
                std::min(ratioBudget, opt.proxyMergeMaxTrianglesHardCap);
        }
        std::cout << "[ProxyMerge] parent=" << baseName << " mergeInputs=" << childLowGlbPaths.size()
                  << " leafHighSum=" << childLeafHighTrisSum << " childLowSum=" << childLowTrisSum;
        if (mergePassOpts.MaxTriangles > 0)
        {
            std::cout << " maxTriangles=" << mergePassOpts.MaxTriangles;
        }
        else if (childLeafHighTrisSum > 0 &&
                 childLeafHighTrisSum <= opt.proxyMergeRatioMinLeafHighTris)
        {
            std::cout << " ratioBudget=skipped(leafHighSum<="
                      << opt.proxyMergeRatioMinLeafHighTris << ")";
        }
        std::cout << "\n";
        {
            std::size_t ordinal = 0;
            for (const auto& childPtr : node.children)
            {
                if (!childPtr)
                {
                    continue;
                }
                const auto it = bakeState.artifacts.find(childPtr.get());
                if (it == bakeState.artifacts.end() || it->second.LowForParentGlbPath.empty())
                {
                    continue;
                }
                const std::string childBase =
                    opt.tileFilePrefix + std::to_string(it->second.nodeId);
                std::cout << "[ProxyMerge]   child[" << ordinal << "] tile=" << childBase
                          << " glb=" << it->second.LowForParentGlbPath.filename().string()
                          << " subtreeLeafHighTris=" << it->second.SubtreeLeafHighTriangleCount
                          << " lowTris=" << it->second.LowForParentTriangleCount << "\n";
                ++ordinal;
            }
        }
        glbopt::Stats proxyStats;
        if (!glbopt::OptimizeGlbFiles(
                childLowGlbPaths,
                proxyGlbPath.string(),
                mergePassOpts,
                proxyStats,
                "ProxyMerge"))
        {
            std::cerr << "[TilesetEmit] proxy merge failed tile=" << baseName << "\n";
            return false;
        }
        if (!HasRenderableTriangleMesh(proxyStats))
        {
            std::cout << "[TilesetEmit] skipping tile=" << baseName
                      << " kind=proxy: merged mesh degenerated to empty/non-renderable"
                      << " (" << GlboptTriCountsDebug(proxyStats) << ")\n";
            std::error_code ec;
            std::filesystem::remove(proxyGlbPath, ec);
            artifact.GlbPath.clear();
            artifact.ContentUri.clear();
            artifact.HighGlbPath.clear();
            artifact.LowForParentGlbPath.clear();
            artifact.HighTriangleCount = 0;
            artifact.LowForParentTriangleCount = 0;
            artifact.SubtreeLeafHighTriangleCount = childLeafHighTrisSum;
            return true;
        }
        artifact.HighTriangleCount =
            static_cast<std::uint64_t>(proxyStats.OutputPrimitiveElementCount / 3u);

        if (parentCellBounds && parentCellBounds->valid)
        {
            const NodeTuning parentTuning = BuildNodeTuning(
                *parentCellBounds,
                rootBounds,
                std::max(0, depthFromRoot - 1),
                maxDepth,
                false,
                opt.viewerTargetSse);
            glbopt::Options lowOptions = BuildLowForParentGlbOptOptions(parentTuning);
            glbopt::Stats lowStats{};
            if (!glbopt::OptimizeGlbFile(
                    proxyGlbPath.string(),
                    lowGlbPath.string(),
                    lowOptions,
                    lowStats,
                    "TileLowForParent"))
            {
                std::cerr << "[TilesetEmit] proxy low optimize failed tile=" << baseName << "\n";
                return false;
            }
            artifact.LowForParentGlbPath = lowGlbPath;
            artifact.LowForParentTriangleCount =
                static_cast<std::uint64_t>(lowStats.OutputPrimitiveElementCount / 3u);
        }
        if (artifact.LowForParentTriangleCount == 0)
        {
            artifact.LowForParentTriangleCount = artifact.HighTriangleCount;
        }
        artifact.SubtreeLeafHighTriangleCount = childLeafHighTrisSum;

        const double ratioVsLeafHigh = childLeafHighTrisSum > 0
            ? static_cast<double>(artifact.HighTriangleCount) /
                static_cast<double>(childLeafHighTrisSum)
            : 0.0;
        const double ratioVsChildLow = childLowTrisSum > 0
            ? static_cast<double>(artifact.HighTriangleCount) /
                static_cast<double>(childLowTrisSum)
            : 0.0;
        std::cout << "[TileExport] tile=" << baseName << " kind=proxy"
                  << " children=" << childLowGlbPaths.size()
                  << " materials=" << proxyStats.MaterialCountCanonical
                  << " tris=" << (proxyStats.OutputPrimitiveElementCount / 3)
                  << " leafHighSum=" << childLeafHighTrisSum
                  << " childLowSum=" << childLowTrisSum
                  << " ratioVsLeafHigh=" << std::fixed << std::setprecision(3) << ratioVsLeafHigh
                  << " ratioVsChildLow=" << ratioVsChildLow
                  << std::defaultfloat << "\n";

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

        artifact.HighGlbPath = proxyGlbPath;
        artifact.GlbPath = proxyGlbPath;
        artifact.ContentUri = JoinUri(opt.contentSubdir, baseName + ".b3dm");

        return true;
    }

    static bool BakeArtifactsBottomUp(
        const TileOctree::Node& node,
        const core::SceneIR& sceneIr,
        const TilesetEmit::Options& opt,
        BakeState& bakeState,
        const core::Aabb* parentCellBounds,
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
                    &node.volume,
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
                parentCellBounds,
                depthFromRoot,
                maxDepth,
                rootBounds);
        }
        else
        {
            ok = BakeInternalProxyArtifact(
                node,
                sceneIr,
                opt,
                artifact,
                parentCellBounds,
                depthFromRoot,
                maxDepth,
                rootBounds,
                bakeState);
        }

        if (!ok)
        {
            return false;
        }

        bakeState.artifacts[&node] = artifact;
        return true;
    }

    static void EmitJsonNode(
        std::ostream& out,
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

        Indent(out, depth);
        out << "{\n";

        Indent(out, depth + 1);
        out << "\"boundingVolume\":{\"box\":[";
        const std::array<double, 12> box = ToTilesBoxGltfSpace(artifact.bounds);
        for (int i = 0; i < 12; ++i)
        {
            if (i)
            {
                out << ",";
            }
            out << std::setprecision(15) << box[static_cast<std::size_t>(i)];
        }
        out << "]}," << "\n";

        Indent(out, depth + 1);
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

        out << "\"geometricError\":" << std::setprecision(15) << geometricError << ",\n";

        Indent(out, depth + 1);
        out << "\"refine\":\"REPLACE\"";

        if (!artifact.ContentUri.empty())
        {
            out << ",\n";
            Indent(out, depth + 1);
            out << "\"content\":{\"uri\":\"" << artifact.ContentUri << "\"}";
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
            out << ",\n";
            Indent(out, depth + 1);
            out << "\"children\":[\n";

            for (std::size_t i = 0; i < children.size(); ++i)
            {
                EmitJsonNode(
                    out,
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
                    out << ",";
                }
                out << "\n";
            }

            Indent(out, depth + 1);
            out << "]";
        }

        out << "\n";
        Indent(out, depth);
        out << "}";
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
                nullptr,
                0,
                maxDepth,
                rootBounds))
        {
            return false;
        }

        {
            const std::uint32_t maxTileId =
                bakeState.nextNodeId > 0 ? bakeState.nextNodeId - 1u : 0u;
            std::cout << "[TileExport] baked " << bakeState.artifacts.size() << " tiles (tile ids 0.."
                      << maxTileId << "); writing tileset.json\n";
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

        const std::filesystem::path tilesetPath =
            std::filesystem::path(opt.tilesetOutDir) / "tileset.json";

        std::ofstream f(tilesetPath);
        if (!f)
        {
            return false;
        }

        f << "{\n";
        f << "  \"asset\":{\"version\":\"1.0\"},\n";
        f << "  \"geometricError\":" << std::setprecision(15) << rootGeometricError << ",\n";
        f << "  \"root\":\n";

        EmitJsonNode(
            f,
            tree.Root(),
            bakeState,
            2,
            0,
            maxDepth,
            opt.viewerTargetSse,
            rootBounds,
            rootGeometricError,
            true);

        f << "\n}\n";
        f.flush();
        std::cout << "[TileExport] wrote tileset.json (" << tilesetPath.string() << ")\n";
        return true;
    }
}
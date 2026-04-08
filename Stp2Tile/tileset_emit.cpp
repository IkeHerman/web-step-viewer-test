#include "tileset_emit.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <system_error>
#include <cmath>

#include <Bnd_Box.hxx>
#include <Message_ProgressRange.hxx>

#include "export_glb.h"
#include "b3dm.h"
#include "glbopt.h"

namespace
{
    static void GetMinMax(const Bnd_Box& b,
                          double& xmin, double& ymin, double& zmin,
                          double& xmax, double& ymax, double& zmax)
    {
        b.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    }

    static double DiagonalLength(const Bnd_Box& b)
    {
        if (b.IsVoid())
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

    static gp_Pnt ToGltfSpace(const gp_Pnt& p)
    {
        return gp_Pnt(p.X(), -p.Z(), p.Y());
    }

    static gp_Vec ToGltfSpace(const gp_Vec& v)
    {
        return gp_Vec(v.X(), -v.Z(), v.Y());
    }

    static std::array<double, 12> ToTilesBoxGltfSpace(const Bnd_Box& b)
    {
        double xmin, ymin, zmin, xmax, ymax, zmax;
        GetMinMax(b, xmin, ymin, zmin, xmax, ymax, zmax);

        gp_Pnt cOcct(
            (xmin + xmax) * 0.5,
            (ymin + ymax) * 0.5,
            (zmin + zmax) * 0.5
        );

        const double hxLen = (xmax - xmin) * 0.5;
        const double hyLen = (ymax - ymin) * 0.5;
        const double hzLen = (zmax - zmin) * 0.5;

        gp_Vec hxOcct(hxLen, 0.0, 0.0);
        gp_Vec hyOcct(0.0, hyLen, 0.0);
        gp_Vec hzOcct(0.0, 0.0, hzLen);

        gp_Pnt c = ToGltfSpace(cOcct);
        gp_Vec hx = ToGltfSpace(hxOcct);
        gp_Vec hy = ToGltfSpace(hyOcct);
        gp_Vec hz = ToGltfSpace(hzOcct);

        return {
            c.X(),  c.Y(),  c.Z(),
            hx.X(), hx.Y(), hx.Z(),
            hy.X(), hy.Y(), hy.Z(),
            hz.X(), hz.Y(), hz.Z()
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

    static bool IsFinite(double v)
    {
        return std::isfinite(v);
    }

    static bool ValidateBounds(const Bnd_Box& b, bool expectNonEmpty, const std::string& label)
    {
        if (b.IsVoid())
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

    static void AccumulateTightBounds(
        const TileOctree::Node& node,
        const std::vector<Occurrence>& occs,
        Bnd_Box& out)
    {
        for (std::uint32_t idx : node.items)
        {
            if (idx >= occs.size())
            {
                continue;
            }

            const Bnd_Box& ob = occs[static_cast<std::size_t>(idx)].WorldBounds;
            if (ob.IsVoid())
            {
                continue;
            }

            out.Add(ob);
        }

        for (const auto& c : node.children)
        {
            if (!c)
            {
                continue;
            }

            AccumulateTightBounds(*c, occs, out);
        }
    }

    static Bnd_Box ComputeTightBounds(const TileOctree::Node& node, const std::vector<Occurrence>& occs)
    {
        Bnd_Box b;
        b.SetVoid();
        AccumulateTightBounds(node, occs, b);

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

    struct NodeTuning
    {
        float RelativeSize = 0.0f;
        float NormalizedDepth = 0.0f;
        float Importance = 0.0f;

        float ProxyPositionStep = 1e-5f;
        float LeafPositionStep = 1e-5f;

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
        const Bnd_Box& nodeBounds,
        const Bnd_Box& rootBounds,
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
        const Bnd_Box& nodeBounds,
        const Bnd_Box& rootBounds,
        int depthFromRoot,
        int maxDepth,
        bool isLeaf)
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

        const double rootStepMin = rootDiag * 1e-9;
        const double rootStepProxyMax = rootDiag * 5e-4;
        const double rootStepLeafMax = rootDiag * 2e-5;

        const double proxyBaseStep = rootDiag * 1e-6;
        const double leafBaseStep = rootDiag * 2e-7;

        const double proxyStep = ClampDouble(
            proxyBaseStep * (0.35 + 6.0 * static_cast<double>(aggressiveness)),
            rootStepMin,
            std::max(rootStepMin, rootStepProxyMax));

        const double leafStep = ClampDouble(
            leafBaseStep * (0.8 + 1.6 * static_cast<double>(tuning.NormalizedDepth)),
            rootStepMin,
            std::max(rootStepMin, rootStepLeafMax));

        tuning.ProxyPositionStep = static_cast<float>(proxyStep);
        tuning.LeafPositionStep = static_cast<float>(leafStep);

        // Keep proxy reduction aggressive, but tie to depth and relative size.
        tuning.ProxySimplifyRatio =
            LerpClamped(0.06f, 0.34f, 1.0f - aggressiveness);

        // Mild leaf optimization with conservative retention.
        const float leafAggressiveness =
            Clamp01(tuning.NormalizedDepth * 0.5f + (1.0f - tuning.RelativeSize) * 0.5f);
        tuning.LeafSimplifyRatio =
            LerpClamped(0.98f, 0.90f, leafAggressiveness);

        // Geometric error model scales with both overall extent and node extent.
        if (!isLeaf)
        {
            const double depthBase = (rootDiag / 8.0) * std::pow(0.72, std::max(0, depthFromRoot));
            const double nodeCap = nodeDiag / 3.5;
            tuning.GeometricError = std::max(0.0, std::min(depthBase, nodeCap));
        }
        else
        {
            tuning.GeometricError = 0.0;
        }

        const double errorFromGeom = std::max(1e-8, tuning.GeometricError / std::max(1e-9, rootDiag));

        tuning.ProxySimplifyError = static_cast<float>(ClampDouble(
            std::max(0.0025, errorFromGeom * 0.75) * (0.85 + 0.55 * static_cast<double>(tuning.NormalizedDepth)),
            0.0025,
            0.06));

        tuning.LeafSimplifyError = static_cast<float>(ClampDouble(
            std::max(0.0005, errorFromGeom * 0.2),
            0.0005,
            0.008));

        return tuning;
    }

    static glbopt::Options BuildProxyGlbOptOptionsForNode(const NodeTuning& tuning)
    {
        glbopt::Options options;

        // Keep proxy reduction deterministic; avoid cross-material collapsing.
        options.DeduplicateMaterials = false;

        // Keep position welding enabled for proxies to avoid ballooning duplicated
        // vertices across merged child content.
        options.WeldPositions = true;
        options.WeldNormals = false;
        options.WeldTexcoord0 = false;
        options.WeldColor0 = false;
        // Preserve source vertex colors in proxy tiles. Some CAD exports rely on
        // COLOR_0 when explicit glTF material bindings are sparse or missing.
        options.DropAllBlackColor0 = false;
        options.StripColor0Always = false;
        options.ForceDefaultMaterialForMissing = true;

        options.RemoveDegenerateByIndex = true;
        options.RemoveDegenerateByArea = true;

        options.OptimizeVertexCache = true;
        options.OptimizeOverdraw = false;
        options.OptimizeVertexFetch = true;

        options.PositionStep = tuning.ProxyPositionStep;
        options.NormalStep   = 0.001f;
        options.TexcoordStep = 0.0001f;
        options.ColorStep    = 1.0f / 255.0f;

        options.DegenerateAreaEpsilonSq = 1e-20f;
        options.Simplify = true;
        options.SimplifyRatio = tuning.ProxySimplifyRatio;
        options.SimplifyError = tuning.ProxySimplifyError;
        options.OverdrawThreshold = 1.05f;

        return options;
    }

    static glbopt::Options BuildLeafGlbOptOptionsForNode(const NodeTuning& tuning)
    {
        glbopt::Options options;

        options.DeduplicateMaterials = false;

        options.WeldPositions = true;
        options.WeldNormals = true;
        options.WeldTexcoord0 = true;
        options.WeldColor0 = true;

        // Keep leaf weld conservative to reduce risk of topology damage.
        options.PositionStep = tuning.LeafPositionStep;
        options.NormalStep = 0.001f;
        options.TexcoordStep = 0.0001f;
        options.ColorStep = 1.0f / 255.0f;

        options.RemoveDegenerateByIndex = true;
        options.RemoveDegenerateByArea = true;

        options.Simplify = true;
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
        Bnd_Box bounds;
        std::filesystem::path GlbPath;
        std::string ContentUri;
    };

    struct BakeState
    {
        std::uint32_t nextNodeId = 0;
        std::unordered_map<const TileOctree::Node*, BakedNodeArtifact> artifacts;
    };

    static bool BakeLeafArtifacts(
        const TileOctree::Node& node,
        const std::vector<Occurrence>& occurrences,
        const TilesetEmit::Options& opt,
        BakedNodeArtifact& artifact,
        int depthFromRoot,
        int maxDepth,
        const Bnd_Box& rootBounds)
    {
        const std::string baseName = opt.tileFilePrefix + std::to_string(artifact.nodeId);
        const std::filesystem::path tilesDir =
            std::filesystem::path(opt.tilesetOutDir) / opt.contentSubdir;

        std::filesystem::create_directories(tilesDir);

        const std::filesystem::path rawFullGlbPath = tilesDir / (baseName + "_raw.glb");
        const std::filesystem::path fullGlbPath = tilesDir / (baseName + ".glb");
        const std::filesystem::path fullB3dmPath = tilesDir / (baseName + ".b3dm");

        const bool okRaw = ExportTileToGlbFile(
            occurrences,
            node.items,
            rawFullGlbPath.string(),
            0.0f,
            opt.debugAppearance);

        if (!okRaw)
        {
            return false;
        }

        const NodeTuning tuning = BuildNodeTuning(
            artifact.bounds,
            rootBounds,
            depthFromRoot,
            maxDepth,
            true);

        const glbopt::Options leafGlbOptions = BuildLeafGlbOptOptionsForNode(tuning);
        glbopt::Stats leafGlbStats;
        const bool okOptimize =
            glbopt::OptimizeGlbFile(
                rawFullGlbPath.string(),
                fullGlbPath.string(),
                leafGlbOptions,
                leafGlbStats);

        if (!okOptimize)
        {
            return false;
        }

        const bool expectNonEmpty = node.totalTriangles > 0;
        if (!ValidateTileGeometryStats(leafGlbStats, expectNonEmpty, baseName))
        {
            return false;
        }

        if (opt.debugAppearance)
        {
            const std::size_t trisIn = leafGlbStats.InputPrimitiveElementCount / 3u;
            const std::size_t trisOut = leafGlbStats.OutputPrimitiveElementCount / 3u;

            std::cout << "[LeafGlbOpt] tile=" << baseName
                      << " depth=" << depthFromRoot
                      << " relSize=" << tuning.RelativeSize
                      << " weldPosStep=" << leafGlbOptions.PositionStep
                      << " simplifyRatio=" << leafGlbOptions.SimplifyRatio
                      << " simplifyError=" << leafGlbOptions.SimplifyError
                      << " vertsIn=" << leafGlbStats.InputVertexCount
                      << " vertsOut=" << leafGlbStats.OutputVertexCount
                      << " vertsMerged=" << leafGlbStats.MergedVertexCount
                      << " trisIn=" << trisIn
                      << " trisOut=" << trisOut
                      << " matsIn=" << leafGlbStats.MaterialCountInput
                      << " matsCanon=" << leafGlbStats.MaterialCountCanonical
                      << " matRemap=" << leafGlbStats.MaterialSlotsRemapped
                      << " primIn=" << leafGlbStats.PrimitiveCountExtracted
                      << " primOut=" << leafGlbStats.PrimitiveCountMergedOut
                      << "\n";
        }

        const bool okB3dm =
            B3dm::WrapGlbFileToB3dmFile(fullGlbPath.string(), fullB3dmPath.string());

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
        const TilesetEmit::Options& opt,
        BakedNodeArtifact& artifact,
        const BakeState& bakeState,
        int depthFromRoot,
        int maxDepth,
        const Bnd_Box& rootBounds)
    {
        std::vector<std::string> childGlbPaths;
        childGlbPaths.reserve(8);

        for (const auto& childPtr : node.children)
        {
            if (!childPtr)
            {
                continue;
            }

            const TileOctree::Node* child = childPtr.get();
            std::unordered_map<const TileOctree::Node*, BakedNodeArtifact>::const_iterator it =
                bakeState.artifacts.find(child);

            if (it == bakeState.artifacts.end())
            {
                continue;
            }

            if (!it->second.GlbPath.empty())
            {
                childGlbPaths.push_back(it->second.GlbPath.string());
            }
        }

        if (childGlbPaths.empty())
        {
            artifact.GlbPath.clear();
            artifact.ContentUri.clear();
            return true;
        }

        const std::string baseName = opt.tileFilePrefix + std::to_string(artifact.nodeId);
        const std::filesystem::path tilesDir =
            std::filesystem::path(opt.tilesetOutDir) / opt.contentSubdir;

        std::filesystem::create_directories(tilesDir);

        const std::filesystem::path proxyGlbPath = tilesDir / (baseName + "_proxy.glb");
        const std::filesystem::path proxyB3dmPath = tilesDir / (baseName + "_proxy.b3dm");

        const NodeTuning tuning = BuildNodeTuning(
            artifact.bounds,
            rootBounds,
            depthFromRoot,
            maxDepth,
            false);

        const glbopt::Options glbOptions =
            BuildProxyGlbOptOptionsForNode(tuning);

        glbopt::Stats proxyStats;

        const bool okProxyBake =
            glbopt::OptimizeGlbFiles(childGlbPaths, proxyGlbPath.string(), glbOptions, proxyStats);

        if (!okProxyBake)
        {
            return false;
        }

        if (opt.debugAppearance)
        {
            const std::size_t trisIn = proxyStats.InputPrimitiveElementCount / 3u;
            const std::size_t trisOut = proxyStats.OutputPrimitiveElementCount / 3u;

            std::cout << "[ProxyGlbOpt] tile=" << baseName
                      << " depth=" << depthFromRoot
                      << " relSize=" << tuning.RelativeSize
                      << " importance=" << tuning.Importance
                      << " weldPosStep=" << glbOptions.PositionStep
                      << " simplifyRatio=" << glbOptions.SimplifyRatio
                      << " simplifyError=" << glbOptions.SimplifyError
                      << " geomError=" << tuning.GeometricError
                      << " vertsIn=" << proxyStats.InputVertexCount
                      << " vertsOut=" << proxyStats.OutputVertexCount
                      << " trisIn=" << trisIn
                      << " trisOut=" << trisOut
                      << " primIn=" << proxyStats.PrimitiveCountExtracted
                      << " primOut=" << proxyStats.PrimitiveCountMergedOut
                      << "\n";
        }

        const bool expectNonEmpty = node.totalTriangles > 0;
        const bool geometryOk =
            ValidateTileGeometryStats(proxyStats, expectNonEmpty, baseName + "_proxy");
        const bool reductionOk =
            ValidateProxyReduction(proxyStats, expectNonEmpty, baseName + "_proxy");

        if (!geometryOk || !reductionOk)
        {
            return false;
        }

        const bool okProxyB3dm =
            B3dm::WrapGlbFileToB3dmFile(proxyGlbPath.string(), proxyB3dmPath.string());

        if (!okProxyB3dm)
        {
            return false;
        }

        artifact.GlbPath = proxyGlbPath;
        artifact.ContentUri = JoinUri(opt.contentSubdir, baseName + "_proxy.b3dm");

        if (!opt.keepGlbFilesForDebug)
        {
            std::error_code ec;
            std::filesystem::remove(proxyGlbPath, ec);
        }

        return true;
    }

    static bool BakeArtifactsBottomUp(
        const TileOctree::Node& node,
        const std::vector<Occurrence>& occurrences,
        const TilesetEmit::Options& opt,
        BakeState& bakeState,
        int depthFromRoot,
        int maxDepth,
        const Bnd_Box& rootBounds)
    {
        for (const auto& childPtr : node.children)
        {
            if (!childPtr)
            {
                continue;
            }

            if (!BakeArtifactsBottomUp(
                    *childPtr,
                    occurrences,
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
            const Bnd_Box tight = ComputeTightBounds(node, occurrences);
            if (!tight.IsVoid())
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
                occurrences,
                opt,
                artifact,
                depthFromRoot,
                maxDepth,
                rootBounds);
        }
        else
        {
            ok = BakeInternalProxyArtifact(
                node,
                opt,
                artifact,
                bakeState,
                depthFromRoot,
                maxDepth,
                rootBounds);
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
        const Bnd_Box& rootBounds,
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
                false);
            geometricError = tuning.GeometricError;
            if (!isRoot && parentGeometricError > 0.0)
            {
                const double maxAllowed = parentGeometricError * 0.7;
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
        const std::vector<Occurrence>& occurrences,
        const Options& opt)
    {
        std::filesystem::create_directories(opt.tilesetOutDir);

        const int maxDepth = ComputeMaxDepth(tree.Root());
        const Bnd_Box rootBounds = tree.Root().volume;

        BakeState bakeState;
        if (!BakeArtifactsBottomUp(
                tree.Root(),
                occurrences,
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
            false);
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
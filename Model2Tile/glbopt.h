#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace glbopt
{
    struct Options
    {
        bool DeduplicateMaterials = true;

        // Fidelity-first default: keep weld checks enabled for position/normal/uv/color.
        // OptimizeGlbFile(s) only forces WeldTexcoord0=false when neither materials use
        // textures nor meshes carry TEXCOORD_0 (welding positions without UV keys would merge seams).
        bool WeldPositions = true;
        bool WeldNormals = true;
        bool WeldTexcoord0 = true;
        bool WeldColor0 = true;
        bool DropAllBlackColor0 = false;
        bool StripColor0Always = false;
        bool ForceDefaultMaterialForMissing = true;
        bool ForceDoubleSidedMaterials = true;

        float PositionStep = 0.00001f;
        float NormalStep = 0.001f;
        float TexcoordStep = 0.0001f;
        float ColorStep = 0.00392156862f;

        bool RemoveDegenerateByIndex = true;
        bool RemoveDegenerateByArea = true;
        float DegenerateAreaEpsilonSq = 1e-20f;

        bool Simplify = false;
        float SimplifyRatio = 1.0f;
        float SimplifyError = 1e-2f;

        // Per-component weights for meshopt_simplifyWithAttributes (one float per attribute slot).
        float SimplifyNormalWeight = 0.5f;
        float SimplifyTexcoordWeight = 1.0f;
        float SimplifyColorWeight = 0.25f;

        bool OptimizeVertexCache = true;
        bool OptimizeOverdraw = false;
        float OverdrawThreshold = 1.05f;
        bool OptimizeVertexFetch = true;

        /// Hard cap on total triangles in the output GLB (all merged triangle primitives). Smallest-area
        /// triangles are dropped first (after weld + degenerate cull). `0` disables the cap.
        std::uint64_t MaxTriangles = 1000000;
    };

    struct Stats
    {
        std::size_t MeshCount = 0;
        std::size_t MaterialCountInput = 0;
        std::size_t MaterialCountCanonical = 0;
        std::size_t MaterialSlotsRemapped = 0;

        std::size_t PrimitiveCountSeen = 0;
        std::size_t PrimitiveCountExtracted = 0;
        std::size_t PrimitiveCountMergedOut = 0;

        std::size_t InputVertexCount = 0;
        std::size_t OutputVertexCount = 0;
        std::size_t MergedVertexCount = 0;

        std::size_t InputPrimitiveElementCount = 0;
        std::size_t OutputPrimitiveElementCount = 0;

        std::size_t DroppedDegenerateById = 0;
        std::size_t DroppedDegenerateByArea = 0;
        std::size_t DroppedTrianglesInvalidIndices = 0;
        std::size_t DroppedPrimitivesInvalidAccessor = 0;
        std::size_t DroppedPrimitivesInvalidIndices = 0;
        std::size_t DroppedIndicesInvalidRemap = 0;

        /// Triangles removed after weld + degenerate cull (before simplify), summed over triangle primitives.
        std::size_t TrianglesRemovedWeldPhase = 0;
        /// Triangles removed by mesh simplification, summed over triangle primitives.
        std::size_t TrianglesRemovedSimplify = 0;
        /// Triangles removed to satisfy `MaxTriangles` (smallest-area-first), summed over triangle primitives.
        std::size_t TrianglesRemovedMaxBudget = 0;
    };

    /// Always fills `outStats` (reset at entry). Callers that do not need metrics may pass a temporary.
    bool OptimizeGlbFile(
        const std::string& inputPath,
        const std::string& outputPath,
        const Options& options,
        Stats& outStats,
        const std::string& passTag);

    bool OptimizeGlbFiles(
        const std::vector<std::string>& inputPaths,
        const std::string& outputPath,
        const Options& options,
        Stats& outStats,
        const std::string& passTag);
}
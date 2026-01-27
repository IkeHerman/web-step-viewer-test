// glbopt.h
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace GlbOpt
{
    struct Options
    {
        // If > 0, the *final welded mesh* is simplified down to this many triangles (global budget).
        // If 0, no simplification is performed (but cache/overdraw/remap will still run).
        std::uint32_t maxTriangleCountTotal = 0;

        // Maximum error for meshopt_simplify (units are in POSITION space).
        // Typical starting points: 1e-3 .. 1e-1 depending on model scale.
        float simplifyMaxError = 1e-2f;

        // Overdraw optimization threshold (meshoptimizer default-ish is around 1.05f).
        float overdrawThreshold = 1.05f;

        // If true, primitives we can’t read (non-triangles, missing POSITION, etc.) are skipped.
        // If false, any unsupported primitive causes GlbOptimize() to return false.
        bool skipUnsupportedPrimitives = true;

        // Area threshold for degenerate triangle removal.
        // Triangles with area^2 <= this value are removed.
        float degenerateAreaEpsilon = 1e-6f;

        // Verbose debug logging to stdout.
        bool verbose = false;
    };

    // Optimize a GLB -> GLB.
    // Behavior:
    //  - Extract TRIANGLES primitives with float3 POSITION.
    //  - Keep only primitives whose attribute layout matches the first extracted primitive.
    //  - Weld everything into ONE mesh/ONE primitive (materials squashed).
    //  - Optionally simplify to Options::maxTriangleCountTotal.
    //  - Always run cache/overdraw optimization + vertex remap.
    //  - Rebuilds buffers/views/accessors so output can actually shrink.
    bool GlbOptimize(const std::filesystem::path& inGlbPath,
                     const std::filesystem::path& outGlbPath,
                     const Options& options);

    // Convenience overload: treat maxTriangleCount as a GLOBAL budget after welding.
    bool GlbOptimize(const std::filesystem::path& inGlbPath,
                     const std::filesystem::path& outGlbPath,
                     std::uint32_t maxTriangleCountTotal);

    // Generate multiple LOD GLBs using the provided global triangle budgets.
    // Example budgets: { 200000, 80000, 20000 } -> _LOD0/_LOD1/_LOD2
    bool GlbOptimizeLods(const std::filesystem::path& inGlbPath,
                         const std::filesystem::path& outDir,
                         const std::vector<std::uint32_t>& lodTriangleCounts,
                         const Options& baseOptions);
} // namespace GlbOpt

#pragma once

#include <algorithm>
#include <cmath>

namespace model2tile
{
/// Shared with `tileset_emit` / `step_glb_export`: clamp `v` to `[lo, hi]`.
inline double ClampDouble(double v, double lo, double hi)
{
    return std::max(lo, std::min(v, hi));
}

/// `nodeDiag / rootDiag` with root treated as at least 1e-9 (callers normally pass `max(1e-9, rootDiag)`).
inline double NodeToRootDiagonalRatio(double nodeDiag, double rootDiag)
{
    const double safeRoot = std::max(1e-9, rootDiag);
    return ClampDouble(nodeDiag / safeRoot, 1e-12, 1e6);
}

/// OCCT / tile pipeline SSE knob: treat targets below 1 like 1, then divide by 80.
inline double ViewerSseOver80(double viewerTargetSse)
{
    return std::max(1.0, viewerTargetSse) / 80.0;
}

/// Explicit clamp range (e.g. low-LOD export uses a wider hi bound than tile bake).
inline double ClampViewerSseScale(double viewerTargetSse, double clampLo, double clampHi)
{
    return ClampDouble(ViewerSseOver80(viewerTargetSse), clampLo, clampHi);
}

/// Same SSE scale as octree `BuildNodeTuning` geometric-error path: `max(1,sse)/80` in `[0.5, 2]`.
inline double ClampViewerSseScaleTileDefault(double viewerTargetSse)
{
    return ClampViewerSseScale(viewerTargetSse, 0.5, 2.0);
}

/// Octree-style geometric error: `nodeDiag * kFraction * sseScale`, clamped to min/max fractions of `nodeDiag`.
inline double ClampDiagonalGeometricError(
    double nodeDiag,
    double sseScale,
    double kGeomErrFraction,
    double kGeomErrMinFraction,
    double kGeomErrMaxFraction)
{
    const double safeNode = std::max(0.0, nodeDiag);
    const double minErr = safeNode * kGeomErrMinFraction;
    const double maxErr = safeNode * kGeomErrMaxFraction;
    const double rawErr = safeNode * kGeomErrFraction * sseScale;
    return ClampDouble(rawErr, minErr, std::max(minErr, maxErr));
}

/// Position weld step from node vs root AABB diagonals (glbopt `PositionStep` source).
inline double ComputeNodeBoxWeldStep(double nodeDiag, double rootDiag)
{
    const double safeNodeDiag = std::max(0.0, nodeDiag);
    const double rootStepMin = std::max(1e-12, rootDiag * 1e-9);

    // Coarser than original 2e-4; `PositionStep` scales with node diagonal until `stepMax`.
    constexpr double kWeldFractionOfNodeDiag = 8e-4;

    const double stepMin = std::max(rootStepMin, safeNodeDiag * 1e-9);
    // Upper clamp on position weld step: 10× prior `2e-3 * diag` cap (was 0.2% of diagonal, now 2%).
    const double stepMax = std::max(stepMin, safeNodeDiag * 2e-2);

    return ClampDouble(
        safeNodeDiag * kWeldFractionOfNodeDiag,
        stepMin,
        stepMax);
}

} // namespace model2tile

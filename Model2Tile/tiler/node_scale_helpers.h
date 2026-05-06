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

/// Fixed world-space weld grid for glbopt vertex quantization (does **not** scale with octree volume).
/// Conservative: intended only to merge corners separated by float precision / export noise, not by LOD scale.
inline constexpr double kVertexWeldPositionEpsilonWorld = 1e-6;

inline double GlobalVertexWeldPositionStep()
{
    return kVertexWeldPositionEpsilonWorld;
}

/// Normal component weld step derived from positional weld: calibrated so ~1µm positional step ⇒ ~\(10^{-5}\)
/// normal step (prior fixed default); scales up on large cells, clamped.
inline double ComputeWeldNormalStepFromPositionStep(double positionStep)
{
    constexpr double kRefPos = 1e-6;
    constexpr double kRefNorm = 1e-5;
    constexpr double kMin = 8e-7;
    constexpr double kMax = 8e-3;
    return ClampDouble(kRefNorm * (positionStep / kRefPos), kMin, kMax);
}

/// UV weld step derived from positional weld the same way (texture space is unitless; caps stay conservative).
inline double ComputeWeldTexcoordStepFromPositionStep(double positionStep)
{
    constexpr double kRefPos = 1e-6;
    constexpr double kRefTc = 1e-7;
    constexpr double kMin = 1e-9;
    constexpr double kMax = 3e-4;
    return ClampDouble(kRefTc * (positionStep / kRefPos), kMin, kMax);
}

} // namespace model2tile

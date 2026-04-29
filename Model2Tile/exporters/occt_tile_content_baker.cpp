#include "occt_tile_content_baker.h"

#include "../b3dm.h"
#include "../export_glb.h"
#include "../glbopt.h"

namespace exporters
{
OcctTileContentBaker::OcctTileContentBaker(const std::vector<Occurrence>& occurrences)
    : m_occurrences(occurrences)
{
}

TileBakeResult OcctTileContentBaker::Bake(const TileBakeRequest& request)
{
    TileBakeResult result;
    result.glbPath = request.outputBasename + ".glb";
    result.b3dmPath = request.outputBasename + ".b3dm";

    const bool exported = ExportTileToGlbFile(
        m_occurrences,
        request.itemIndices,
        result.glbPath,
        0.0f,
        request.debugAppearance,
        request.nodeBoundsDiagonal);
    if (!exported)
    {
        result.error = "failed to export GLB";
        return result;
    }

    glbopt::Stats stats;
    const bool optimized = glbopt::OptimizeGlbFile(
        result.glbPath,
        result.glbPath,
        glbopt::Options{},
        stats);
    if (!optimized)
    {
        result.error = "failed to optimize GLB";
        return result;
    }

    const bool wrapped = B3dm::WrapGlbFileToB3dmFile(result.glbPath, result.b3dmPath);
    if (!wrapped)
    {
        result.error = "failed to wrap B3DM";
        return result;
    }

    result.ok = true;
    return result;
}
} // namespace exporters

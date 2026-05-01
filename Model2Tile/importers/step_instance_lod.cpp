#include "step_instance_lod.h"

#include "step_glb_export.h"

#include <cmath>
#include <filesystem>
#include <iostream>

namespace
{
static double DiagonalLength(const Bnd_Box& b)
{
    if (b.IsVoid())
    {
        return 0.0;
    }

    double xmin, ymin, zmin, xmax, ymax, zmax;
    b.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    const double dx = xmax - xmin;
    const double dy = ymax - ymin;
    const double dz = zmax - zmin;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
}

namespace importers
{
bool BakeStepInstanceLods(
    const std::vector<Occurrence>& occurrences,
    const Bnd_Box& rootBounds,
    const double viewerTargetSse,
    const std::string& outputDirectory,
    const std::string& outputUriPrefix,
    const bool debugAppearance,
    std::vector<std::string>& outHighGlbUris)
{
    std::error_code ec;
    std::filesystem::create_directories(outputDirectory, ec);
    if (ec)
    {
        std::cerr << "[StepInstanceLod] failed to create directory: " << outputDirectory << "\n";
        return false;
    }

    const double rootDiag = std::max(1e-9, DiagonalLength(rootBounds));

    outHighGlbUris.assign(occurrences.size(), std::string());

    for (std::size_t i = 0; i < occurrences.size(); ++i)
    {
        const Occurrence& occ = occurrences[i];
        if (!occ.Appearance)
        {
            continue;
        }

        const std::filesystem::path base =
            std::filesystem::path(outputDirectory) /
            ("occ_" + std::to_string(i));

        const std::string highFileName = "occ_" + std::to_string(i) + "_high.glb";
        const std::string highPath = (base.string() + "_high.glb");

        const double occDiag =
            occ.WorldBounds.IsVoid() ? rootDiag : std::max(1e-9, DiagonalLength(occ.WorldBounds));

        const ExportTessellationPolicy highPol =
            MakeInstanceHighTessellationPolicy(viewerTargetSse, occDiag);

        const std::vector<std::uint32_t> oneIndex = { static_cast<std::uint32_t>(i) };

        if (!ExportTileToGlbFile(
                occurrences,
                oneIndex,
                highPath,
                debugAppearance,
                highPol))
        {
            std::cerr << "[StepInstanceLod] failed high export index=" << i << "\n";
            return false;
        }

        outHighGlbUris[i] = outputUriPrefix + "/" + highFileName;
    }

    std::cout << "[StepInstanceLod] baked high GLBs for "
              << occurrences.size() << " occurrences under " << outputDirectory << "\n";

    return true;
}
} // namespace importers

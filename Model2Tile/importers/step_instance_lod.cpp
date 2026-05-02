#include "step_instance_lod.h"

#include "step_glb_export.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <unordered_map>
#include <vector>

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
    std::unordered_map<std::string, std::string>& outPrototypeHighLodUrisByQualifiedKey)
{
    std::error_code ec;
    std::filesystem::create_directories(outputDirectory, ec);
    if (ec)
    {
        std::cerr << "[StepInstanceLod] failed to create directory: " << outputDirectory << "\n";
        return false;
    }

    const double rootDiag = std::max(1e-9, DiagonalLength(rootBounds));

    outPrototypeHighLodUrisByQualifiedKey.clear();

    // Match Scene IR prototype order: first encounter of each QualifiedPrototypeKey in occurrence order.
    std::vector<std::string> prototypeKeyOrder;
    std::unordered_map<std::string, std::vector<std::uint32_t>> indicesByKey;

    for (std::size_t i = 0; i < occurrences.size(); ++i)
    {
        const Occurrence& occ = occurrences[i];
        const std::string& k = occ.QualifiedPrototypeKey;
        auto it = indicesByKey.find(k);
        if (it == indicesByKey.end())
        {
            prototypeKeyOrder.push_back(k);
            indicesByKey.emplace(k, std::vector<std::uint32_t>{});
            it = indicesByKey.find(k);
        }
        if (occ.Appearance)
        {
            it->second.push_back(static_cast<std::uint32_t>(i));
        }
    }

    std::size_t nextProtoFileIndex = 0;

    for (const std::string& key : prototypeKeyOrder)
    {
        const std::vector<std::uint32_t>& idxs = indicesByKey[key];
        if (idxs.empty())
        {
            continue;
        }

        double maxDiag = rootDiag;
        for (const std::uint32_t ix : idxs)
        {
            const Occurrence& o = occurrences[static_cast<std::size_t>(ix)];
            const double d =
                o.WorldBounds.IsVoid() ? rootDiag : std::max(1e-9, DiagonalLength(o.WorldBounds));
            maxDiag = std::max(maxDiag, d);
        }

        const ExportTessellationPolicy highPol =
            MakeInstanceHighTessellationPolicy(viewerTargetSse, maxDiag);

        const std::string stem = "proto_" + std::to_string(nextProtoFileIndex++);
        const std::filesystem::path highPath =
            std::filesystem::path(outputDirectory) / (stem + "_high.glb");

        const std::vector<std::uint32_t> oneIndex = { idxs.front() };

        if (!ExportTileToGlbFile(
                occurrences,
                oneIndex,
                highPath.string(),
                highPol,
                false))
        {
            std::cerr << "[StepInstanceLod] failed high export prototypeKey=\"" << key << "\"\n";
            return false;
        }

        const std::string uri = outputUriPrefix + "/" + stem + "_high.glb";
        outPrototypeHighLodUrisByQualifiedKey[key] = uri;
    }

    std::cout << "[StepInstanceLod] baked high GLBs for "
              << outPrototypeHighLodUrisByQualifiedKey.size() << " prototype keys ("
              << nextProtoFileIndex << " files on disk) under " << outputDirectory << "\n";

    return true;
}
} // namespace importers

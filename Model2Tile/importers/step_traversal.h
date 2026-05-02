#pragma once

#include "step_occurrence.h"

#include <Bnd_Box.hxx>
#include <TopAbs_ShapeEnum.hxx>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace importers
{
struct NonRenderableLeafInfo
{
    std::string Label;
    std::string Name;
    TopAbs_ShapeEnum Type = TopAbs_SHAPE;
    int ShellCount = 0;
    int FaceCount = 0;
};

bool CollectStepOccurrencesFromFiles(
    const std::vector<std::filesystem::path>& stepFiles,
    bool occtVerbose,
    std::vector<Occurrence>& occurrences,
    Bnd_Box& globalBounds,
    std::size_t& traversedLeafLabels,
    std::size_t& labelsWithNoRenderableGeometry,
    std::size_t& labelsWithShellFallback,
    std::size_t& nonAssemblyLabelsWithComponents,
    std::vector<NonRenderableLeafInfo>& nonRenderableLeaves,
    std::uint64_t& totalTriangles,
    std::size_t& totalRoots);
} // namespace importers

#pragma once

#include "fbx_occurrence.h"

#include <filesystem>
#include <vector>

namespace importers
{
bool CollectFbxOccurrences(
    const std::filesystem::path& filePath,
    std::vector<FbxOccurrence>& outOccurrences,
    core::Aabb& outWorldBounds,
    bool verbose);
} // namespace importers

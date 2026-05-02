#pragma once

#include "fbx_occurrence.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace importers
{
bool CollectFbxOccurrences(
    const std::filesystem::path& filePath,
    std::vector<FbxOccurrence>& outOccurrences,
    core::Aabb& outWorldBounds,
    bool verbose);

bool CollectFbxOccurrencesAndBakeLods(
    const std::filesystem::path& filePath,
    const std::string& outputDirectory,
    const std::string& outputUriPrefix,
    std::vector<FbxOccurrence>& outOccurrences,
    std::unordered_map<std::string, std::string>& outPrototypeHighLodUrisByQualifiedKey,
    core::Aabb& outWorldBounds,
    bool verbose);
} // namespace importers

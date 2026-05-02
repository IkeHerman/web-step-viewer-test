#pragma once

#include "../core/scene_ir.h"
#include "../importers/fbx_occurrence.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace adapters
{
core::SceneIR BuildSceneIRFromFbxOccurrences(
    const std::string& sourcePath,
    const std::vector<importers::FbxOccurrence>& occurrences,
    const core::Aabb& globalBounds,
    const std::unordered_map<std::string, std::string>* prototypeHighLodUrisByQualifiedKey,
    const std::vector<std::string>* lowLodGlbUris);
} // namespace adapters

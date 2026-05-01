#pragma once

#include "../core/scene_ir.h"
#include "../importers/fbx_occurrence.h"

#include <string>
#include <vector>

namespace adapters
{
core::SceneIR BuildSceneIRFromFbxOccurrences(
    const std::string& sourcePath,
    const std::vector<importers::FbxOccurrence>& occurrences,
    const core::Aabb& globalBounds,
    const std::vector<std::string>* highLodGlbUris,
    const std::vector<std::string>* lowLodGlbUris);
} // namespace adapters

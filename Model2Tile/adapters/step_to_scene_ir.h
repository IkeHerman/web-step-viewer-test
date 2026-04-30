#pragma once

#include "../importers/step_occurrence.h"
#include "../core/scene_ir.h"

#include <string>
#include <vector>

namespace adapters
{
core::SceneIR BuildSceneIRFromStepOccurrences(
    const std::string& sourcePath,
    const std::vector<Occurrence>& occurrences,
    const core::Aabb& globalBounds,
    const std::vector<std::string>* highLodGlbUris = nullptr,
    const std::vector<std::string>* lowLodGlbUris = nullptr);
}

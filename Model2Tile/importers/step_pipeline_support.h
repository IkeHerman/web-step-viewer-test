#pragma once

#include "../app/cli_options.h"
#include "step_occurrence.h"
#include "../core/scene_ir.h"

#include <filesystem>
#include <vector>

namespace importers
{
bool ValidateSceneIrInstanceIds(const core::SceneIR& sceneIr, bool verbose, bool requireLodUris);

std::vector<std::filesystem::path> CollectStepFiles(const std::filesystem::path& input);

void WriteStepFidelityArtifacts(
    const std::filesystem::path& artifactDir,
    const std::vector<Occurrence>& occurrences,
    const core::SceneIR& sceneIr);
} // namespace importers

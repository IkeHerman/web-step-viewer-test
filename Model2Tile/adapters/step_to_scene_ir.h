#pragma once

#include "../common.h"
#include "../core/scene_ir.h"

#include <Bnd_Box.hxx>

#include <string>
#include <vector>

namespace adapters
{
core::SceneIR BuildSceneIRFromStepOccurrences(
    const std::string& sourcePath,
    const std::vector<Occurrence>& occurrences,
    const Bnd_Box& globalBounds);
}

#pragma once

#include <Bnd_Box.hxx>

#include <string>
#include <unordered_map>
#include <vector>

#include "step_occurrence.h"

namespace importers
{
/// Builds tessellated high GLBs keyed by coarse `Occurrence::QualifiedPrototypeKey` (geometry + material).
/// One exported mesh per distinct qualified key; bake reference is the first occurrence with that key.
bool BakeStepInstanceLods(
    const std::vector<Occurrence>& occurrences,
    const Bnd_Box& rootBounds,
    double viewerTargetSse,
    const std::string& outputDirectory,
    const std::string& outputUriPrefix,
    std::unordered_map<std::string, std::string>& outPrototypeHighLodUrisByQualifiedKey);
} // namespace importers

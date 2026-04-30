#pragma once

#include <Bnd_Box.hxx>

#include <string>
#include <vector>

#include "step_occurrence.h"

namespace importers
{
// STEP-specific pre-bake:
// Builds high/low GLBs for each STEP occurrence and returns URIs parallel to
// occurrence order.
bool BakeStepInstanceLods(
    const std::vector<Occurrence>& occurrences,
    const Bnd_Box& rootBounds,
    double viewerTargetSse,
    const std::string& outputDirectory,
    const std::string& outputUriPrefix,
    bool debugAppearance,
    std::vector<std::string>& outHighGlbUris,
    std::vector<std::string>& outLowGlbUris);
} // namespace importers

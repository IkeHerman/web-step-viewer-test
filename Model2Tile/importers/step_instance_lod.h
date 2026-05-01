#pragma once

#include <Bnd_Box.hxx>

#include <string>
#include <vector>

#include "step_occurrence.h"

namespace importers
{
/// Builds one tessellated high GLB per STEP occurrence (no separate low LOD).
bool BakeStepInstanceLods(
    const std::vector<Occurrence>& occurrences,
    const Bnd_Box& rootBounds,
    double viewerTargetSse,
    const std::string& outputDirectory,
    const std::string& outputUriPrefix,
    bool debugAppearance,
    std::vector<std::string>& outHighGlbUris);
} // namespace importers

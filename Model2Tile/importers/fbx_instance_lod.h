#pragma once

#include "fbx_occurrence.h"

#include <string>
#include <vector>

namespace importers
{
/// Bakes one high-detail GLB per occurrence (no separate low LOD).
bool BakeFbxInstanceLods(
    const std::vector<FbxOccurrence>& occurrences,
    const std::string& outputDirectory,
    const std::string& outputUriPrefix,
    std::vector<std::string>& outHighGlbUris);
} // namespace importers

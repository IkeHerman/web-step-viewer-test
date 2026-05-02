#pragma once

#include "fbx_occurrence.h"

#include <string>
#include <vector>

namespace importers
{
/// Writes one occurrence to a GLB on disk (world transform on root node).
bool WriteFbxOccurrenceHighGlb(
    const FbxOccurrence& occurrence,
    const std::string& glbPath,
    bool verbose);

/// Writes mesh in local space with identity root transform (prototype asset).
bool WriteFbxOccurrenceHighGlbLocalIdentity(
    const FbxOccurrence& occurrence,
    const std::string& glbPath,
    bool verbose);

/// Bakes one high-detail GLB per occurrence (no separate low LOD).
bool BakeFbxInstanceLods(
    const std::vector<FbxOccurrence>& occurrences,
    const std::string& outputDirectory,
    const std::string& outputUriPrefix,
    std::vector<std::string>& outHighGlbUris);
} // namespace importers

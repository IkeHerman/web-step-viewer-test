// b3dm.h
#pragma once

#include <map>
#include <string>

namespace B3dm
{
    /// Stream GLB from disk into a minimal B3DM (3D Tiles 1.0) without loading the full GLB twice.
    bool WrapGlbFileToB3dmFile(const std::string& glbPath, const std::string& b3dmPath);
    bool WrapGlbFileToB3dmFile(
        const std::string& glbPath,
        const std::string& b3dmPath,
        const std::map<std::string, std::string>& metadata);
}

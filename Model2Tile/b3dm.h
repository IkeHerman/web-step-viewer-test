// b3dm.h
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace B3dm
{
    /// Wrap raw GLB bytes into a minimal B3DM payload (3D Tiles 1.0).
    /// Produces empty feature table and batch table JSON ("{}") padded to 8-byte boundaries.
    std::vector<std::uint8_t> WrapGlbBytes(const std::vector<std::uint8_t>& glbBytes);

    /// Read all bytes from a file. Returns empty vector on failure.
    std::vector<std::uint8_t> ReadFileBytes(const std::string& path);

    /// Write bytes to a file (binary). Returns true on success.
    bool WriteBytesToFile(const std::string& path, const std::vector<std::uint8_t>& bytes);

    /// Convenience: read GLB from glbPath, wrap into B3DM, write to b3dmPath.
    bool WrapGlbFileToB3dmFile(const std::string& glbPath, const std::string& b3dmPath);
    bool WrapGlbFileToB3dmFile(
        const std::string& glbPath,
        const std::string& b3dmPath,
        const std::map<std::string, std::string>& metadata);
}

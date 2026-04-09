#pragma once

#include "common.h"

#include <cstdint>
#include <string>
#include <vector>

#include <TDocStd_Document.hxx>

Handle(TDocStd_Document) CreateEmptyXcafDocument();

bool ExportTileToGlbFile(
    const std::vector<Occurrence>& occurrences,
    const std::vector<std::uint32_t>& itemIndices,
    const std::string& glbPath,
    const float decimationFactor,
    const bool debugAppearance,
    const double nodeBoundsDiagonal = -1.0
    );

bool ExportBoxToGlbFile(
    const Bnd_Box& bounds,
    const std::string& glbPath);

std::vector<std::uint8_t> ReadFileBytes(const std::string& path);

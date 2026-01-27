#pragma once

#include "common.h"

#include <cstdint>
#include <string>
#include <vector>

#include <TDocStd_Document.hxx>
#include <XCAFDoc_ShapeTool.hxx>

Handle(TDocStd_Document) CreateEmptyXcafDocument();

bool ExportTileToGlbFile(
    const Handle(XCAFDoc_ShapeTool)& sourceShapeTool,
    const std::vector<Occurrence>& occurrences,
    const std::vector<std::uint32_t>& itemIndices,
    const std::string& glbPath,
    const float decimationFactor
    );

bool ExportBoxToGlbFile(
    const Bnd_Box& bounds,
    const std::string& glbPath);

std::vector<std::uint8_t> ReadFileBytes(const std::string& path);

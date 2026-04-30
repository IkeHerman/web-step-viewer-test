#pragma once

#include "step_occurrence.h"

#include <cstdint>
#include <string>
#include <vector>

#include <TDocStd_Document.hxx>

Handle(TDocStd_Document) CreateEmptyXcafDocument();
void ConfigureFidelityArtifactOutput(const std::string& directoryPath);

enum class ExportTileClass
{
    Leaf,
    Proxy
};

struct ExportTessellationPolicy
{
    double viewerTargetSse = 80.0;
    double tileGeometricError = 0.0;
    double nodeBoundsDiagonal = -1.0;
    double linearMinFraction = 5e-4;
    double linearMaxFraction = 2e-1;
    double angularMinDeg = 0.2;
    double angularMaxDeg = 3.0;
    double qualityBias = 1.0;
    ExportTileClass tileClass = ExportTileClass::Leaf;
};

struct ExportResolvedTessellation
{
    double chosenSse = 80.0;
    double linearDeflection = 0.05;
    double angularDeflectionDeg = 0.75;
};

ExportResolvedTessellation ResolveExportTessellation(
    const ExportTessellationPolicy& policy);

ExportTessellationPolicy MakeInstanceHighTessellationPolicy(
    double viewerTargetSse,
    double occurrenceBoundsDiagonal);

ExportTessellationPolicy MakeInstanceLowTessellationPolicy(
    double viewerTargetSse,
    double occurrenceBoundsDiagonal);

bool ExportTileToGlbFile(
    const std::vector<Occurrence>& occurrences,
    const std::vector<std::uint32_t>& itemIndices,
    const std::string& glbPath,
    const bool debugAppearance,
    const ExportTessellationPolicy& tessellationPolicy
    );

bool ExportBoxToGlbFile(
    const Bnd_Box& bounds,
    const std::string& glbPath);

std::vector<std::uint8_t> ReadFileBytes(const std::string& path);

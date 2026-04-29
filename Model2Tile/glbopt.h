#pragma once

#include <string>
#include <vector>

namespace glbopt
{
    void SetVerboseLogging(bool enabled);
    bool IsVerboseLogging();

    struct Options
    {
        bool DeduplicateMaterials = true;

        bool WeldPositions = true;
        bool WeldNormals = true;
        bool WeldTexcoord0 = true;
        bool WeldColor0 = true;
        bool DropAllBlackColor0 = false;
        bool StripColor0Always = false;
        bool ForceDefaultMaterialForMissing = true;
        bool ForceDoubleSidedMaterials = true;

        float PositionStep = 0.00001f;
        float NormalStep = 0.001f;
        float TexcoordStep = 0.0001f;
        float ColorStep = 0.00392156862f;

        bool RemoveDegenerateByIndex = true;
        bool RemoveDegenerateByArea = true;
        float DegenerateAreaEpsilonSq = 1e-20f;

        bool Simplify = false;
        float SimplifyRatio = 1.0f;
        float SimplifyError = 1e-2f;

        bool OptimizeVertexCache = true;
        bool OptimizeOverdraw = false;
        float OverdrawThreshold = 1.05f;
        bool OptimizeVertexFetch = true;
    };

    struct Stats
    {
        std::size_t MeshCount = 0;
        std::size_t MaterialCountInput = 0;
        std::size_t MaterialCountCanonical = 0;
        std::size_t MaterialSlotsRemapped = 0;

        std::size_t PrimitiveCountSeen = 0;
        std::size_t PrimitiveCountExtracted = 0;
        std::size_t PrimitiveCountMergedOut = 0;

        std::size_t InputVertexCount = 0;
        std::size_t OutputVertexCount = 0;
        std::size_t MergedVertexCount = 0;

        std::size_t InputPrimitiveElementCount = 0;
        std::size_t OutputPrimitiveElementCount = 0;

        std::size_t DroppedDegenerateById = 0;
        std::size_t DroppedDegenerateByArea = 0;
        std::size_t SimplifiedIndexCount = 0;
    };

    bool OptimizeGlbFile(
        const std::string& inputPath,
        const std::string& outputPath,
        const Options& options,
        Stats& outStats);

    bool OptimizeGlbFile(
        const std::string& inputPath,
        const std::string& outputPath,
        const Options& options);

    bool OptimizeGlbFile(
        const std::string& inputPath,
        const std::string& outputPath);

    bool OptimizeGlbFiles(
        const std::vector<std::string>& inputPaths,
        const std::string& outputPath,
        const Options& options,
        Stats& outStats);

    bool OptimizeGlbFiles(
        const std::vector<std::string>& inputPaths,
        const std::string& outputPath,
        const Options& options);

    bool OptimizeGlbFiles(
        const std::vector<std::string>& inputPaths,
        const std::string& outputPath);
}
#pragma once

#include <cstdint>
#include <filesystem>

struct CliOptions
{
    std::filesystem::path inputPath;
    std::filesystem::path outDir = "../Tile-Viewer/public";
    std::string contentSubdir = "tiles";
    std::string tilePrefix = "tile_";
    std::string inputFormat = "auto";
    std::filesystem::path fidelityArtifactsDir;
    double viewerTargetSse = 80.0;
    bool keepGlb = true;
    bool verbose = false;

    double instanceMinSizeRatio = 1e-3;
    /// Upper bound on proxy merge triangle count vs 50% leaf-high budget (smaller wins).
    std::uint64_t proxyMergeMaxTrianglesHardCap = 2000000ULL;
    /// Apply the 50% leaf-high triangle budget only when summed subtree leaf-high tris exceed this.
    std::uint64_t proxyMergeRatioMinLeafHighTris = 50000ULL;
};

void PrintUsage(const char* programName);
bool ParseCli(int argc, char** argv, CliOptions& out, int& outExitCode);

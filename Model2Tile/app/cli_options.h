#pragma once

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
};

void PrintUsage(const char* programName);
bool ParseCli(int argc, char** argv, CliOptions& out, int& outExitCode);

#pragma once

#include <filesystem>

struct CliOptions
{
    std::filesystem::path inputPath;
    std::filesystem::path outDir = "../Tile-Viewer/public";
    std::string contentSubdir = "tiles";
    std::string tilePrefix = "tile_";
    std::string inputFormat = "auto";
    bool keepGlb = true;
    bool useTightBounds = true;
    bool contentOnlyAtLeaves = false;
    bool verbose = false;
};

void PrintUsage(const char* programName);
bool ParseCli(int argc, char** argv, CliOptions& out, int& outExitCode);

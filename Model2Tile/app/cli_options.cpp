#include "cli_options.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

void PrintUsage(const char* programName)
{
    const std::string exe = (programName && *programName) ? programName : "model2tile";

    std::cout
        << "Usage:\n"
        << "  " << exe << " [options] <input_model|input_directory>\n\n"
        << "Options:\n"
        << "  -o, --out-dir <path>         Output directory for tileset.json and tile content\n"
        << "                               (default: ../Tile-Viewer/public)\n"
        << "      --content-subdir <name>  Subdirectory under out-dir for tile content\n"
        << "                               (default: tiles)\n"
        << "      --tile-prefix <prefix>   Tile filename prefix (default: tile_)\n"
        << "      --input-format <fmt>     Input format: auto|step|fbx (default: auto)\n"
        << "      --fidelity-artifacts-dir <path>\n"
        << "                               Emit importer fidelity evidence artifacts to directory\n"
        << "      --viewer-target-sse <num> Viewer-aligned target SSE for export tessellation\n"
        << "                               (default: 80)\n"
        << "      --keep-glb               Keep intermediate .glb files\n"
        << "      --discard-glb            Delete intermediate .glb files after wrap\n"
        << "      --instance-min-size-ratio <x>\n"
        << "                               Min occurrenceDiag/tileDiag to include (default 1e-3; 0=no cull)\n"
        << "      --proxy-merge-max-tris <n>\n"
        << "                               Hard cap on proxy merge MaxTriangles vs 50% leaf-high (default 2M)\n"
        << "      --proxy-merge-ratio-min-leaf-high-tris <n>\n"
        << "                               Only apply the 50%% leaf-high ratio budget above this triangle sum\n"
        << "                               (default 50000; 0 = always apply when leaf-high sum > 0)\n"
        << "  -v, --verbose                Open CASCADE STEP transfer checks (IFSelect::PrintCheckTransfer)\n"
        << "  -h, --help                   Show this help\n";
}

bool ParseCli(int argc, char** argv, CliOptions& out, int& outExitCode)
{
    outExitCode = 0;

    bool hasInputPath = false;

    auto requireValue = [&](int& index, const std::string& optName, std::string& outValue) -> bool
    {
        if (index + 1 >= argc)
        {
            std::cerr << "Missing value for option: " << optName << "\n";
            return false;
        }

        ++index;
        outValue = argv[index];
        return true;
    };

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];

        if (arg == "-h" || arg == "--help")
        {
            PrintUsage(argv[0]);
            outExitCode = 0;
            return false;
        }
        else if (arg == "-o" || arg == "--out-dir")
        {
            std::string value;
            if (!requireValue(i, arg, value))
            {
                outExitCode = 2;
                return false;
            }
            out.outDir = value;
        }
        else if (arg == "--content-subdir")
        {
            std::string value;
            if (!requireValue(i, arg, value))
            {
                outExitCode = 2;
                return false;
            }
            out.contentSubdir = value;
        }
        else if (arg == "--tile-prefix")
        {
            std::string value;
            if (!requireValue(i, arg, value))
            {
                outExitCode = 2;
                return false;
            }
            out.tilePrefix = value;
        }
        else if (arg == "--input-format")
        {
            std::string value;
            if (!requireValue(i, arg, value))
            {
                outExitCode = 2;
                return false;
            }
            std::transform(value.begin(), value.end(), value.begin(), ::tolower);
            out.inputFormat = value;
        }
        else if (arg == "--fidelity-artifacts-dir")
        {
            std::string value;
            if (!requireValue(i, arg, value))
            {
                outExitCode = 2;
                return false;
            }
            out.fidelityArtifactsDir = value;
        }
        else if (arg == "--viewer-target-sse")
        {
            std::string value;
            if (!requireValue(i, arg, value))
            {
                outExitCode = 2;
                return false;
            }
            out.viewerTargetSse = std::stod(value);
        }
        else if (arg == "--keep-glb")
        {
            out.keepGlb = true;
        }
        else if (arg == "--discard-glb")
        {
            out.keepGlb = false;
        }
        else if (arg == "-v" || arg == "--verbose")
        {
            out.verbose = true;
        }
        else if (arg == "--instance-min-size-ratio")
        {
            std::string value;
            if (!requireValue(i, arg, value))
            {
                outExitCode = 2;
                return false;
            }
            out.instanceMinSizeRatio = std::stod(value);
        }
        else if (arg == "--proxy-merge-max-tris")
        {
            std::string value;
            if (!requireValue(i, arg, value))
            {
                outExitCode = 2;
                return false;
            }
            try
            {
                const unsigned long long v = std::stoull(value);
                if (v == 0ULL)
                {
                    std::cerr << "--proxy-merge-max-tris must be > 0.\n";
                    outExitCode = 2;
                    return false;
                }
                out.proxyMergeMaxTrianglesHardCap = static_cast<std::uint64_t>(v);
            }
            catch (const std::exception&)
            {
                std::cerr << "--proxy-merge-max-tris must be a positive integer.\n";
                outExitCode = 2;
                return false;
            }
        }
        else if (arg == "--proxy-merge-ratio-min-leaf-high-tris")
        {
            std::string value;
            if (!requireValue(i, arg, value))
            {
                outExitCode = 2;
                return false;
            }
            try
            {
                out.proxyMergeRatioMinLeafHighTris = std::stoull(value);
            }
            catch (const std::exception&)
            {
                std::cerr << "--proxy-merge-ratio-min-leaf-high-tris must be an integer.\n";
                outExitCode = 2;
                return false;
            }
        }
        else if (!arg.empty() && arg[0] == '-')
        {
            std::cerr << "Unknown option: " << arg << "\n";
            PrintUsage(argv[0]);
            outExitCode = 2;
            return false;
        }
        else
        {
            if (hasInputPath)
            {
                std::cerr << "Multiple input paths provided. Only one input path is allowed.\n";
                outExitCode = 2;
                return false;
            }

            out.inputPath = arg;
            hasInputPath = true;
        }
    }

    if (!hasInputPath)
    {
        std::cerr << "Missing input path.\n";
        PrintUsage(argv[0]);
        outExitCode = 2;
        return false;
    }

    if (out.contentSubdir.empty())
    {
        std::cerr << "--content-subdir must not be empty.\n";
        outExitCode = 2;
        return false;
    }

    if (out.tilePrefix.empty())
    {
        std::cerr << "--tile-prefix must not be empty.\n";
        outExitCode = 2;
        return false;
    }

    if (out.inputFormat != "auto" && out.inputFormat != "step" && out.inputFormat != "fbx")
    {
        std::cerr << "--input-format must be one of: auto, step, fbx.\n";
        outExitCode = 2;
        return false;
    }

    if (!(out.viewerTargetSse > 0.0))
    {
        std::cerr << "--viewer-target-sse must be > 0.\n";
        outExitCode = 2;
        return false;
    }

    if (out.instanceMinSizeRatio < 0.0 || !std::isfinite(out.instanceMinSizeRatio))
    {
        std::cerr << "--instance-min-size-ratio must be >= 0 and finite.\n";
        outExitCode = 2;
        return false;
    }

    return true;
}

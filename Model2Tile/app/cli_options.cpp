#include "cli_options.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>

void PrintUsage(const char* programName)
{
    const std::string exe = (programName && *programName) ? programName : "model2tile";

    std::cout
        << "Usage:\n"
        << "  " << exe << " [options] <input.step|input_directory>\n\n"
        << "Options:\n"
        << "  -o, --out-dir <path>         Output directory for tileset.json and tile content\n"
        << "                               (default: ../Tile-Viewer/public)\n"
        << "      --content-subdir <name>  Subdirectory under out-dir for tile content\n"
        << "                               (default: tiles)\n"
        << "      --tile-prefix <prefix>   Tile filename prefix (default: tile_)\n"
        << "      --input-format <fmt>     Input format: auto|step|fbx (default: auto)\n"
        << "      --keep-glb               Keep intermediate .glb files\n"
        << "      --discard-glb            Delete intermediate .glb files after wrap\n"
        << "      --tight-bounds           Enable tight tile bounds (default)\n"
        << "      --no-tight-bounds        Disable tight tile bounds\n"
        << "      --content-only-leaves    Emit content only at leaves\n"
        << "      --content-all-levels     Allow content at internal levels (default)\n"
        << "  -v, --verbose                Enable verbose debug output\n"
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
        else if (arg == "--keep-glb")
        {
            out.keepGlb = true;
        }
        else if (arg == "--discard-glb")
        {
            out.keepGlb = false;
        }
        else if (arg == "--tight-bounds")
        {
            out.useTightBounds = true;
        }
        else if (arg == "--no-tight-bounds")
        {
            out.useTightBounds = false;
        }
        else if (arg == "--content-only-leaves")
        {
            out.contentOnlyAtLeaves = true;
        }
        else if (arg == "--content-all-levels")
        {
            out.contentOnlyAtLeaves = false;
        }
        else if (arg == "-v" || arg == "--verbose")
        {
            out.verbose = true;
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

    return true;
}

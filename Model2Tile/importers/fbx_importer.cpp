#include "fbx_importer.h"

#include "../fbx_pipeline.h"

#include <cctype>
#include <iostream>
#include <string>

namespace
{
bool EndsWithInsensitive(const std::string& value, const std::string& suffix)
{
    if (value.size() < suffix.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < suffix.size(); ++i)
    {
        const char a = static_cast<char>(std::tolower(value[value.size() - suffix.size() + i]));
        const char b = static_cast<char>(std::tolower(suffix[i]));
        if (a != b)
        {
            return false;
        }
    }
    return true;
}
} // namespace

const char* FbxImporter::FormatName() const
{
    return "fbx";
}

ImportContract FbxImporter::Contract() const
{
    ImportContract contract;
    contract.producesSceneIr = true;
    contract.emitsInstanceLodUris = true;
    return contract;
}

bool FbxImporter::Supports(const CliOptions& cli) const
{
    if (cli.inputFormat == "fbx")
    {
        return true;
    }

    if (cli.inputFormat != "auto")
    {
        return false;
    }

    return EndsWithInsensitive(cli.inputPath.string(), ".fbx");
}

int FbxImporter::Run(const CliOptions& cli) const
{
    return RunFbxPipeline(cli);
}

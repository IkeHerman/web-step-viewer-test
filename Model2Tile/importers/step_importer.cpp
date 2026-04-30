#include "step_importer.h"

#include "../step_pipeline.h"

#include <algorithm>
#include <cctype>

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

const char* StepImporter::FormatName() const
{
    return "step";
}

ImportContract StepImporter::Contract() const
{
    return ImportContract{};
}

bool StepImporter::Supports(const CliOptions& cli) const
{
    if (cli.inputFormat == "step")
    {
        return true;
    }

    if (cli.inputFormat != "auto")
    {
        return false;
    }

    const std::string path = cli.inputPath.string();
    return EndsWithInsensitive(path, ".step") || EndsWithInsensitive(path, ".stp");
}

int StepImporter::Run(const CliOptions& cli) const
{
    return RunStepPipeline(cli);
}

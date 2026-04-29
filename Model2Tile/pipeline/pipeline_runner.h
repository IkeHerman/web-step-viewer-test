#pragma once

#include "../app/cli_options.h"

class PipelineRunner
{
public:
    int Run(const CliOptions& cli) const;
};

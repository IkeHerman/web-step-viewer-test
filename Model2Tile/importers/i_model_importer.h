#pragma once

#include <string>

#include "../app/cli_options.h"

class IModelImporter
{
public:
    virtual ~IModelImporter() = default;
    virtual const char* FormatName() const = 0;
    virtual bool Supports(const CliOptions& cli) const = 0;
    virtual int Run(const CliOptions& cli) const = 0;
};

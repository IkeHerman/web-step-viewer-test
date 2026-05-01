#pragma once

#include <string>

#include "../app/cli_options.h"

struct ImportContract
{
    // Importers are expected to produce SceneIR as the primary intermediate
    // artifact used by tiling/export orchestration.
    bool producesSceneIr = true;
    // Importers should pre-bake per-instance high GLBs and attach URIs on instances.
    bool emitsInstanceLodUris = true;
};

class IModelImporter
{
public:
    virtual ~IModelImporter() = default;
    virtual const char* FormatName() const = 0;
    virtual ImportContract Contract() const = 0;
    virtual bool Supports(const CliOptions& cli) const = 0;
    virtual int Run(const CliOptions& cli) const = 0;
};

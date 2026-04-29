#include "pipeline_runner.h"

#include "../importers/fbx_importer.h"
#include "../importers/i_model_importer.h"
#include "../importers/step_importer.h"

#include <array>

int PipelineRunner::Run(const CliOptions& cli) const
{
    const StepImporter stepImporter;
    const FbxImporter fbxImporter;
    const std::array<const IModelImporter*, 2> importers = {
        &stepImporter,
        &fbxImporter
    };

    for (const IModelImporter* importer : importers)
    {
        if (importer->Supports(cli))
        {
            return importer->Run(cli);
        }
    }

    return stepImporter.Run(cli);
}

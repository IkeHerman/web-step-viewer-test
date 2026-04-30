#include "pipeline_runner.h"

#include "../importers/fbx_importer.h"
#include "../importers/i_model_importer.h"
#include "../importers/step_importer.h"

#include <array>
#include <iostream>

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
            const ImportContract contract = importer->Contract();
            if (!contract.producesSceneIr)
            {
                std::cerr << "[Pipeline] importer '" << importer->FormatName()
                          << "' does not yet satisfy SceneIR primary contract\n";
                return 2;
            }
            if (!contract.emitsInstanceLodUris)
            {
                std::cerr << "[Pipeline] importer '" << importer->FormatName()
                          << "' does not yet satisfy instance LOD URI contract\n";
                return 2;
            }
            return importer->Run(cli);
        }
    }

    return stepImporter.Run(cli);
}

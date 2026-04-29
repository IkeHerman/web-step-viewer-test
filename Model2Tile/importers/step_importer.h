#pragma once

#include "i_model_importer.h"

class StepImporter final : public IModelImporter
{
public:
    const char* FormatName() const override;
    bool Supports(const CliOptions& cli) const override;
    int Run(const CliOptions& cli) const override;
};

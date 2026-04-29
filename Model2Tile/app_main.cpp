#include "app/cli_options.h"
#include "pipeline/pipeline_runner.h"

int main(int argc, char** argv)
{
    CliOptions cli;
    int cliExitCode = 0;
    if (!ParseCli(argc, argv, cli, cliExitCode))
    {
        return cliExitCode;
    }

    const PipelineRunner runner;
    return runner.Run(cli);
}

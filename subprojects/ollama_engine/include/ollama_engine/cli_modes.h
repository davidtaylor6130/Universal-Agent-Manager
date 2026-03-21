#pragma once

/// <summary>Entry points for dedicated Ollama engine CLI modes.</summary>

namespace ollama_engine_cli {

/// <summary>Runs basic model load and chat CLI.</summary>
/// <param name="argc">Argument count.</param>
/// <param name="argv">Argument array.</param>
/// <returns>Process exit code.</returns>
int RunBasicLoadAndChatCli(int argc, char** argv);

/// <summary>Runs finetuning/autotune wizard CLI.</summary>
/// <param name="argc">Argument count.</param>
/// <param name="argv">Argument array.</param>
/// <returns>Process exit code.</returns>
int RunFinetuneWizardCli(int argc, char** argv);

/// <summary>Runs built-in benchmark auto-test CLI.</summary>
/// <param name="argc">Argument count.</param>
/// <param name="argv">Argument array.</param>
/// <returns>Process exit code.</returns>
int RunAutoTestCli(int argc, char** argv);

/// <summary>Runs custom Question_N test-folder CLI.</summary>
/// <param name="argc">Argument count.</param>
/// <param name="argv">Argument array.</param>
/// <returns>Process exit code.</returns>
int RunCustomTestsCli(int argc, char** argv);

}  // namespace ollama_engine_cli

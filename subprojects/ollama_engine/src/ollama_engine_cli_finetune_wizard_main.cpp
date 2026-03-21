#include "ollama_engine/cli_modes.h"

int main(int argc, char** argv) {
  return ollama_engine_cli::RunFinetuneWizardCli(argc, argv);
}

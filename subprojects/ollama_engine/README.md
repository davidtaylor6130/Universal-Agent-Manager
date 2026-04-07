# ollama_engine

`ollama_engine` is a standalone C++ local-model runtime that powers Universal Agent Manager (UAM).

It provides:

- a small engine API for model discovery, load, inference, and lifecycle polling
- CLI tools for chat, autotuning, benchmark probes, custom prompt tests, and evaluation suite runs
- optional bridge server target for local integration workflows

## Features

- Recursive `.gguf` model discovery from a configurable model root.
- Real llama.cpp-backed token inference when the `llama` target is available.
- Deterministic embedding payload generation for stable tests and app behavior.
- Built-in regression harness (`Question_N.txt` + expected answer files).
- Evaluation suite that builds an indexed code corpus and runs deterministic RAG checks.

## Runtime Modes

- `UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP=1` (llama.cpp linked): real GGUF model load and generation.
- llama.cpp not linked: engine still builds, but `Load()` fails with a linkage/configuration error.

## Requirements

- CMake `>= 3.20`
- C++20 compiler
- SQLite3 development package
- OpenSSL development package

## Build (Standalone)

Configure from this repository root:

```bash
cmake -S . -B build -DUAM_FETCH_LLAMA_CPP=ON
```

Build all runtime targets:

```bash
cmake --build build --target \
  uam_ollama_engine_cli \
  uam_ollama_engine_cli_finetune_wizard \
  uam_ollama_engine_cli_auto_test \
  uam_ollama_engine_cli_custom_tests \
  uam_ollama_engine_cli_eval_suite \
  uam_ollama_engine_bridge \
  --config Release --parallel
```

## CLI Targets

- `uam_ollama_engine_cli`: basic model load + chat loop (`/quit` to exit)
- `uam_ollama_engine_cli_finetune_wizard`: interactive autotune wizard and profile/template persistence
- `uam_ollama_engine_cli_auto_test`: built-in benchmark probes
- `uam_ollama_engine_cli_custom_tests`: `Question_N`/answer folder regression tests
- `uam_ollama_engine_cli_eval_suite`: deterministic LLM/RAG evaluation run

Run examples:

```bash
./build/uam_ollama_engine_cli [model_folder]
./build/uam_ollama_engine_cli_finetune_wizard [model_folder]
./build/uam_ollama_engine_cli_auto_test [model_folder]
./build/uam_ollama_engine_cli_custom_tests [model_folder] [tests_directory]
./build/uam_ollama_engine_cli_eval_suite [model_folder] [evaluation_root]
```

If `[model_folder]` is omitted, each CLI defaults to `<current-working-directory>/models`.

## Public API

Primary methods:

- `ListModels()`
- `Load(model_name, error_out)`
- `SendMessage(prompt)`
- `QueryCurrentState()`

Headers:

- `include/ollama_engine/engine_structures.h`
- `include/ollama_engine/engine_interface.h`
- `include/ollama_engine/engine_factory.h`
- `include/ollama_engine/engine_api.h`

## Model Discovery and Load Rules

`ListModels()`:

- scans recursively from configured model folder
- includes only `.gguf` files (case-insensitive)
- skips hidden files/folders (names starting with `.`)
- returns sorted relative paths

`Load(model_name)` accepts:

- the relative file path returned by `ListModels()`
- a directory path relative to model root (first discovered `.gguf` is selected)

## Minimal C++ Example

```cpp
#include "ollama_engine/engine_api.h"

ollama_engine::EngineOptions options;
options.pPathModelFolder = "/absolute/path/to/models";
options.piEmbeddingDimensions = 256;

auto engine = ollama_engine::CreateEngine(options);
const auto models = engine->ListModels();
if (!models.empty()) {
  std::string error;
  if (engine->Load(models.front(), &error)) {
    const auto response = engine->SendMessage("Hello");
    const auto state = engine->QueryCurrentState();
    (void)response;
    (void)state;
  }
}
```

## Repository Layout

- `include/ollama_engine/`: public headers
- `src/internal/`: runtime implementation (`local_ollama_engine`, embeddings, deterministic hash, vectorised RAG)
- `src/ollama_engine_cli_*.cpp`: CLI mode orchestration and entrypoints
- `models/`: local model storage for development runs
- `evaluation/`: indexed corpus assets and evaluation outputs

## Integration Notes

This repository can be consumed as a subdirectory dependency:

```cmake
add_subdirectory(path/to/ollama_engine)
```

If the parent project already defines a `llama` target, `ollama_engine` links against it automatically.

## Troubleshooting

- `No models found in folder ...`
  - wrong folder path, no `.gguf` files, or hidden-path filtering excluded files
- `Failed to load GGUF model via llama.cpp ...`
  - invalid/corrupt/incompatible GGUF, or llama.cpp linkage not present
- custom tests report no cases
  - filenames must match `Question_N.txt` + `Question_N_Answer.txt` (legacy `_Anser.txt` is also accepted)

## License

Licensed under the Universal Agent Manager License (UAML) v1.0. See `LICENSE`.
Originally created by David Taylor (davidtaylor6130).

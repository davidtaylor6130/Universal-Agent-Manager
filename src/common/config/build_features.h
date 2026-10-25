#pragma once

// CMake (cmake/build_features.cmake) defines every flag as 0 or 1 on each target's command line.
// There are no defaults here: a translation unit compiled without the flags must fail, not silently
// include an excluded provider or feature.
#if !defined(UAM_ENABLE_COMPUTER_USE) || !defined(UAM_ENABLE_SSH) || !defined(UAM_ENABLE_MOBILE_COMPANION)
#error "UAM_ENABLE_COMPUTER_USE, UAM_ENABLE_SSH and UAM_ENABLE_MOBILE_COMPANION must be defined by CMake as 0 or 1."
#endif
#if !defined(UAM_ENABLE_RUNTIME_GEMINI_CLI) || !defined(UAM_ENABLE_RUNTIME_CODEX_CLI) || !defined(UAM_ENABLE_RUNTIME_CLAUDE_CLI) || \
    !defined(UAM_ENABLE_RUNTIME_OPENCODE_CLI) || !defined(UAM_ENABLE_RUNTIME_COPILOT_CLI) || !defined(UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI)
#error "Every UAM_ENABLE_RUNTIME_<PROVIDER>_CLI flag must be defined by CMake as 0 or 1."
#endif
#if !(UAM_ENABLE_RUNTIME_GEMINI_CLI || UAM_ENABLE_RUNTIME_CODEX_CLI || UAM_ENABLE_RUNTIME_CLAUDE_CLI || UAM_ENABLE_RUNTIME_OPENCODE_CLI || \
      UAM_ENABLE_RUNTIME_COPILOT_CLI || UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI)
#error "At least one provider runtime must be enabled."
#endif
// A full build rejects unknown provider ids in saved data; a build that excludes providers keeps them inert.
#define UAM_ALL_PROVIDERS_INCLUDED (UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_RUNTIME_CODEX_CLI && UAM_ENABLE_RUNTIME_CLAUDE_CLI && \
    UAM_ENABLE_RUNTIME_OPENCODE_CLI && UAM_ENABLE_RUNTIME_COPILOT_CLI && UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI)

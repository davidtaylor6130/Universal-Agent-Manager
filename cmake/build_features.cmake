# One normalised build feature list. Native targets, the runner, tests and the Vite bundle
# all read these 0/1 values, so -DUAM_ENABLE_SSH=0, OFF or FALSE mean the same everywhere.
option(UAM_ENABLE_COMPUTER_USE "Include UAM computer use and platform input/capture support." ON)
option(UAM_ENABLE_SSH "Include SSH remote execution and bundled helpers." ON)
option(UAM_ENABLE_MOBILE_COMPANION "Include the mobile companion server and frontend." ON)
option(UAM_ENABLE_RUNTIME_GEMINI_CLI "Enable Gemini CLI runtime backend." ON)
option(UAM_ENABLE_RUNTIME_CODEX_CLI "Enable Codex CLI runtime backend." ON)
option(UAM_ENABLE_RUNTIME_CLAUDE_CLI "Enable Claude Code CLI runtime backend." ON)
option(UAM_ENABLE_RUNTIME_OPENCODE_CLI "Enable OpenCode CLI runtime backend." ON)
option(UAM_ENABLE_RUNTIME_COPILOT_CLI "Enable GitHub Copilot CLI runtime backend." ON)
option(UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI "Enable Antigravity CLI terminal runtime backend." ON)

# <frontend name>=<CMake option>; providers stay in canonical order (first enabled is the default).
set(UAM_BUILD_FEATURES
  COMPUTER_USE=UAM_ENABLE_COMPUTER_USE
  SSH=UAM_ENABLE_SSH
  MOBILE_COMPANION=UAM_ENABLE_MOBILE_COMPANION
  GEMINI=UAM_ENABLE_RUNTIME_GEMINI_CLI
  CODEX=UAM_ENABLE_RUNTIME_CODEX_CLI
  CLAUDE=UAM_ENABLE_RUNTIME_CLAUDE_CLI
  OPENCODE=UAM_ENABLE_RUNTIME_OPENCODE_CLI
  COPILOT=UAM_ENABLE_RUNTIME_COPILOT_CLI
  ANTIGRAVITY=UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
)

set(UAM_FEATURE_DEFINITIONS)
set(UAM_PROVIDER_DEFINITIONS)
set(UAM_UI_FEATURE_ENV)
set(UAM_ENABLED_PROVIDER_COUNT 0)
foreach(_uam_feature IN LISTS UAM_BUILD_FEATURES)
  string(REPLACE "=" ";" _uam_feature "${_uam_feature}")
  list(GET _uam_feature 0 _uam_feature_name)
  list(GET _uam_feature 1 _uam_feature_option)
  if(${_uam_feature_option})
    set(_uam_feature_value 1)
  else()
    set(_uam_feature_value 0)
  endif()
  set(UAM_FEATURE_${_uam_feature_name} ${_uam_feature_value})
  list(APPEND UAM_UI_FEATURE_ENV "UAM_FEATURE_${_uam_feature_name}=${_uam_feature_value}")
  if(_uam_feature_option MATCHES "^UAM_ENABLE_RUNTIME_")
    list(APPEND UAM_PROVIDER_DEFINITIONS "${_uam_feature_option}=${_uam_feature_value}")
    math(EXPR UAM_ENABLED_PROVIDER_COUNT "${UAM_ENABLED_PROVIDER_COUNT} + ${_uam_feature_value}")
  else()
    list(APPEND UAM_FEATURE_DEFINITIONS "${_uam_feature_option}=${_uam_feature_value}")
  endif()
endforeach()

if(UAM_ENABLED_PROVIDER_COUNT EQUAL 0)
  message(FATAL_ERROR "At least one provider runtime must be enabled.")
endif()

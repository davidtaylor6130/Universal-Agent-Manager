cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
project(universal_agent_manager VERSION 4.11.0 LANGUAGES C CXX)
set(UAM_PRODUCT_VERSION "4.11.0")
include("${CMAKE_SOURCE_DIR}/cmake/runner_source_contract.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/local_build_version.cmake")
set(UAM_LOCAL_BUILD_VERSION "" CACHE STRING "Optional local X.Y.Z-alpha-Q presentation version; release and runner versions remain unchanged.")
uam_local_build_presentation("${UAM_PRODUCT_VERSION}" "${UAM_LOCAL_BUILD_VERSION}" UAM_PRESENTATION_VERSION)

option(UAM_ENABLE_COMPUTER_USE "Include UAM computer use and platform input/capture support." ON)
option(UAM_ENABLE_SSH "Include SSH remote execution and bundled helpers." ON)
option(UAM_ENABLE_MOBILE_COMPANION "Include the mobile companion server and frontend." ON)
set(UAM_FEATURE_DEFINITIONS
  UAM_ENABLE_COMPUTER_USE=$<BOOL:${UAM_ENABLE_COMPUTER_USE}>
  UAM_ENABLE_SSH=$<BOOL:${UAM_ENABLE_SSH}>
  UAM_ENABLE_MOBILE_COMPANION=$<BOOL:${UAM_ENABLE_MOBILE_COMPANION}>
)
option(UAM_RUNNER_ONLY "Build only the headless remote runner." OFF)
option(UAM_PACKAGE_REMOTE_RUNNERS "Validate and package all supported SSH helpers." ON)
set(UAM_REMOTE_RUNNER_ARTIFACT_DIR "${CMAKE_SOURCE_DIR}/Builds/remote-artifacts" CACHE PATH
  "Directory containing linux-arm64, linux-x86_64, and windows-x86_64 runner folders.")

if(APPLE)
  enable_language(OBJCXX)
endif()

# Compiler launchers are supported by Make and Ninja; preserve explicit toolchain choices.
if(CMAKE_GENERATOR MATCHES "Makefiles|Ninja")
  find_program(UAM_CCACHE_EXECUTABLE NAMES ccache)
  if(UAM_CCACHE_EXECUTABLE)
    foreach(_language IN ITEMS C CXX OBJCXX)
      if(NOT DEFINED CMAKE_${_language}_COMPILER_LAUNCHER)
        set(CMAKE_${_language}_COMPILER_LAUNCHER "${UAM_CCACHE_EXECUTABLE}")
      endif()
    endforeach()
    message(STATUS "Compiler cache available: ${UAM_CCACHE_EXECUTABLE}")
  endif()
endif()

if(NOT WIN32 AND NOT APPLE AND NOT UAM_RUNNER_ONLY)
  message(FATAL_ERROR "universal_agent_manager supports only Windows and macOS builds.")
endif()

if(UAM_RUNNER_ONLY AND NOT UAM_ENABLE_SSH)
  message(FATAL_ERROR "UAM_RUNNER_ONLY requires UAM_ENABLE_SSH=ON.")
endif()
if(UAM_PACKAGE_REMOTE_RUNNERS AND NOT UAM_ENABLE_SSH)
  message(FATAL_ERROR "UAM_PACKAGE_REMOTE_RUNNERS requires UAM_ENABLE_SSH=ON. Disable runner packaging for a local-only build.")
endif()
if(NOT UAM_RUNNER_ONLY AND UAM_PACKAGE_REMOTE_RUNNERS)
  include("${CMAKE_SOURCE_DIR}/cmake/validate_remote_runners.cmake")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Source contract metadata must follow changed runner and shared source files.
set(CMAKE_SUPPRESS_REGENERATION OFF)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "_CMake")

# ---------------------------------------------------------------------------
# Build-directory enforcement
# ---------------------------------------------------------------------------

set(UAM_REQUIRED_BUILD_ROOT "${CMAKE_SOURCE_DIR}/Builds")
file(MAKE_DIRECTORY "${UAM_REQUIRED_BUILD_ROOT}")
file(REAL_PATH "${CMAKE_SOURCE_DIR}" UAM_SOURCE_DIR_REAL)
file(REAL_PATH "${UAM_REQUIRED_BUILD_ROOT}" UAM_REQUIRED_BUILD_ROOT_REAL)
file(REAL_PATH "${CMAKE_BINARY_DIR}" UAM_BINARY_DIR_REAL)
cmake_path(
  RELATIVE_PATH UAM_BINARY_DIR_REAL
  BASE_DIRECTORY "${UAM_REQUIRED_BUILD_ROOT_REAL}"
  OUTPUT_VARIABLE UAM_BINARY_DIR_RELATIVE_TO_REQUIRED_ROOT
)
cmake_path(
  RELATIVE_PATH UAM_BINARY_DIR_REAL
  BASE_DIRECTORY "${UAM_SOURCE_DIR_REAL}"
  OUTPUT_VARIABLE UAM_BINARY_DIR_RELATIVE_TO_SOURCE_ROOT
)

set(UAM_BUILD_DIR_IN_REQUIRED_ROOT TRUE)
if(UAM_BINARY_DIR_RELATIVE_TO_REQUIRED_ROOT MATCHES "^\\.\\.")
  set(UAM_BUILD_DIR_IN_REQUIRED_ROOT FALSE)
endif()

set(UAM_BUILD_DIR_IS_CLION_DEFAULT FALSE)
if(NOT UAM_BINARY_DIR_RELATIVE_TO_SOURCE_ROOT MATCHES "^\\.\\." AND
   UAM_BINARY_DIR_RELATIVE_TO_SOURCE_ROOT MATCHES "^cmake-build-")
  set(UAM_BUILD_DIR_IS_CLION_DEFAULT TRUE)
endif()

if(NOT UAM_BUILD_DIR_IN_REQUIRED_ROOT)
  if(UAM_BUILD_DIR_IS_CLION_DEFAULT)
    message(STATUS
      "Using CLion default build directory '${CMAKE_BINARY_DIR}'. "
      "Preferred location is '${UAM_REQUIRED_BUILD_ROOT}'."
    )
  else()
    message(FATAL_ERROR
      "Build directory must be inside '${UAM_REQUIRED_BUILD_ROOT}'.\n"
      "Use one of:\n"
      "  cmake -S . -B Builds\n"
      "  cmake -S . -B Builds/<name>\n"
      "\n"
      "CLion default profiles are also accepted (for example: cmake-build-debug)."
    )
  endif()
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(UAM_BUILD_TESTS "Build UAM core tests executable." OFF)
option(UAM_ENABLE_RUNTIME_GEMINI_CLI "Enable Gemini CLI runtime backend." ON)
option(UAM_ENABLE_RUNTIME_CODEX_CLI "Enable Codex CLI runtime backend." ON)
option(UAM_ENABLE_RUNTIME_CLAUDE_CLI "Enable Claude Code CLI runtime backend." ON)
option(UAM_ENABLE_RUNTIME_OPENCODE_CLI "Enable OpenCode CLI runtime backend." ON)
option(UAM_ENABLE_RUNTIME_COPILOT_CLI "Enable GitHub Copilot CLI runtime backend." ON)
option(UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI "Enable Antigravity CLI terminal runtime backend." ON)

foreach(_removed_runtime_flag IN ITEMS
  UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED
  UAM_ENABLE_RUNTIME_OPENCODE_LOCAL
  UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
  UAM_ENABLE_ENGINE_RAG
)
  if(DEFINED ${_removed_runtime_flag})
    message(FATAL_ERROR
      "${_removed_runtime_flag} is no longer supported in this release slice. "
      "Only UAM_ENABLE_RUNTIME_GEMINI_CLI, UAM_ENABLE_RUNTIME_CODEX_CLI, UAM_ENABLE_RUNTIME_CLAUDE_CLI, UAM_ENABLE_RUNTIME_OPENCODE_CLI, UAM_ENABLE_RUNTIME_COPILOT_CLI, and UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI are supported."
    )
  endif()
endforeach()

if(NOT UAM_ENABLE_RUNTIME_GEMINI_CLI AND NOT UAM_ENABLE_RUNTIME_CODEX_CLI AND NOT UAM_ENABLE_RUNTIME_CLAUDE_CLI AND NOT UAM_ENABLE_RUNTIME_OPENCODE_CLI AND NOT UAM_ENABLE_RUNTIME_COPILOT_CLI AND NOT UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI)
  message(FATAL_ERROR "At least one provider runtime must be enabled.")
endif()

set(UAM_FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH
  "Directory used for FetchContent source checkouts.")
if(APPLE)
  option(UAM_MACOS_BUNDLE "Build as a macOS .app bundle (no Terminal window needed)." ON)
  set(UAM_MACOS_SIGNING_IDENTITY "-" CACHE STRING "macOS codesign SHA-1 identity; '-' keeps portable ad-hoc signing.")
  if(UAM_MACOS_BUNDLE AND NOT UAM_RUNNER_ONLY AND NOT UAM_MACOS_SIGNING_IDENTITY STREQUAL "-")
    string(LENGTH "${UAM_MACOS_SIGNING_IDENTITY}" _uam_signing_identity_length)
    if(NOT _uam_signing_identity_length EQUAL 40 OR NOT UAM_MACOS_SIGNING_IDENTITY MATCHES "^[0-9A-Fa-f]+$")
      message(FATAL_ERROR "UAM_MACOS_SIGNING_IDENTITY must be a 40-character codesign identity SHA-1 or '-' for ad-hoc signing.")
    endif()
    execute_process(COMMAND /usr/bin/security find-identity -v -p codesigning
      RESULT_VARIABLE _uam_signing_identity_status OUTPUT_VARIABLE _uam_signing_identities ERROR_VARIABLE _uam_signing_identity_error)
    string(TOUPPER "${UAM_MACOS_SIGNING_IDENTITY}" _uam_signing_identity)
    string(FIND "${_uam_signing_identities}" " ${_uam_signing_identity} " _uam_signing_identity_index)
    if(NOT _uam_signing_identity_status EQUAL 0 OR _uam_signing_identity_index EQUAL -1)
      message(FATAL_ERROR "Configured macOS signing identity '${UAM_MACOS_SIGNING_IDENTITY}' is unavailable: ${_uam_signing_identity_error}")
    endif()
  endif()
endif()

file(MAKE_DIRECTORY "${UAM_FETCHCONTENT_BASE_DIR}")
set(FETCHCONTENT_BASE_DIR "${UAM_FETCHCONTENT_BASE_DIR}")

function(uam_set_target_folder target_name folder_name)
  if(TARGET ${target_name})
    set_property(TARGET ${target_name} PROPERTY FOLDER ${folder_name})
  endif()
endfunction()

include(FetchContent)

# ---------------------------------------------------------------------------
# nlohmann/json (header-only)
# ---------------------------------------------------------------------------

set(CMAKE_FOLDER "ThirdParty/nlohmann_json")
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)
set(CMAKE_FOLDER "")

# ---------------------------------------------------------------------------
# CEF (Chromium Embedded Framework) — platform-aware prebuilt download
# ---------------------------------------------------------------------------

if(NOT UAM_RUNNER_ONLY)

# Platform-specific CEF versions (latest stable from cef-builds.spotifycdn.com).
if(WIN32)
  set(CEF_PLATFORM "windows64")
  set(PROJECT_ARCH "x86_64")
  set(CEF_VERSION "146.0.10+g8219561+chromium-146.0.7680.179")
  set(CEF_SHA256 "f5b0a6fcd3c6d4abd7486e849db2640c27bb4d94d7b792691be348dec8063ff6")
elseif(APPLE)
  if(CMAKE_OSX_ARCHITECTURES)
    if(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
      set(CEF_PLATFORM "macosarm64")
      set(PROJECT_ARCH "arm64")
      set(CEF_SHA256 "7ac8e9ba2980bfa607c64f3e4d67feb630ebb9ba2d680f8945212b79d30da2ae")
    elseif(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
      set(CEF_PLATFORM "macosx64")
      set(PROJECT_ARCH "x86_64")
      set(CEF_SHA256 "a245962946c4ae90cd7b31a998a5b0b4aac88f71d55c682c5c7ec4099486271c")
    else()
      message(FATAL_ERROR "CEF macOS builds require exactly one architecture: arm64 or x86_64.")
    endif()
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    set(CEF_PLATFORM "macosarm64")
    set(PROJECT_ARCH "arm64")
    set(CEF_SHA256 "7ac8e9ba2980bfa607c64f3e4d67feb630ebb9ba2d680f8945212b79d30da2ae")
  else()
    set(CEF_PLATFORM "macosx64")
    set(PROJECT_ARCH "x86_64")
    set(CEF_SHA256 "a245962946c4ae90cd7b31a998a5b0b4aac88f71d55c682c5c7ec4099486271c")
  endif()
  set(CEF_VERSION "146.0.10+g8219561+chromium-146.0.7680.179")
else()
  set(CEF_PLATFORM "linux64")
  set(PROJECT_ARCH "x86_64")
  set(CEF_VERSION "138.0.61+g664e74a+chromium-138.0.7204.308")
  set(CEF_SHA256 "dd2de7339c4f56de23ed2d80c9b11db81a6553180c18af64f694963a50be5707")
endif()

# Encode the + characters as %2B for the URL.
string(REPLACE "+" "%2B" CEF_VERSION_ENCODED "${CEF_VERSION}")

set(CMAKE_FOLDER "ThirdParty/CEF")
FetchContent_Declare(
  cef_binary
  URL "https://cef-builds.spotifycdn.com/cef_binary_${CEF_VERSION_ENCODED}_${CEF_PLATFORM}_minimal.tar.bz2"
  URL_HASH "SHA256=${CEF_SHA256}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# Force libcef_dll_wrapper to be a STATIC library regardless of BUILD_SHARED_LIBS.
# Shared wrapper builds can hide symbols and leave the app with unresolved CEF
# references at link time.
set(_saved_bsl ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cef_binary)
set(BUILD_SHARED_LIBS ${_saved_bsl} CACHE BOOL "" FORCE)
set(CMAKE_FOLDER "")
endif()

# FetchContent_MakeAvailable already processes the CEF CMakeLists.txt, which
# builds libcef_dll_wrapper and creates the libcef_dll_wrapper target.
# No explicit add_subdirectory needed.

# ---------------------------------------------------------------------------
# Provider sources
# ---------------------------------------------------------------------------

set(UAM_PROVIDER_SOURCES)
if(UAM_ENABLE_RUNTIME_GEMINI_CLI)
  list(APPEND UAM_PROVIDER_SOURCES
    src/common/provider/gemini/cli/gemini_cli_provider_runtime.cpp
    src/common/provider/gemini/cli/gemini_native_agent_history.cpp
    src/common/provider/gemini/base/gemini_history_loader.cpp
  )
endif()
if(UAM_ENABLE_RUNTIME_CODEX_CLI)
  list(APPEND UAM_PROVIDER_SOURCES
    src/common/provider/codex/cli/codex_cli_provider_runtime.cpp
  )
endif()
if(UAM_ENABLE_RUNTIME_CLAUDE_CLI)
  list(APPEND UAM_PROVIDER_SOURCES
    src/common/provider/claude/cli/claude_cli_provider_runtime.cpp
    src/common/provider/claude/cli/claude_native_agent_history.cpp
  )
endif()
if(UAM_ENABLE_RUNTIME_OPENCODE_CLI)
  list(APPEND UAM_PROVIDER_SOURCES
    src/common/provider/opencode/cli/opencode_cli_provider_runtime.cpp
    src/common/provider/opencode/v2/opencode_v2_session.cpp
    src/common/provider/opencode/v2/opencode_v2_policy.cpp
    src/common/provider/opencode/v2/opencode_v2_bridge.cpp
  )
endif()
if(UAM_ENABLE_RUNTIME_COPILOT_CLI)
  list(APPEND UAM_PROVIDER_SOURCES
    src/common/provider/copilot/cli/copilot_cli_provider_runtime.cpp
    src/common/provider/copilot/cli/copilot_native_protocol.cpp
    src/common/provider/copilot/cli/copilot_managed_read_policy.cpp
  )
endif()

if(UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI)
  list(APPEND UAM_PROVIDER_SOURCES src/common/provider/antigravity/cli/antigravity_cli_provider_runtime.cpp)
endif()

# ---------------------------------------------------------------------------
# Platform sources
# ---------------------------------------------------------------------------

set(UAM_PLATFORM_SOURCES)
if(APPLE)
  set(UAM_ATTACHMENT_PREVIEW_PLATFORM src/app/attachment_preview_platform_macos.mm)
elseif(WIN32)
  set(UAM_ATTACHMENT_PREVIEW_PLATFORM src/app/attachment_preview_platform_windows.cpp)
endif()

set(UAM_PLATFORM_IMPL_SOURCES
  src/common/platform/platform_native_attachment_assets.cpp
  src/common/platform/platform_incremental_sha256.cpp)
set(UAM_COMPUTER_USE_PLATFORM_SOURCES)
if(WIN32)
  list(APPEND UAM_PLATFORM_SOURCES src/uam.rc)
  list(APPEND UAM_PLATFORM_IMPL_SOURCES
    src/common/platform/platform_services_windows_impl.cpp
    src/common/platform/platform_pty_windows.cpp
    src/common/platform/platform_process_windows.cpp
    src/common/platform/platform_dialogs_windows.cpp
    src/common/platform/platform_paths_windows.cpp
    src/common/platform/platform_dictation_windows.cpp
    src/common/platform/platform_speech_output_windows.cpp
  )
  list(APPEND UAM_COMPUTER_USE_PLATFORM_SOURCES
    src/computer_use/computer_use_platform_windows.cpp
  )
elseif(APPLE)
  list(APPEND UAM_PLATFORM_IMPL_SOURCES
    src/common/platform/platform_application_macos.mm
    src/common/platform/platform_parent_watchdog_macos.cpp
    src/common/platform/platform_services_macos_impl.cpp
    src/common/platform/platform_pty_macos.cpp
    src/common/platform/platform_process_macos.cpp
    src/common/platform/platform_dialogs_macos.cpp
    src/common/platform/platform_paths_macos.cpp
    src/common/platform/platform_dictation_macos.mm
    src/common/platform/platform_speech_output_macos.mm
  )
  list(APPEND UAM_COMPUTER_USE_PLATFORM_SOURCES
    src/computer_use/computer_use_platform_macos.mm
    src/computer_use/computer_use_launch_macos.mm
    src/computer_use/computer_use_install_macos.mm
  )
  set_source_files_properties(
    src/common/platform/platform_dictation_macos.mm
    src/common/platform/platform_speech_output_macos.mm
    src/computer_use/computer_use_platform_macos.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc"
  )
endif()

if(NOT UAM_ENABLE_COMPUTER_USE)
  set(UAM_COMPUTER_USE_PLATFORM_SOURCES src/computer_use/computer_use_disabled.cpp)
endif()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------

add_executable(uam_runner
  src/common/memory/memory_library_store.cpp
  src/common/usage/transcript_usage.cpp
  src/common/usage/antigravity_accounting.cpp
  src/common/usage/antigravity_history_scan.cpp
  src/common/usage/usage_aggregation.cpp
  src/common/usage/transcript_scan.cpp
  src/common/usage/opencode_history_scan.cpp
  src/common/usage/usage_history_service.cpp
  src/common/usage/usage_ledger.cpp
  src/common/usage/usage_store.cpp
  src/common/usage/usage_collector.cpp
  src/common/usage/usage_pricing.cpp
  src/common/usage/usage_pricing_cache.cpp
  src/common/provider/opencode/cli/opencode_native_http.cpp
  src/remote/runner_main.cpp
  src/remote/runner_client.cpp
  src/remote/runner_protocol.cpp
  src/remote/runner_proxy.cpp
  src/remote/runner_state.cpp
)
set_target_properties(uam_runner PROPERTIES
  EXCLUDE_FROM_ALL $<NOT:$<BOOL:${UAM_ENABLE_SSH}>>
  OUTPUT_NAME "uam-runner"
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  FOLDER "Remote"
)
# A header avoids rewriting Make's shared flags file when only the fingerprint changes.
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/generated/uam_runner_build.h"
  CONTENT "#pragma once\n#define UAM_RUNNER_SOURCE_FINGERPRINT \"@UAM_RUNNER_SOURCE_FINGERPRINT@\"\n" @ONLY)
target_include_directories(uam_runner PRIVATE src "${CMAKE_BINARY_DIR}/generated")
# OpenCode and other provider histories use SQLite. Read them directly with read-only connections.
FetchContent_Declare(uam_sqlite
  URL https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip
  URL_HASH SHA256=68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(uam_sqlite)
add_library(uam_sqlite STATIC "${uam_sqlite_SOURCE_DIR}/sqlite3.c")
target_include_directories(uam_sqlite PUBLIC "${uam_sqlite_SOURCE_DIR}")
target_compile_definitions(uam_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)
if(MSVC)
  set_property(TARGET uam_sqlite PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  )
endif()
target_link_libraries(uam_runner PRIVATE uam_sqlite)

target_link_libraries(uam_runner PRIVATE nlohmann_json::nlohmann_json)
if(APPLE)
  target_sources(uam_runner PRIVATE
    src/common/platform/platform_parent_watchdog_macos.cpp
    src/common/platform/platform_process_macos.cpp
    src/remote/runner_service_posix.cpp
  )
  target_link_libraries(uam_runner PRIVATE
    "-Wl,-framework,Security"
    "-Wl,-framework,AppKit"
    "-Wl,-framework,IOKit"
    "-Wl,-framework,Foundation"
  )
elseif(WIN32)
  target_sources(uam_runner PRIVATE
    src/common/platform/platform_process_windows.cpp
    src/remote/runner_service_windows.cpp
  )
  target_link_libraries(uam_runner PRIVATE advapi32 shell32 user32 ws2_32)
elseif(UNIX)
  find_package(Threads REQUIRED)
  target_sources(uam_runner PRIVATE
    src/common/platform/platform_process_linux.cpp
    src/remote/runner_service_posix.cpp
  )
  target_link_libraries(uam_runner PRIVATE Threads::Threads)
endif()
if(MSVC)
  set_property(TARGET uam_runner PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  )
elseif(MINGW)
  target_link_options(uam_runner PRIVATE -static -static-libgcc -static-libstdc++)
endif()
target_compile_definitions(uam_runner PRIVATE
  ${UAM_FEATURE_DEFINITIONS}
  NOMINMAX
  UAM_REMOTE_RUNNER_VERSION="${UAM_PRODUCT_VERSION}"
  UAM_HEADLESS_RUNNER=1
  UAM_ENABLE_RUNTIME_GEMINI_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_GEMINI_CLI}>
  UAM_ENABLE_RUNTIME_CODEX_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CODEX_CLI}>
  UAM_ENABLE_RUNTIME_CLAUDE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CLAUDE_CLI}>
  UAM_ENABLE_RUNTIME_OPENCODE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_OPENCODE_CLI}>
  UAM_ENABLE_RUNTIME_COPILOT_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_COPILOT_CLI}>
  UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI}>
)
set(UAM_RUNNER_SHA256_FILE "$<TARGET_FILE_DIR:uam_runner>/uam-runner.sha256")
set(UAM_RUNNER_VERSION_FILE "$<TARGET_FILE_DIR:uam_runner>/uam-runner.version")
add_custom_command(TARGET uam_runner POST_BUILD
  COMMAND ${CMAKE_COMMAND}
    -DINPUT=$<TARGET_FILE:uam_runner>
    -DOUTPUT=${UAM_RUNNER_SHA256_FILE}
    -DVERSION=${UAM_PRODUCT_VERSION}
    -DVERSION_OUTPUT=${UAM_RUNNER_VERSION_FILE}
    -DSOURCE_FINGERPRINT=${UAM_RUNNER_SOURCE_FINGERPRINT}
    -DPROTOCOL_VERSION=${UAM_RUNNER_PROTOCOL_VERSION}
    -DMANIFEST_OUTPUT=$<TARGET_FILE_DIR:uam_runner>/uam-runner.manifest.json
    -P "${CMAKE_SOURCE_DIR}/cmake/write_sha256.cmake"
  VERBATIM
  COMMENT "Writing the remote runner checksum and version"
)

if(UAM_RUNNER_ONLY)
  return()
endif()

set(UAM_EXECUTABLE_KIND "")
if(APPLE AND UAM_MACOS_BUNDLE)
  set(UAM_EXECUTABLE_KIND MACOSX_BUNDLE)
elseif(WIN32)
  set(UAM_EXECUTABLE_KIND WIN32)
endif()

add_executable(universal_agent_manager ${UAM_EXECUTABLE_KIND}
  ${UAM_PLATFORM_SOURCES}
  src/app/attachment_preview_service.cpp
  ${UAM_ATTACHMENT_PREVIEW_PLATFORM}
  ${UAM_PLATFORM_IMPL_SOURCES}
  ${UAM_COMPUTER_USE_PLATFORM_SOURCES}
  src/main.cpp
  src/common/platform/app_crash_supervisor.cpp
  src/common/platform/companion_network_platform.cpp
	  src/computer_use/computer_use_mcp_config.cpp
	  $<$<BOOL:${UAM_ENABLE_COMPUTER_USE}>:src/computer_use/computer_use_mcp_server.cpp>
	  src/app/agent_definition_service.cpp
	  src/app/agent_run_ledger.cpp
	  src/app/agent_run_scheduler.cpp
	  src/app/managed_parent_continuation_service.cpp
	    src/app/managed_subagent_control_service.cpp
	    src/app/managed_subagent_followup_service.cpp
	  src/app/uam_control_service.cpp
  src/app/application.cpp
  src/common/utils/diagnostic_log.cpp
  src/app/chat_domain_service.cpp
  src/app/chat_lifecycle_service.cpp
  src/app/computer_use_service.cpp
  src/app/git_worktree_service.cpp
  src/app/worktree_retention_service.cpp
  src/app/storage_retention_service.cpp
  src/app/worktree_launch_preparation_service.cpp
  src/app/local_chat_bundle_service.cpp
  src/app/markdown_store_service.cpp
  src/common/memory/memory_library_store.cpp
  src/app/memory_library_service.cpp
  src/app/memory_service.cpp
  src/app/goal_service.cpp
  src/app/native_session_link_service.cpp
  src/app/persistence_coordinator.cpp
  src/app/provider_resolution_service.cpp
  src/app/chat_file_change_receipt_service.cpp
  src/app/organization_service.cpp
  src/app/transcript_retention_service.cpp
  src/app/chat_search_service.cpp
  src/app/execution_routing_service.cpp
  src/app/repository_route_match_service.cpp
  src/app/execution_routing_integration.cpp
  src/app/resource_collection_service.cpp
    src/app/provider_worker_command.cpp
    src/app/chat_title_service.cpp
  src/app/provider_profile_migration_service.cpp
  src/app/provider_model_catalog_service.cpp
  src/app/copilot_agent_history_service.cpp
  src/app/native_agent_history_service.cpp
  src/app/opencode_native_activity_service.cpp
  src/common/provider/opencode/cli/opencode_terminal_activity.cpp
  src/app/runtime_orchestration_services.cpp
  src/app/startup_native_import_service.cpp
  src/app/opencode_compaction_sync_service.cpp
  src/app/opencode_compaction_history.cpp
  src/app/model_discovery_service.cpp
  src/app/shell_action_service.cpp
  src/app/theme_service.cpp
  src/app/vcs_commit_service.cpp
  src/common/discovery/management_discovery.cpp
  src/app/workspace_vcs_context_service.cpp
  src/cef/uam_cef_app.cpp
  src/cef/uam_cef_command_line_config.cpp
  src/cef/uam_cef_client.cpp
  src/cef/uam_bridge_request.cpp
  $<$<BOOL:${UAM_ENABLE_MOBILE_COMPANION}>:src/cef/companion_server.cpp>
  src/cef/uam_query_handler.cpp
  src/cef/state_serializer.cpp
  src/cef/cef_push.cpp
  src/cef/chat_title_handlers.cpp
  src/cef/chat_handlers.cpp
  src/cef/organization_handlers.cpp
  src/cef/chat_receipt_handlers.cpp
  src/cef/startup_native_import_handlers.cpp
  src/cef/chat_config_handlers.cpp
  src/cef/managed_subagent_handlers.cpp
  src/cef/settings_handlers.cpp
  src/cef/remote_host_handlers.cpp
  src/cef/local_chat_bundle_handlers.cpp
  src/cef/theme_handlers.cpp
  src/cef/shell_action_handlers.cpp
  src/cef/markdown_handlers.cpp
  src/cef/folder_handlers.cpp
  src/cef/chat_branch_handlers.cpp
  src/cef/repository_route_handlers.cpp
  src/cef/resource_collection_handlers.cpp
  src/cef/memory_handlers.cpp
  src/cef/workspace_handlers.cpp
  src/cef/vcs_handlers.cpp
  src/cef/terminal_handlers.cpp
  src/cef/dictation_handlers.cpp
  src/cef/attachment_preview_handlers.cpp
  src/cef/usage_handlers.cpp
  src/cef/acp_handlers.cpp
  src/cef/goal_handlers.cpp
  src/common/models/app_models.cpp
  src/common/paths/app_paths.cpp
  src/common/chat/chat_folder_store.cpp
  src/common/chat/chat_repository.cpp
  src/common/chat/native_media_chat_save.cpp
  src/common/chat/branch_native_assets.cpp
  src/common/chat/chat_message_page_index.cpp
  src/common/chat/chat_branching.cpp
  src/common/chat/native_chat_identity.cpp
  src/common/usage/transcript_usage.cpp
  src/common/usage/antigravity_accounting.cpp
  src/common/usage/antigravity_history_scan.cpp
  src/common/usage/usage_aggregation.cpp
  src/common/usage/transcript_scan.cpp
  src/common/usage/opencode_history_scan.cpp
  src/common/usage/usage_history_service.cpp
  src/common/usage/usage_ledger.cpp
  src/common/usage/usage_store.cpp
  src/common/usage/usage_collector.cpp
  src/common/usage/multi_host_history.cpp
    src/common/usage/account_allowance_fetcher.cpp
    src/common/usage/account_allowance_service.cpp
    src/common/usage/account_credentials_macos.cpp
    src/common/usage/google_account_allowance.cpp
    src/common/usage/claude_account_allowance.cpp
  src/common/usage/usage_pricing.cpp
  src/common/usage/usage_pricing_cache.cpp
  src/core/chat_import_utils.cpp
  src/app/opencode_import_stream.cpp
  src/common/config/settings_store.cpp
  src/common/utils/command_line_words.cpp
  src/common/config/frontend_actions.cpp
  src/common/provider/provider_profile.cpp
  src/common/provider/runtime/provider_runtime_facade.cpp
  src/common/provider/runtime/provider_runtime_registry.cpp
  src/common/provider/runtime/provider_acp_strategy.cpp
  src/common/provider/structured_attachments.cpp
  src/common/provider/opencode/cli/opencode_native_http.cpp
  src/common/provider/claude/cli/claude_acp_message_handlers.cpp
  src/common/provider/codex/cli/codex_acp_message_handlers.cpp
  src/common/runtime/acp/acp_content_text.cpp
  src/common/runtime/acp/acp_diagnostics.cpp
  src/common/runtime/acp/acp_goal_helpers.cpp
  src/common/runtime/acp/acp_goal_loop.cpp
  src/common/runtime/acp/acp_message_sync.cpp
  src/common/runtime/acp/acp_permission_handlers.cpp
  src/common/runtime/acp/acp_polling.cpp
  src/common/runtime/acp/acp_replay_state.cpp
  src/common/runtime/acp/acp_response_handlers.cpp
  src/common/runtime/acp/acp_session_lifecycle.cpp
  src/common/runtime/acp/acp_session_update_handler.cpp
  src/common/runtime/acp/acp_tool_call_state.cpp
  src/common/runtime/acp/acp_turn_events.cpp
  src/common/runtime/acp/acp_json_helpers.cpp
  src/common/runtime/acp/acp_launch.cpp
  src/common/runtime/acp/acp_message_detail.cpp
  src/common/runtime/acp/acp_request_builders.cpp
  src/common/runtime/acp/acp_resume_rules.cpp
  src/common/runtime/acp/claude_quota_continuation.cpp
  src/common/provider/attached_skill_resources.cpp
  src/common/runtime/acp/acp_session_runtime.cpp
  src/common/runtime/acp/acp_wait_state.cpp
  src/common/runtime/json_runtime.cpp
  src/common/runtime/terminal_polling.cpp
  src/common/runtime/terminal/terminal_chat_sync.cpp
  src/common/runtime/terminal/terminal_debug_diagnostics.cpp
  src/common/runtime/terminal/terminal_provider_cli.cpp
  src/common/runtime/terminal/terminal_idle_classifier.cpp
  src/common/runtime/terminal/terminal_identity.cpp
  src/common/runtime/terminal/terminal_lifecycle.cpp
  src/core/gemini_cli_compat.cpp
  ${UAM_PROVIDER_SOURCES}
  src/common/runtime/provider_cli_compatibility_service.cpp
  src/remote/runner_bootstrap.cpp
  src/remote/runner_client.cpp
  src/remote/runner_proxy.cpp
  src/common/platform/platform_services.cpp
)

set_target_properties(universal_agent_manager PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

target_include_directories(universal_agent_manager
  PRIVATE
    src
    src/common
    ${cef_binary_SOURCE_DIR}
)

find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
  # A fresh desktop toolchain can build its dependency in this output folder.
  set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
  set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(uam_zlib
    URL https://zlib.net/zlib-1.3.2.tar.gz
    URL_HASH SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
  )
  FetchContent_MakeAvailable(uam_zlib)
  add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()
target_link_libraries(universal_agent_manager PRIVATE ZLIB::ZLIB)

target_link_libraries(universal_agent_manager PRIVATE uam_sqlite)

# CEF link targets depend on platform.
# The CEF distribution's own CMakeLists.txt exposes 'libcef_dll_wrapper'.
# The actual CEF library must be linked by name.
if(APPLE)
  # Use LINK_FLAGS (a raw string) so the shell can parse the double-quoted path
  # that contains spaces (the project root contains spaces).
  set_property(TARGET universal_agent_manager APPEND PROPERTY
    LINK_FLAGS
    "-F\"${cef_binary_SOURCE_DIR}/Release\" -framework \"Chromium Embedded Framework\""
  )
  target_link_libraries(universal_agent_manager PRIVATE
    nlohmann_json::nlohmann_json
    libcef_dll_wrapper
    "-Wl,-framework,Security"
    "-Wl,-framework,AppKit"
    "-Wl,-framework,WebKit"
    "-Wl,-framework,IOKit"
    "-Wl,-framework,CoreFoundation"
    "-Wl,-framework,AVFoundation"
    "-Wl,-framework,Foundation"
    "-Wl,-framework,Speech"
    "$<$<BOOL:${UAM_ENABLE_COMPUTER_USE}>:-Wl,-framework,ApplicationServices>"
    "$<$<BOOL:${UAM_ENABLE_COMPUTER_USE}>:-Wl,-framework,Carbon>"
    "-Wl,-framework,ImageIO"
    "-Wl,-framework,CoreGraphics"
    "$<$<BOOL:${UAM_ENABLE_COMPUTER_USE}>:-Wl,-framework,QuartzCore>"
    "$<$<BOOL:${UAM_ENABLE_COMPUTER_USE}>:-Wl,-weak_framework,ScreenCaptureKit>"
  )
elseif(WIN32)
  target_link_libraries(universal_agent_manager PRIVATE
    nlohmann_json::nlohmann_json
    libcef_dll_wrapper
    "${cef_binary_SOURCE_DIR}/Release/libcef.lib"
    advapi32
    shell32
    windowsapp
    dnsapi
    gdi32
    ole32
    uuid
    oleaut32
    user32
    uiautomationcore
    windowscodecs
    ws2_32
  )
endif()

target_compile_definitions(universal_agent_manager PRIVATE
  ${UAM_FEATURE_DEFINITIONS}
  NOMINMAX
  UAM_REMOTE_RUNNER_VERSION="${UAM_PRODUCT_VERSION}"
  UAM_ENABLE_RUNTIME_GEMINI_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_GEMINI_CLI}>
  UAM_ENABLE_RUNTIME_CODEX_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CODEX_CLI}>
  UAM_ENABLE_RUNTIME_CLAUDE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CLAUDE_CLI}>
  UAM_ENABLE_RUNTIME_OPENCODE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_OPENCODE_CLI}>
  UAM_ENABLE_RUNTIME_COPILOT_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_COPILOT_CLI}>
  UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI}>
)

set_target_properties(universal_agent_manager PROPERTIES FOLDER "App")
if(MSVC)
  set_property(TARGET universal_agent_manager PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  )
  set_target_properties(universal_agent_manager PROPERTIES
    VS_GLOBAL_VcpkgEnabled "false"
    VS_GLOBAL_VcpkgApplocalDeps "false"
  )
endif()

# ---------------------------------------------------------------------------
# CEF binary resources — copy to build output
# ---------------------------------------------------------------------------

# macOS: copy the .framework into the .app bundle or next to the binary,
#        and create the four CEF helper app bundles required by the macOS
#        multi-process architecture.  Without these, CefInitialize() crashes
#        with EXC_BREAKPOINT (SIGTRAP) because Chromium cannot spawn its
#        renderer / GPU / plugin subprocesses.
if(APPLE)
  set(CEF_FRAMEWORK_SRC "${cef_binary_SOURCE_DIR}/Release/Chromium Embedded Framework.framework")
  if(UAM_MACOS_BUNDLE)
    add_custom_command(TARGET universal_agent_manager POST_BUILD
      COMMAND /bin/sh -c
        "codesign --verify --strict '$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Frameworks/Chromium Embedded Framework.framework' && cmp -s '${CEF_FRAMEWORK_SRC}/Resources/Info.plist' '$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Frameworks/Chromium Embedded Framework.framework/Resources/Info.plist' || '${CMAKE_COMMAND}' -E copy_directory '${CEF_FRAMEWORK_SRC}' '$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Frameworks/Chromium Embedded Framework.framework'"
      VERBATIM
      COMMENT "Ensuring CEF framework is present in .app bundle"
    )

    # -----------------------------------------------------------------------
    # CEF helper app bundles
    # Each helper is a minimal .app containing a copy of the main executable.
    # CEF discovers them by convention:
    #   Contents/Frameworks/<ExeName> Helper[<suffix>].app/Contents/MacOS/<ExeName> Helper[<suffix>]
    #
    # Helper names contain spaces and parentheses which break shell quoting in
    # add_custom_command COMMAND lists.  We avoid the issue entirely by writing
    # a small CMake script at configure time and executing it via cmake -P at
    # build time.  Generator expressions are expanded via a GENEX_EVAL wrapper.
    # -----------------------------------------------------------------------
    set(_cef_plists_dir "${CMAKE_BINARY_DIR}/cef_helper_plists")

    # JIT entitlement plist (needed on macOS 12+ for V8 to work).
    set(_jit_entitlements "${_cef_plists_dir}/jit.entitlements")
    file(WRITE "${_jit_entitlements}"
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
  <key>com.apple.security.cs.allow-jit</key><true/>
  <key>com.apple.security.cs.allow-unsigned-executable-memory</key><true/>
  <key>com.apple.security.cs.disable-library-validation</key><true/>
</dict>
</plist>")

    # The Screen Recording + Accessibility companion never initializes CEF/V8.
    # Keep its high-privilege process free of JIT and library-validation bypasses.
    set(_computer_use_entitlements "${_cef_plists_dir}/computer-use.entitlements")
    file(WRITE "${_computer_use_entitlements}"
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict/>
</plist>")

    # Write Info.plists and accumulate cmake_script content at configure time.
    set(_helper_script_content
"# Auto-generated by CMakeLists.txt — do not edit.
cmake_minimum_required(VERSION 3.20)
function(sign_owned_code target entitlements)
  execute_process(COMMAND /usr/bin/codesign --force --sign \"${UAM_MACOS_SIGNING_IDENTITY}\" --entitlements \"\${entitlements}\" \"\${target}\"
    RESULT_VARIABLE status ERROR_VARIABLE error)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR \"Failed to sign \${target} with configured identity '${UAM_MACOS_SIGNING_IDENTITY}': \${error}\")
  endif()
endfunction()
function(require_dictation_usage_descriptions plist)
  foreach(key IN ITEMS NSMicrophoneUsageDescription NSSpeechRecognitionUsageDescription NSLocalNetworkUsageDescription)
    execute_process(COMMAND /usr/bin/plutil -extract \"\${key}\" raw -o - \"\${plist}\"
      RESULT_VARIABLE status OUTPUT_VARIABLE description OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT status EQUAL 0 OR description STREQUAL \"\")
      message(FATAL_ERROR \"\${plist} is missing a non-empty \${key}\")
    endif()
  endforeach()
endfunction()
# Remove legacy UI-V2/dist from Contents/MacOS/ — it was moved to Contents/Resources/.
# If left in MacOS/, codesign finds the .woff fonts and produces a broken signature.
file(REMOVE_RECURSE \"\${UAM_BUNDLE_CONTENTS}/MacOS/UI-V2\")
")

    foreach(_hname IN ITEMS
        "universal_agent_manager Helper"
        "universal_agent_manager Helper (GPU)"
        "universal_agent_manager Helper (Plugin)"
        "universal_agent_manager Helper (Renderer)")

      # DNS-safe bundle identifier suffix.
      string(REGEX REPLACE "[() ]" "" _hid "${_hname}")
      string(TOLOWER "${_hid}" _hid)

      # Info.plist written at configure time; script copies it at build time.
      file(WRITE "${_cef_plists_dir}/${_hid}/Info.plist"
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>${_hname}</string>
  <key>CFBundleIdentifier</key><string>com.universalagentmanager.desktop.${_hid}</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>${_hname}</string>
  <key>CFBundleDisplayName</key><string>Universal Agent Manager</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${UAM_PRODUCT_VERSION}</string>
  <key>CFBundleVersion</key><string>${PROJECT_VERSION}</string>
  <key>LSBackgroundOnly</key><true/>
  <key>LSMinimumSystemVersion</key><string>10.15.0</string>
  <key>NSMicrophoneUsageDescription</key><string>Universal Agent Manager uses the microphone when you choose to dictate a prompt.</string>
  <key>NSLocalNetworkUsageDescription</key><string>Universal Agent Manager uses your local network so your phone can connect to this Mac.</string>
  <key>NSSpeechRecognitionUsageDescription</key><string>Universal Agent Manager converts your spoken prompt into text when dictation is active.</string>
</dict>
</plist>")

      # Append to the cmake script that will run POST_BUILD.
      # UAM_BUNDLE_CONTENTS and UAM_MAIN_EXE are passed as -D args at build time.
      string(APPEND _helper_script_content
"
set(_hdir \"\${UAM_BUNDLE_CONTENTS}/Frameworks/${_hname}.app/Contents\")
file(MAKE_DIRECTORY \"\${_hdir}/MacOS\")
file(MAKE_DIRECTORY \"\${_hdir}/Frameworks\")
file(COPY_FILE \"\${UAM_MAIN_EXE}\" \"\${_hdir}/MacOS/${_hname}\" ONLY_IF_DIFFERENT)
file(COPY_FILE \"${_cef_plists_dir}/${_hid}/Info.plist\" \"\${_hdir}/Info.plist\" ONLY_IF_DIFFERENT)
require_dictation_usage_descriptions(\"\${_hdir}/Info.plist\")
# Symlink to the CEF framework in the parent bundle so that
# @executable_path/../Frameworks/Chromium Embedded Framework.framework resolves.
# Relative path: Contents/Frameworks/ -> ../../../ -> Contents/Frameworks/ (parent)
if(NOT EXISTS \"\${_hdir}/Frameworks/Chromium Embedded Framework.framework\")
  file(CREATE_LINK \"../../../Chromium Embedded Framework.framework\"
       \"\${_hdir}/Frameworks/Chromium Embedded Framework.framework\"
       SYMBOLIC)
endif()
sign_owned_code(\"\${UAM_BUNDLE_CONTENTS}/Frameworks/${_hname}.app\" \"${_jit_entitlements}\")
")
    endforeach()

    # Computer Use is a separate UI-element companion so macOS grants its
    # screen-capture and accessibility permissions to the least-privileged process.
    if(UAM_ENABLE_COMPUTER_USE)
    set(_computer_use_name "UAM Computer Use")
    file(WRITE "${_cef_plists_dir}/computer-use-Info.plist"
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>${_computer_use_name}</string>
  <key>CFBundleIdentifier</key><string>com.universalagentmanager.desktop.computer-use</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>${_computer_use_name}</string>
  <key>CFBundleDisplayName</key><string>${_computer_use_name}</string>
  <key>CFBundleIconFile</key><string>app_icon.icns</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${UAM_PRODUCT_VERSION}</string>
  <key>CFBundleVersion</key><string>${PROJECT_VERSION}</string>
  <key>LSUIElement</key><true/>
  <key>LSMinimumSystemVersion</key><string>10.15.0</string>
</dict>
</plist>")
    string(APPEND _helper_script_content
"
set(_computer_use_dir \"\${UAM_BUNDLE_CONTENTS}/Frameworks/${_computer_use_name}.app/Contents\")
file(MAKE_DIRECTORY \"\${_computer_use_dir}/MacOS\")
file(MAKE_DIRECTORY \"\${_computer_use_dir}/Frameworks\")
file(MAKE_DIRECTORY \"\${_computer_use_dir}/Resources\")
file(COPY_FILE \"\${UAM_MAIN_EXE}\" \"\${_computer_use_dir}/MacOS/${_computer_use_name}\" ONLY_IF_DIFFERENT)
file(COPY_FILE \"${_cef_plists_dir}/computer-use-Info.plist\" \"\${_computer_use_dir}/Info.plist\" ONLY_IF_DIFFERENT)
file(COPY_FILE \"${CMAKE_SOURCE_DIR}/assets/computer-use/SoftwareCursor.png\" \"\${_computer_use_dir}/Resources/SoftwareCursor.png\" ONLY_IF_DIFFERENT)
file(COPY_FILE \"${CMAKE_SOURCE_DIR}/assets/app_icon.icns\" \"\${_computer_use_dir}/Resources/app_icon.icns\" ONLY_IF_DIFFERENT)
file(COPY_FILE \"${CMAKE_SOURCE_DIR}/assets/computer-use/gemini-policy.toml\" \"\${_computer_use_dir}/Resources/gemini-policy.toml\" ONLY_IF_DIFFERENT)
if(NOT EXISTS \"\${_computer_use_dir}/Frameworks/Chromium Embedded Framework.framework\")
  file(CREATE_LINK \"../../../Chromium Embedded Framework.framework\"
       \"\${_computer_use_dir}/Frameworks/Chromium Embedded Framework.framework\"
       SYMBOLIC)
endif()
sign_owned_code(\"\${UAM_BUNDLE_CONTENTS}/Frameworks/${_computer_use_name}.app\" \"${_computer_use_entitlements}\")
")

    endif()

    # All nested apps must be signed before codesign inspects the containing app.
    # Sign the main executable binary with the JIT entitlement.
    # We sign only the binary (not the whole bundle) to avoid the sealed-resources
    # error caused by non-code assets (e.g. .woff fonts) inside the bundle.
    string(APPEND _helper_script_content
"require_dictation_usage_descriptions(\"\${UAM_BUNDLE_CONTENTS}/Info.plist\")
sign_owned_code(\"\${UAM_MAIN_EXE}\" \"${_jit_entitlements}\")

")


    # Write the script to the build dir.
    set(_helper_cmake_script "${_cef_plists_dir}/create_helpers.cmake")

    file(WRITE "${_helper_cmake_script}" "${_helper_script_content}")

    # POST_BUILD: run the generated script, passing bundle paths via -D.
    add_custom_command(TARGET universal_agent_manager POST_BUILD
      COMMAND ${CMAKE_COMMAND}
        -DUAM_BUNDLE_CONTENTS=$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>
        -DUAM_BUNDLE_DIR=$<TARGET_BUNDLE_DIR:universal_agent_manager>
        -DUAM_MAIN_EXE=$<TARGET_FILE:universal_agent_manager>
        -P "${_helper_cmake_script}"
      VERBATIM
      COMMENT "Creating and signing CEF helper bundles"
    )

  else()
    add_custom_command(TARGET universal_agent_manager POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CEF_FRAMEWORK_SRC}"
        "$<TARGET_FILE_DIR:universal_agent_manager>/Chromium Embedded Framework.framework"
      COMMENT "Copying CEF framework next to binary"
    )
  endif()
endif()

# Windows: copy CEF runtime DLLs and resources.
if(WIN32)
  set(CEF_RELEASE_DIR "${cef_binary_SOURCE_DIR}/Release")
  set(CEF_RESOURCES_DIR "${cef_binary_SOURCE_DIR}/Resources")

  if(DEFINED CEF_BINARY_FILES)
    set(UAM_CEF_BINARY_FILES ${CEF_BINARY_FILES})
  else()
    set(UAM_CEF_BINARY_FILES
      chrome_elf.dll
      d3dcompiler_47.dll
      libcef.dll
      libEGL.dll
      libGLESv2.dll
      v8_context_snapshot.bin
      vk_swiftshader.dll
      vk_swiftshader_icd.json
      vulkan-1.dll
      dxil.dll
      dxcompiler.dll
    )
  endif()

  foreach(_cef_binary IN LISTS UAM_CEF_BINARY_FILES)
    if(EXISTS "${CEF_RELEASE_DIR}/${_cef_binary}")
      add_custom_command(TARGET universal_agent_manager POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CEF_RELEASE_DIR}/${_cef_binary}"
          "$<TARGET_FILE_DIR:universal_agent_manager>/${_cef_binary}"
      )
    endif()
  endforeach()

  if(DEFINED CEF_RESOURCE_FILES)
    set(UAM_CEF_RESOURCE_FILES ${CEF_RESOURCE_FILES})
  else()
    set(UAM_CEF_RESOURCE_FILES
      chrome_100_percent.pak
      chrome_200_percent.pak
      resources.pak
      icudtl.dat
      locales
    )
  endif()

  foreach(_cef_resource IN LISTS UAM_CEF_RESOURCE_FILES)
    if(IS_DIRECTORY "${CEF_RESOURCES_DIR}/${_cef_resource}")
      add_custom_command(TARGET universal_agent_manager POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CEF_RESOURCES_DIR}/${_cef_resource}"
          "$<TARGET_FILE_DIR:universal_agent_manager>/${_cef_resource}"
        COMMENT "Copying CEF resource directory ${_cef_resource}"
      )
    elseif(EXISTS "${CEF_RESOURCES_DIR}/${_cef_resource}")
      add_custom_command(TARGET universal_agent_manager POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CEF_RESOURCES_DIR}/${_cef_resource}"
          "$<TARGET_FILE_DIR:universal_agent_manager>/${_cef_resource}"
        COMMENT "Copying CEF resource file ${_cef_resource}"
      )
    endif()
  endforeach()
endif()

# Build + copy React UI bundle into the app bundle.
# On macOS: place in Contents/Resources/ so font assets (.woff etc.) don't
#           live beside the binary in Contents/MacOS/, which would cause
#           codesign to scan them as code objects and produce a broken signature.
# On Windows: flat layout next to the executable.
include("${CMAKE_SOURCE_DIR}/cmake/ui_build.cmake")
add_dependencies(universal_agent_manager uam_ui_v2_build)
if(UAM_ENABLE_SSH)
  add_dependencies(universal_agent_manager uam_runner)
endif()

if(APPLE AND UAM_MACOS_BUNDLE)
  set(UAM_REMOTE_ARTIFACT_DEST
    "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/remote")
else()
  set(UAM_REMOTE_ARTIFACT_DEST "$<TARGET_FILE_DIR:universal_agent_manager>/remote")
endif()
set(UAM_REMOTE_ARTIFACT_COPY_COMMAND)
if(UAM_PACKAGE_REMOTE_RUNNERS)
  set(UAM_REMOTE_ARTIFACT_COPY_COMMAND
    COMMAND ${CMAKE_COMMAND}
      "-DUAM_REMOTE_RUNNER_ARTIFACT_DIR=${UAM_REMOTE_RUNNER_ARTIFACT_DIR}"
      "-DUAM_PRODUCT_VERSION=${UAM_PRODUCT_VERSION}"
      -P "${CMAKE_SOURCE_DIR}/cmake/validate_remote_runners.cmake"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${UAM_REMOTE_RUNNER_ARTIFACT_DIR}" "${UAM_REMOTE_ARTIFACT_DEST}")
endif()

if(APPLE AND UAM_MACOS_BUNDLE)
  set(UAM_RESOURCE_DEST "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources")
else()
  set(UAM_RESOURCE_DEST "$<TARGET_FILE_DIR:universal_agent_manager>")
endif()
add_custom_command(TARGET universal_agent_manager POST_BUILD
  COMMAND ${CMAKE_COMMAND}
    "-DUAM_UI_SOURCE=${UAM_UI_DIST_SRC}"
    "-DUAM_UI_DEST=${UAM_RESOURCE_DEST}/UI-V2/dist"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_ui_dist.cmake"
  COMMAND ${CMAKE_COMMAND} -E remove_directory "${UAM_RESOURCE_DEST}/markdown-store"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/markdown-store" "${UAM_RESOURCE_DEST}/markdown-store"
  COMMAND ${CMAKE_COMMAND} -E rm -f "${UAM_RESOURCE_DEST}/markdown-store/skill-builder.uam"
  COMMAND ${CMAKE_COMMAND} -E remove_directory "${UAM_RESOURCE_DEST}/markdown-store/bundled"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/markdown-store/bundled" "${UAM_RESOURCE_DEST}/markdown-store/bundled"
  COMMENT "Copying UI-V2 and bundled Markdown Store entries"
  VERBATIM
)
if(UAM_ENABLE_COMPUTER_USE)
  add_custom_command(TARGET universal_agent_manager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/assets/computer-use/gemini-policy.toml" "${UAM_RESOURCE_DEST}/gemini-policy.toml"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/assets/computer-use/SoftwareCursor.png" "${UAM_RESOURCE_DEST}/SoftwareCursor.png"
    VERBATIM
  )
endif()
if(UAM_ENABLE_SSH)
  add_custom_command(TARGET universal_agent_manager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${UAM_RESOURCE_DEST}/remote"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:uam_runner>" "${UAM_RESOURCE_DEST}/remote/$<TARGET_FILE_NAME:uam_runner>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${UAM_RUNNER_SHA256_FILE}" "${UAM_RESOURCE_DEST}/remote/uam-runner.sha256"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${UAM_RUNNER_VERSION_FILE}" "${UAM_RESOURCE_DEST}/remote/uam-runner.version"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE_DIR:uam_runner>/uam-runner.manifest.json" "${UAM_RESOURCE_DEST}/remote/uam-runner.manifest.json"
    ${UAM_REMOTE_ARTIFACT_COPY_COMMAND}
    VERBATIM
  )
endif()

# ---------------------------------------------------------------------------
# Window icon
# ---------------------------------------------------------------------------

set(UAM_WINDOW_ICON_BMP "${CMAKE_CURRENT_SOURCE_DIR}/assets/app_icon.bmp")
if(APPLE)
  set(UAM_WINDOW_ICON_PNG "${CMAKE_CURRENT_SOURCE_DIR}/assets/app_icon_macos.png")
else()
  set(UAM_WINDOW_ICON_PNG "${CMAKE_CURRENT_SOURCE_DIR}/assets/app_icon.png")
endif()
set(UAM_BUNDLE_ICON_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/assets/app_icon.icns")
if(APPLE AND UAM_MACOS_BUNDLE)
  add_custom_command(TARGET universal_agent_manager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f
      "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/MacOS/app_icon.bmp"
      "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/MacOS/app_icon.png"
    COMMENT "Removing stale executable-directory icon asset"
  )
endif()
if(EXISTS "${UAM_WINDOW_ICON_BMP}")
  if(APPLE AND UAM_MACOS_BUNDLE)
    add_custom_command(TARGET universal_agent_manager POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${UAM_WINDOW_ICON_BMP}"
        "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/app_icon.bmp"
    )
  else()
    add_custom_command(TARGET universal_agent_manager POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${UAM_WINDOW_ICON_BMP}"
        "$<TARGET_FILE_DIR:universal_agent_manager>/app_icon.bmp"
    )
  endif()
endif()
if(EXISTS "${UAM_WINDOW_ICON_PNG}")
  if(APPLE AND UAM_MACOS_BUNDLE)
    add_custom_command(TARGET universal_agent_manager POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${UAM_WINDOW_ICON_PNG}"
        "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/app_icon.png"
    )
  else()
    add_custom_command(TARGET universal_agent_manager POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${UAM_WINDOW_ICON_PNG}"
        "$<TARGET_FILE_DIR:universal_agent_manager>/app_icon.png"
    )
  endif()
endif()
if(APPLE AND UAM_MACOS_BUNDLE AND EXISTS "${UAM_BUNDLE_ICON_ICNS}")
  add_custom_command(TARGET universal_agent_manager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${UAM_BUNDLE_ICON_ICNS}"
      "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/app_icon.icns"
  )
endif()

if(APPLE AND UAM_MACOS_BUNDLE AND UAM_ENABLE_MOBILE_COMPANION)
  find_program(UAM_CADDY_EXECUTABLE NAMES caddy)
  if(UAM_CADDY_EXECUTABLE)
    get_filename_component(_uam_caddy_real "${UAM_CADDY_EXECUTABLE}" REALPATH)
    get_filename_component(_uam_caddy_bin "${_uam_caddy_real}" DIRECTORY)
    find_file(UAM_CADDY_LICENSE NAMES LICENSE PATHS "${_uam_caddy_bin}/.." NO_DEFAULT_PATH REQUIRED)
    add_custom_command(TARGET universal_agent_manager POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/companion"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${UAM_CADDY_EXECUTABLE}" "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/companion/caddy"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${UAM_CADDY_LICENSE}" "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/companion/LICENSE-caddy"
      COMMAND codesign --force --sign "${UAM_MACOS_SIGNING_IDENTITY}" "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/companion/caddy"
      VERBATIM
      COMMENT "Bundling companion HTTPS transport"
    )
  endif()
endif()
if(APPLE AND UAM_MACOS_BUNDLE)
  add_custom_command(TARGET universal_agent_manager POST_BUILD
    COMMAND /bin/sh -c
      "codesign --verify --strict '$<TARGET_BUNDLE_DIR:universal_agent_manager>/Contents/Frameworks/Chromium Embedded Framework.framework' || codesign --force --sign '${UAM_MACOS_SIGNING_IDENTITY}' '$<TARGET_BUNDLE_DIR:universal_agent_manager>/Contents/Frameworks/Chromium Embedded Framework.framework'"
    COMMAND codesign --force --sign "${UAM_MACOS_SIGNING_IDENTITY}" --entitlements "${_jit_entitlements}"
      "$<TARGET_BUNDLE_DIR:universal_agent_manager>"
    COMMAND codesign --verify --deep --strict --verbose=2
      "$<TARGET_BUNDLE_DIR:universal_agent_manager>"
    VERBATIM
    COMMENT "Signing and verifying final macOS app bundle"
  )
endif()

include("${CMAKE_SOURCE_DIR}/cmake/package_inputs.cmake")

foreach(_disabled_feature IN ITEMS COMPUTER_USE SSH MOBILE_COMPANION)
  if(NOT UAM_ENABLE_${_disabled_feature})
    if(APPLE AND UAM_MACOS_BUNDLE)
      set(_feature_root "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>")
      if(_disabled_feature STREQUAL "COMPUTER_USE")
        set(_feature_artifacts "${_feature_root}/Frameworks/UAM Computer Use.app" "${_feature_root}/Resources/SoftwareCursor.png" "${_feature_root}/Resources/gemini-policy.toml")
      elseif(_disabled_feature STREQUAL "SSH")
        set(_feature_artifacts "${_feature_root}/Resources/remote")
      else()
        set(_feature_artifacts "${_feature_root}/Resources/companion")
      endif()
    else()
      set(_feature_root "$<TARGET_FILE_DIR:universal_agent_manager>")
      if(_disabled_feature STREQUAL "COMPUTER_USE")
        set(_feature_artifacts "${_feature_root}/SoftwareCursor.png" "${_feature_root}/gemini-policy.toml")
      elseif(_disabled_feature STREQUAL "SSH")
        set(_feature_artifacts "${_feature_root}/remote")
      else()
        set(_feature_artifacts "${_feature_root}/companion")
      endif()
    endif()
    add_custom_command(TARGET universal_agent_manager PRE_LINK COMMAND ${CMAKE_COMMAND} -E rm -rf ${_feature_artifacts} VERBATIM)
  endif()
endforeach()

file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/uam-build-manifest-$<CONFIG>.txt"
  CONTENT "COMPUTER_USE=${UAM_ENABLE_COMPUTER_USE}\nSSH=${UAM_ENABLE_SSH}\nMOBILE_COMPANION=${UAM_ENABLE_MOBILE_COMPANION}\nSOURCES\n$<JOIN:$<TARGET_GENEX_EVAL:universal_agent_manager,$<TARGET_PROPERTY:universal_agent_manager,SOURCES>>,\n>\n")

# ---------------------------------------------------------------------------
# Platform-specific settings
# ---------------------------------------------------------------------------

if(MSVC)
  set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT universal_agent_manager)
endif()

if(APPLE)
  target_compile_definitions(universal_agent_manager PRIVATE GL_SILENCE_DEPRECATION)
  if(UAM_MACOS_BUNDLE)
    set_target_properties(universal_agent_manager PROPERTIES
      MACOSX_BUNDLE_BUNDLE_NAME "Universal Agent Manager"
      MACOSX_BUNDLE_BUNDLE_VERSION "${UAM_PRESENTATION_VERSION}"
      MACOSX_BUNDLE_GUI_IDENTIFIER "com.universalagentmanager.desktop"
      MACOSX_BUNDLE_ICON_FILE "app_icon.icns"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${UAM_PRESENTATION_VERSION}"
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/Info.plist.in"
    )
  endif()
endif()

foreach(_cmake_target IN ITEMS ALL_BUILD ZERO_CHECK RUN_TESTS Experimental Nightly NightlyMemoryCheck Continuous)
  uam_set_target_folder(${_cmake_target} "_CMake")
endforeach()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(UAM_BUILD_TESTS)
  enable_testing()
  add_executable(uam_companion_protocol_tests
    tests/companion_protocol_tests.cpp src/cef/uam_bridge_request.cpp)
  target_include_directories(uam_companion_protocol_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(uam_companion_protocol_tests PRIVATE nlohmann_json::nlohmann_json)
  target_compile_definitions(uam_companion_protocol_tests PRIVATE
    UAM_ENABLE_RUNTIME_GEMINI_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_GEMINI_CLI}>
    UAM_ENABLE_RUNTIME_CODEX_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CODEX_CLI}>
    UAM_ENABLE_RUNTIME_CLAUDE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CLAUDE_CLI}>
    UAM_ENABLE_RUNTIME_OPENCODE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_OPENCODE_CLI}>
    UAM_ENABLE_RUNTIME_COPILOT_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_COPILOT_CLI}>
    UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI}>)
  set_target_properties(uam_companion_protocol_tests PROPERTIES FOLDER "Tests")
  foreach(companion_case IN ITEMS
      TransferRoundTripCommitOnce TransferMalformedOffsets TransferStrictBase64AndBounds
      InFlightRequestAdmissionSurvivesAsyncOwnership ResponseTransferAggregateBoundsAndFinalRelease ResponseTransferTimerExpiryAndIdentity
      TransferOversizeCapacityAndRelease TransferAbsoluteExpiry TransferIdCollision TransferEnvelopeFieldValidation
      PolicyCapabilitiesAndRestrictions PolicyScopeSelectionAndReadOnly PolicySettingsRedaction
      PolicyProviderActionsCapabilitiesAndReadOnly PolicyProviderActionsRejectMalformedOrPrivilegedFields
      PolicyProviderTransferredActionsUseFinalPolicy
      PolicyByteAttachmentStaging AttachmentDescriptorOwnership TransferredEnvelopeFinalAllowlist
      PolicyQueueStableIdentityAndHeldState PolicyQueueAdmissionTransitionAndStaleIdentity
      PolicyQueueRequiresBoundedIdentity PolicyQueueExistingEditAndReorderOwnership
      SettingsProviderDefaultsPartialAndScoped SettingsMemoryAndWorkerPartialUpdates SettingsMemoryFenceAndOutcomeProjection SettingsPreferencesStorageAndAvailability
      SettingsRejectPrivilegedAndMalformedUpdates SettingsMcpAndNestedProjectionRedaction SettingsTransferredEnvelopeUsesFinalPolicy)
    add_test(NAME uam_companion_${companion_case} COMMAND uam_companion_protocol_tests ${companion_case})
  endforeach()
  add_executable(uam_ui_asset_snapshot_tests tests/ui_asset_snapshot_tests.cpp)
  target_include_directories(uam_ui_asset_snapshot_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(uam_ui_asset_snapshot_tests PRIVATE nlohmann_json::nlohmann_json)
  add_test(NAME uam_ui_asset_snapshot COMMAND uam_ui_asset_snapshot_tests)
  add_executable(uam_crash_supervisor_tests
    tests/app_crash_supervisor_tests.cpp
    src/common/platform/app_crash_supervisor.cpp)
  target_include_directories(uam_crash_supervisor_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
  foreach(crash_case normal once loop term nonzero bypass restart restart_many)
    add_test(NAME uam_crash_supervisor_${crash_case} COMMAND uam_crash_supervisor_tests ${crash_case})
  endforeach()

  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME uam_runner_binary_stdio
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/runner_binary_stdio_test.py" "$<TARGET_FILE:uam_runner>")
  set_tests_properties(uam_runner_binary_stdio PROPERTIES TIMEOUT 40)
  add_test(NAME uam_local_build_version
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/build-configurator/test_local_build_version.py")
  add_test(NAME uam_ui_build_incremental
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ui_build_incremental_test.py")
  if(APPLE)
    add_test(NAME uam_macos_helper_signing
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/macos_helper_signing_test.py"
        --cmake "${CMAKE_COMMAND}" --compiler "${CMAKE_C_COMPILER}")
    set_tests_properties(uam_macos_helper_signing PROPERTIES TIMEOUT 120)
  endif()
  add_test(NAME uam_native_test_runner
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/native_test_runner_test.py")
  foreach(parent_stop terminate kill)
    add_test(NAME uam_crash_supervisor_parent_${parent_stop}
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/app_crash_supervisor_parent_test.py" "$<TARGET_FILE:uam_crash_supervisor_tests>" "${parent_stop}")
  endforeach()
  add_executable(uam_companion_connection_tests tests/companion_connection_lifecycle_tests.cpp)
  target_include_directories(uam_companion_connection_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(uam_companion_connection_tests PRIVATE nlohmann_json::nlohmann_json)
  add_test(NAME uam_companion_connection_lifecycle COMMAND uam_companion_connection_tests)
  add_executable(uam_core_tests
    tests/core_tests.cpp
    tests/input_provenance_native_refresh_tests.cpp
    tests/acp_input_admission_identity_tests.cpp
    tests/async_byte_writer_receipt_tests.cpp
    tests/platform_observed_input_write_tests.cpp
    tests/input_provenance_codec_tests.cpp
    tests/acp_input_admission_bridge_tests.cpp
    tests/chat_branch_selection_tests.cpp
    tests/companion_lifecycle_tests.cpp
    tests/host_scoped_settings_tests.cpp
    tests/repository_route_match_tests.cpp
    tests/deletion_progress_tests.cpp
    tests/manual_compaction_startup_tests.cpp
    tests/runner_startup_observation_tests.cpp
    tests/runner_endpoint_identity_tests.cpp
    tests/provider_model_metadata_tests.cpp
    tests/claude_model_metadata_tests.cpp
    tests/svn_commit_scheduling_tests.cpp
    tests/native_import_failure_tests.cpp
    tests/empty_native_import_tests.cpp
    tests/opencode_import_stream_tests.cpp
    tests/local_chat_bundle_tests.cpp
    tests/antigravity_tests.cpp
    tests/structured_framing_tests.cpp
    tests/structured_attachment_tests.cpp
    tests/remote_attachment_tests.cpp
    tests/claude_native_compaction_tests.cpp
    tests/native_history_refresh_tests.cpp
    tests/managed_subagent_control_tests.cpp
    tests/managed_subagent_followup_tests.cpp
    tests/agent_result_delivery_regression_tests.cpp
    tests/local_input_write_observation_tests.cpp
    tests/input_write_carrier_mutation_tests.cpp
    tests/gemini_history_journal_tests.cpp
    tests/native_child_lineage_tests.cpp
    tests/gemini_native_assets_tests.cpp
    tests/native_attachment_assets_tests.cpp
    tests/native_media_chat_save_tests.cpp
    tests/branch_native_assets_tests.cpp
    tests/chat_birth_preservation_tests.cpp
    tests/gemini_discovered_media_import_tests.cpp
    tests/native_refresh_lifetime_tests.cpp
    tests/codex_native_history_projection_tests.cpp
    tests/codex_native_children_tests.cpp
    tests/native_agent_reference_tests.cpp
    tests/native_provider_agent_history_tests.cpp
    tests/native_remote_history_refresh_tests.cpp
    tests/copilot_native_protocol_tests.cpp
    tests/opencode_native_http_tests.cpp
    tests/opencode_v2_integration_tests.cpp
    tests/opencode_native_activity_tests.cpp
    tests/opencode_terminal_activity_tests.cpp
    tests/antigravity_mcp_tests.cpp
    tests/startup_native_import_tests.cpp
    tests/queued_prompt_tests.cpp
    tests/acp_workspace_launch_tests.cpp
    tests/queued_prompt_hold_tests.cpp
    tests/agent_target_metadata_tests.cpp
    tests/turn_finalization_ownership_tests.cpp
    tests/checkpoint_staging_tests.cpp
    tests/codex_allowance_ownership_tests.cpp
    tests/codex_permission_tests.cpp
    tests/memory_workspace_ownership_tests.cpp
    tests/execution_routing_tests.cpp
    tests/routing_default_scope_tests.cpp
    tests/model_discovery_startup_tests.cpp
    tests/remote_delivery_exit_tests.cpp
    tests/agent_provider_inheritance_tests.cpp
    tests/workspace_import_policy_tests.cpp
    tests/workspace_migration_alias_tests.cpp
    tests/deletion_workspace_ownership_tests.cpp
    tests/copied_deletion_timing_tests.cpp
		tests/remote_context_policy_tests.cpp
		tests/remote_host_inline_instruction_tests.cpp
		tests/memory_recall_preparation_tests.cpp
    tests/native_chat_identity_tests.cpp
    tests/chat_file_change_receipt_tests.cpp
    tests/organization_tests.cpp
    tests/transcript_retention_tests.cpp
    tests/chat_search_tests.cpp
    tests/interaction_performance_tests.cpp
    tests/chat_message_page_tests.cpp
    tests/chat_publication_tests.cpp
    tests/provider_configuration_tests.cpp
    tests/transcript_usage_tests.cpp
    tests/antigravity_accounting_tests.cpp
    tests/antigravity_history_scan_tests.cpp
    tests/transcript_scan_tests.cpp
    tests/usage_history_service_tests.cpp
    tests/usage_history_transport_tests.cpp
    tests/chat_icon_mutation_tests.cpp
    tests/workspace_chat_save_ticket_tests.cpp
    tests/chat_icon_metadata_tests.cpp
    tests/usage_ledger_tests.cpp
    tests/usage_store_tests.cpp
    tests/usage_collector_tests.cpp
    tests/multi_host_history_tests.cpp
    tests/account_allowance_fetcher_tests.cpp
    tests/account_allowance_service_tests.cpp
    tests/google_account_allowance_tests.cpp
    tests/claude_account_allowance_tests.cpp
    tests/chat_title_tests.cpp
    tests/vcs_settings_tests.cpp
    tests/chat_snooze_tests.cpp
    tests/workspace_file_preview_tests.cpp
    tests/workspace_lease_tests.cpp
    tests/worktree_retention_policy_tests.cpp
    tests/git_worktree_defaults_tests.cpp
    tests/git_worktree_chat_base_override_tests.cpp
    tests/workspace_vcs_context_tests.cpp
    tests/git_worktree_pull_tests.cpp
    tests/git_worktree_port_method_tests.cpp
    tests/storage_retention_tests.cpp
    tests/worktree_launch_preparation_tests.cpp
    tests/usage_pricing_tests.cpp
    tests/usage_pricing_cache_tests.cpp
    tests/provider_mcp_settings_tests.cpp
    tests/voice_input_tests.cpp
    tests/provider_availability_tests.cpp
    tests/async_prompt_durability_tests.cpp
    tests/attached_skill_discovery_tests.cpp
    tests/uam_control_transport_tests.cpp
    tests/uam_control_mailbox_response_tests.cpp
    src/common/utils/diagnostic_log.cpp
    tests/attachment_preview_tests.cpp
	    tests/computer_use_tests.cpp
    tests/computer_use_global_policy_tests.cpp
    tests/remote_runner_tests.cpp
    tests/runner_startup_rollback_tests.cpp
    tests/remote_cli_elevation_tests.cpp
    tests/runner_retirement_tests.cpp
    tests/memory_tests.cpp
    tests/memory_policy_tests.cpp
    tests/memory_write_receipt_tests.cpp
    tests/settings_memory_publication_tests.cpp
    tests/remote_memory_publication_tests.cpp
    tests/acp_session_tests.cpp
    tests/acp_permission_ceiling_tests.cpp
    tests/acp_permission_provider_boundary_tests.cpp
    tests/codex_captured_permission_tests.cpp
    tests/acp_permission_projection_tests.cpp
    tests/acp_permission_replay_boundary_tests.cpp
    tests/acp_permission_command_provenance_tests.cpp
    tests/copilot_captured_question_replay_tests.cpp
    tests/read_only_command_tests.cpp
    tests/compaction_runtime_sync_tests.cpp
    tests/installer_quiesce_tests.cpp
    tests/durable_retry_worktree_tests.cpp
    tests/goal_workspace_admission_tests.cpp
    tests/opencode_compaction_sync_tests.cpp
    tests/opencode_compaction_metadata_tests.cpp
    tests/opencode_compaction_history_tests.cpp
    tests/compaction_handoff_tests.cpp
    tests/remote_worktree_tests.cpp
    tests/remote_path_inspection_tests.cpp
    tests/claude_projection_tests.cpp
    tests/claude_usage_tests.cpp
    tests/gemini_usage_tests.cpp
    tests/opencode_usage_tests.cpp
    tests/state_workspace_snapshot_tests.cpp
    tests/terminal_tests.cpp
    tests/deleted_active_native_refresh_tests.cpp
    tests/sidechat_context_tests.cpp
    tests/goal_tests.cpp
    tests/provider_child_activity_tests.cpp
    tests/theme_tests.cpp
    tests/resource_collection_tests.cpp
    src/app/attachment_preview_service.cpp
    ${UAM_ATTACHMENT_PREVIEW_PLATFORM}
    ${UAM_PLATFORM_IMPL_SOURCES}
	    ${UAM_COMPUTER_USE_PLATFORM_SOURCES}
	    src/computer_use/computer_use_mcp_config.cpp
	    src/computer_use/computer_use_mcp_server.cpp
	    src/app/agent_definition_service.cpp
	    src/app/agent_run_ledger.cpp
	    src/app/agent_run_scheduler.cpp
	  src/app/managed_parent_continuation_service.cpp
	    src/app/managed_subagent_control_service.cpp
	    src/app/managed_subagent_followup_service.cpp
	    src/app/uam_control_service.cpp
    src/app/chat_domain_service.cpp
    src/app/chat_lifecycle_service.cpp
    src/app/computer_use_service.cpp
    src/app/git_worktree_service.cpp
    src/app/worktree_retention_service.cpp
    src/app/storage_retention_service.cpp
    src/app/worktree_launch_preparation_service.cpp
    src/app/local_chat_bundle_service.cpp
    src/app/markdown_store_service.cpp
    src/common/memory/memory_library_store.cpp
  src/app/memory_library_service.cpp
    src/app/memory_service.cpp
    src/app/goal_service.cpp
    src/app/native_session_link_service.cpp
    src/app/persistence_coordinator.cpp
    src/app/provider_resolution_service.cpp
    src/app/chat_file_change_receipt_service.cpp
    src/app/organization_service.cpp
    src/app/transcript_retention_service.cpp
    src/app/chat_search_service.cpp
    src/app/execution_routing_service.cpp
  src/app/repository_route_match_service.cpp
    src/app/execution_routing_integration.cpp
    src/app/resource_collection_service.cpp
  src/app/provider_worker_command.cpp
    src/app/chat_title_service.cpp
    src/app/provider_profile_migration_service.cpp
    src/app/provider_model_catalog_service.cpp
  src/app/copilot_agent_history_service.cpp
  src/app/native_agent_history_service.cpp
  src/app/opencode_native_activity_service.cpp
  src/common/provider/opencode/cli/opencode_terminal_activity.cpp
    src/app/runtime_orchestration_services.cpp
    src/app/startup_native_import_service.cpp
    src/app/opencode_compaction_sync_service.cpp
  src/app/opencode_compaction_history.cpp
    src/app/model_discovery_service.cpp
    src/app/shell_action_service.cpp
    src/app/theme_service.cpp
    src/app/vcs_commit_service.cpp
  src/app/workspace_vcs_context_service.cpp
    src/cef/uam_cef_command_line_config.cpp
    src/cef/uam_bridge_request.cpp
    src/cef/cef_push.cpp
    src/cef/state_serializer.cpp
    src/common/models/app_models.cpp
    src/common/paths/app_paths.cpp
    src/common/chat/chat_folder_store.cpp
    src/common/config/frontend_actions.cpp
    src/common/utils/command_line_words.cpp
    src/common/chat/chat_repository.cpp
  src/common/chat/native_media_chat_save.cpp
  src/common/chat/branch_native_assets.cpp
    src/common/chat/chat_message_page_index.cpp
    src/common/chat/chat_branching.cpp
    src/common/chat/native_chat_identity.cpp
    src/common/usage/transcript_usage.cpp
    src/common/usage/antigravity_accounting.cpp
    src/common/usage/antigravity_history_scan.cpp
    src/common/usage/usage_aggregation.cpp
    src/common/usage/transcript_scan.cpp
    src/common/usage/opencode_history_scan.cpp
    src/common/usage/usage_history_service.cpp
    src/common/usage/usage_ledger.cpp
    src/common/usage/usage_store.cpp
    src/common/usage/usage_collector.cpp
    src/common/usage/multi_host_history.cpp
    src/common/usage/account_allowance_fetcher.cpp
    src/common/usage/account_allowance_service.cpp
    src/common/usage/account_credentials_macos.cpp
    src/common/usage/google_account_allowance.cpp
    src/common/usage/claude_account_allowance.cpp
    src/common/usage/usage_pricing.cpp
    src/common/usage/usage_pricing_cache.cpp
    src/common/provider/provider_profile.cpp
    src/common/provider/runtime/provider_runtime_facade.cpp
    src/common/provider/runtime/provider_runtime_registry.cpp
    src/common/provider/runtime/provider_acp_strategy.cpp
  src/common/provider/structured_attachments.cpp
  src/common/provider/opencode/cli/opencode_native_http.cpp
    src/common/provider/claude/cli/claude_acp_message_handlers.cpp
    src/common/provider/codex/cli/codex_acp_message_handlers.cpp
    src/common/runtime/acp/acp_content_text.cpp
    src/common/runtime/acp/acp_diagnostics.cpp
    src/common/runtime/acp/acp_goal_helpers.cpp
    src/common/runtime/acp/acp_goal_loop.cpp
    src/common/runtime/acp/acp_message_sync.cpp
    src/common/runtime/acp/acp_permission_handlers.cpp
    src/common/runtime/acp/acp_polling.cpp
    src/common/runtime/acp/acp_replay_state.cpp
    src/common/runtime/acp/acp_response_handlers.cpp
    src/common/runtime/acp/acp_session_lifecycle.cpp
    src/common/runtime/acp/acp_session_update_handler.cpp
    src/common/runtime/acp/acp_tool_call_state.cpp
    src/common/runtime/acp/acp_turn_events.cpp
    src/common/runtime/acp/acp_json_helpers.cpp
    src/common/runtime/acp/acp_launch.cpp
    src/common/runtime/acp/acp_message_detail.cpp
    src/common/runtime/acp/acp_request_builders.cpp
    src/common/runtime/acp/acp_resume_rules.cpp
  src/common/runtime/acp/claude_quota_continuation.cpp
    src/common/provider/attached_skill_resources.cpp
    src/common/runtime/acp/acp_session_runtime.cpp
    src/common/runtime/acp/acp_wait_state.cpp
    src/common/runtime/json_runtime.cpp
    src/common/runtime/terminal_polling.cpp
    src/common/runtime/terminal/terminal_chat_sync.cpp
    src/common/runtime/terminal/terminal_debug_diagnostics.cpp
    src/common/runtime/terminal/terminal_provider_cli.cpp
    src/common/runtime/terminal/terminal_idle_classifier.cpp
    src/common/runtime/terminal/terminal_identity.cpp
    src/common/runtime/terminal/terminal_lifecycle.cpp
    src/common/runtime/provider_cli_compatibility_service.cpp
    src/core/chat_import_utils.cpp
    src/app/opencode_import_stream.cpp
    src/core/gemini_cli_compat.cpp
    ${UAM_PROVIDER_SOURCES}
    src/common/config/settings_store.cpp
    src/remote/runner_protocol.cpp
    src/remote/runner_bootstrap.cpp
    src/remote/runner_client.cpp
    src/remote/runner_proxy.cpp
    src/remote/runner_state.cpp
    src/common/platform/platform_services.cpp
  )
  set_target_properties(uam_core_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
  )

  target_link_libraries(uam_core_tests PRIVATE uam_sqlite)
  target_include_directories(uam_core_tests
    PRIVATE
      src
      src/common
      ${cef_binary_SOURCE_DIR}
  )
  if(APPLE)
    target_sources(uam_core_tests PRIVATE src/remote/runner_service_posix.cpp)
    target_link_libraries(uam_core_tests PRIVATE
      nlohmann_json::nlohmann_json
      libcef_dll_wrapper
      "-Wl,-framework,Security"
      "-Wl,-framework,AppKit"
      "-Wl,-framework,IOKit"
      "-Wl,-framework,CoreFoundation"
      "-Wl,-framework,AVFoundation"
      "-Wl,-framework,Foundation"
      "-Wl,-framework,Speech"
      "-Wl,-framework,ApplicationServices"
      "-Wl,-framework,Carbon"
      "-Wl,-framework,ImageIO"
      "-Wl,-framework,CoreGraphics"
      "-Wl,-framework,QuartzCore"
      "-Wl,-weak_framework,ScreenCaptureKit"
    )
  elseif(WIN32)
    target_sources(uam_core_tests PRIVATE src/remote/runner_service_windows.cpp)
    target_link_libraries(uam_core_tests PRIVATE
      nlohmann_json::nlohmann_json
      libcef_dll_wrapper
      "${cef_binary_SOURCE_DIR}/Release/libcef.lib"
      advapi32
      shell32
      windowsapp
      gdi32
      ole32
      uuid
      oleaut32
      user32
      uiautomationcore
      windowscodecs
      ws2_32
    )
    add_custom_command(TARGET uam_core_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${cef_binary_SOURCE_DIR}/Release/libcef.dll"
        "$<TARGET_FILE_DIR:uam_core_tests>/libcef.dll"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${cef_binary_SOURCE_DIR}/Release/chrome_elf.dll"
        "$<TARGET_FILE_DIR:uam_core_tests>/chrome_elf.dll"
    )
  endif()
  target_compile_definitions(uam_core_tests PRIVATE
  ${UAM_FEATURE_DEFINITIONS}
    NOMINMAX
    UAM_ENABLE_RUNTIME_GEMINI_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_GEMINI_CLI}>
    UAM_ENABLE_RUNTIME_CODEX_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CODEX_CLI}>
    UAM_ENABLE_RUNTIME_CLAUDE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CLAUDE_CLI}>
    UAM_ENABLE_RUNTIME_OPENCODE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_OPENCODE_CLI}>
    UAM_ENABLE_RUNTIME_COPILOT_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_COPILOT_CLI}>
    UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI}>
  )
  if(MSVC)
    target_compile_options(uam_core_tests PRIVATE /bigobj)
    set_property(TARGET uam_core_tests PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    set_target_properties(uam_core_tests PROPERTIES
      VS_GLOBAL_VcpkgEnabled "false"
      VS_GLOBAL_VcpkgApplocalDeps "false"
    )
  endif()
  set_target_properties(uam_core_tests PROPERTIES FOLDER "Tests")
  add_dependencies(uam_core_tests uam_runner)
  add_test(NAME uam_core_tests COMMAND uam_core_tests)
  add_custom_target(uam_check_core
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/run_native_tests.py"
      "$<TARGET_FILE:uam_core_tests>" --jobs 2
    DEPENDS uam_core_tests
    USES_TERMINAL
    COMMENT "Running the complete native inventory in two isolated processes")

  # Explicitly built connected bridge fixtures use the real handler/router sources.
  # This target inherits compile/link settings only, never app packaging or startup.
  get_target_property(UAM_ICON_HANDLER_TEST_SOURCES universal_agent_manager SOURCES)
  list(REMOVE_ITEM UAM_ICON_HANDLER_TEST_SOURCES src/main.cpp "${CMAKE_BINARY_DIR}/uam-package-inputs-$<CONFIG>.cpp")
  set(UAM_CONNECTED_HANDLER_TEST_SOURCES ${UAM_ICON_HANDLER_TEST_SOURCES})
  list(REMOVE_ITEM UAM_CONNECTED_HANDLER_TEST_SOURCES
    src/common/platform/platform_pty_macos.cpp src/common/platform/platform_pty_windows.cpp)
  add_executable(uam_chat_icon_handler_tests EXCLUDE_FROM_ALL
    tests/chat_icon_handler_tests.cpp tests/companion_connection_handler_tests.cpp tests/memory_settings_handler_tests.cpp tests/computer_use_terminal_runtime_fixture.cpp ${UAM_CONNECTED_HANDLER_TEST_SOURCES})
  set_target_properties(uam_chat_icon_handler_tests PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF FOLDER "Tests")
  foreach(UAM_ICON_TEST_PROPERTY IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_LIBRARIES LINK_OPTIONS LINK_FLAGS MSVC_RUNTIME_LIBRARY)
    get_target_property(UAM_ICON_TEST_VALUE universal_agent_manager ${UAM_ICON_TEST_PROPERTY})
    if(NOT UAM_ICON_TEST_VALUE STREQUAL "UAM_ICON_TEST_VALUE-NOTFOUND")
      set_property(TARGET uam_chat_icon_handler_tests PROPERTY ${UAM_ICON_TEST_PROPERTY} "${UAM_ICON_TEST_VALUE}")
    endif()
  endforeach()
  target_include_directories(uam_chat_icon_handler_tests PRIVATE tests)
  if(APPLE)
    # CEF uses @executable_path/../Frameworks; link immutable fixtures without copying it.
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/icon-handler-test-framework-$<CONFIG>.cmake" CONTENT
      "set(destination \"$<TARGET_FILE_DIR:uam_chat_icon_handler_tests>/../Frameworks/Chromium Embedded Framework.framework\")
if(NOT EXISTS \"\${destination}\")
  file(MAKE_DIRECTORY \"$<TARGET_FILE_DIR:uam_chat_icon_handler_tests>/../Frameworks\")
  file(CREATE_LINK \"${cef_binary_SOURCE_DIR}/Release/Chromium Embedded Framework.framework\" \"\${destination}\" SYMBOLIC RESULT result)
  if(NOT result STREQUAL \"0\")
    message(FATAL_ERROR \"Cannot link the connected test CEF framework: \${result}\")
  endif()
endif()
")
    add_custom_command(TARGET uam_chat_icon_handler_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/icon-handler-test-framework-$<CONFIG>.cmake")
  elseif(WIN32)
    add_custom_command(TARGET uam_chat_icon_handler_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${cef_binary_SOURCE_DIR}/Release/libcef.dll" "$<TARGET_FILE_DIR:uam_chat_icon_handler_tests>/libcef.dll"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${cef_binary_SOURCE_DIR}/Release/chrome_elf.dll" "$<TARGET_FILE_DIR:uam_chat_icon_handler_tests>/chrome_elf.dll")
  endif()
  # Actual goal bridge timing fixtures keep their three-case inventory separate.
  add_executable(uam_goal_handler_timing_tests EXCLUDE_FROM_ALL
    tests/goal_handler_timing_tests.cpp ${UAM_ICON_HANDLER_TEST_SOURCES})
  set_target_properties(uam_goal_handler_timing_tests PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF FOLDER "Tests")
  foreach(UAM_GOAL_TEST_PROPERTY IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_LIBRARIES LINK_OPTIONS LINK_FLAGS MSVC_RUNTIME_LIBRARY)
    get_target_property(UAM_GOAL_TEST_VALUE universal_agent_manager ${UAM_GOAL_TEST_PROPERTY})
    if(NOT UAM_GOAL_TEST_VALUE STREQUAL "UAM_GOAL_TEST_VALUE-NOTFOUND")
      set_property(TARGET uam_goal_handler_timing_tests PROPERTY ${UAM_GOAL_TEST_PROPERTY} "${UAM_GOAL_TEST_VALUE}")
    endif()
  endforeach()
  target_include_directories(uam_goal_handler_timing_tests PRIVATE tests)
  if(APPLE)
    add_custom_command(TARGET uam_goal_handler_timing_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/icon-handler-test-framework-$<CONFIG>.cmake")
  elseif(WIN32)
    add_custom_command(TARGET uam_goal_handler_timing_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${cef_binary_SOURCE_DIR}/Release/libcef.dll" "$<TARGET_FILE_DIR:uam_goal_handler_timing_tests>/libcef.dll"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${cef_binary_SOURCE_DIR}/Release/chrome_elf.dll" "$<TARGET_FILE_DIR:uam_goal_handler_timing_tests>/chrome_elf.dll")
  endif()
  # Keep this opt-in executable outside the standard CTest inventory until built.

  add_executable(uam_managed_subagent_handler_tests EXCLUDE_FROM_ALL
    tests/managed_subagent_handler_tests.cpp tests/native_media_refresh_handler_tests.cpp tests/native_model_options_handler_tests.cpp ${UAM_ICON_HANDLER_TEST_SOURCES})
  set_target_properties(uam_managed_subagent_handler_tests PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF FOLDER "Tests")
  foreach(UAM_MANAGED_TEST_PROPERTY IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_LIBRARIES LINK_OPTIONS LINK_FLAGS MSVC_RUNTIME_LIBRARY)
    get_target_property(UAM_MANAGED_TEST_VALUE universal_agent_manager ${UAM_MANAGED_TEST_PROPERTY})
    if(NOT UAM_MANAGED_TEST_VALUE STREQUAL "UAM_MANAGED_TEST_VALUE-NOTFOUND")
      set_property(TARGET uam_managed_subagent_handler_tests PROPERTY ${UAM_MANAGED_TEST_PROPERTY} "${UAM_MANAGED_TEST_VALUE}")
    endif()
  endforeach()
  target_include_directories(uam_managed_subagent_handler_tests PRIVATE tests)
  if(APPLE)
    add_custom_command(TARGET uam_managed_subagent_handler_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/icon-handler-test-framework-$<CONFIG>.cmake")
  elseif(WIN32)
    add_custom_command(TARGET uam_managed_subagent_handler_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${cef_binary_SOURCE_DIR}/Release/libcef.dll" "$<TARGET_FILE_DIR:uam_managed_subagent_handler_tests>/libcef.dll"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${cef_binary_SOURCE_DIR}/Release/chrome_elf.dll" "$<TARGET_FILE_DIR:uam_managed_subagent_handler_tests>/chrome_elf.dll")
  endif()
  # Keep this opt-in executable outside the standard CTest inventory until built.

  if(APPLE)
    # macOS Unix sockets have a 104-byte path limit; the default TMPDIR can exceed it.
    set_tests_properties(uam_core_tests PROPERTIES ENVIRONMENT "TMPDIR=/tmp")
  endif()
  add_test(NAME uam_remote_runner_validation
    COMMAND ${CMAKE_COMMAND} -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
      -P ${CMAKE_SOURCE_DIR}/cmake/check_remote_runner_validation.cmake)

  if(APPLE)
    add_executable(uam_macos_application_tests
      tests/macos_application_tests.mm
      src/common/platform/platform_application_macos.mm
    )
    target_include_directories(uam_macos_application_tests PRIVATE src ${cef_binary_SOURCE_DIR})
    target_link_libraries(uam_macos_application_tests PRIVATE
      "-Wl,-framework,AppKit"
      "-Wl,-framework,AVFoundation"
      "-Wl,-framework,Speech"
    )
    add_test(NAME uam_macos_application_tests COMMAND uam_macos_application_tests)
  endif()
  add_test(
    NAME uam_platform_ifdef_guard
    COMMAND ${CMAKE_COMMAND} -DSOURCE_ROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/cmake/check_platform_ifdefs.cmake
  )
  add_test(
    NAME uam_provider_terminal_boundary
    COMMAND ${CMAKE_COMMAND} -DSOURCE_ROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/cmake/check_provider_terminal_boundary.cmake
  )
  add_test(
    NAME uam_windows_process_containment_contract
    COMMAND ${CMAKE_COMMAND} -DSOURCE_ROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/cmake/check_windows_process_containment.cmake
  )
  add_test(
    NAME uam_macos_process_spawn_contract
    COMMAND ${CMAKE_COMMAND} -DSOURCE_ROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/cmake/check_macos_process_spawn_contract.cmake
  )
  add_test(
    NAME uam_computer_use_permission_contract
    COMMAND ${CMAKE_COMMAND} -DSOURCE_ROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/cmake/check_computer_use_permission_contract.cmake
  )
endif()

if(UAM_BUILD_TESTS)
  add_executable(uam_management_discovery_tests
    tests/management_discovery_tests.cpp
    src/common/discovery/management_discovery.cpp)
  target_include_directories(uam_management_discovery_tests PRIVATE src)
  target_compile_features(uam_management_discovery_tests PRIVATE cxx_std_20)
  if(WIN32)
    target_link_libraries(uam_management_discovery_tests PRIVATE dnsapi)
  endif()
  add_test(NAME uam_management_discovery_validation COMMAND uam_management_discovery_tests)
endif()

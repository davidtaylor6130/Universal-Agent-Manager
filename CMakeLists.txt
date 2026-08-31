cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
project(universal_agent_manager VERSION 4.8.0 LANGUAGES C CXX)
set(UAM_PRODUCT_VERSION "4.8.0")

option(UAM_RUNNER_ONLY "Build only the headless remote runner." OFF)
option(UAM_PACKAGE_REMOTE_RUNNERS "Validate and package all supported SSH helpers." ON)
set(UAM_REMOTE_RUNNER_ARTIFACT_DIR "${CMAKE_SOURCE_DIR}/Builds/remote-artifacts" CACHE PATH
  "Directory containing linux-arm64, linux-x86_64, and windows-x86_64 runner folders.")

if(APPLE)
  enable_language(OBJCXX)
endif()

if(NOT WIN32 AND NOT APPLE AND NOT UAM_RUNNER_ONLY)
  message(FATAL_ERROR "universal_agent_manager supports only Windows and macOS builds.")
endif()

if(NOT UAM_RUNNER_ONLY AND UAM_PACKAGE_REMOTE_RUNNERS)
  set(UAM_REQUIRED_REMOTE_RUNNERS
    "linux-arm64/uam-runner"
    "linux-x86_64/uam-runner"
    "windows-x86_64/uam-runner.exe"
  )
  foreach(UAM_REMOTE_RUNNER IN LISTS UAM_REQUIRED_REMOTE_RUNNERS)
    cmake_path(GET UAM_REMOTE_RUNNER PARENT_PATH UAM_REMOTE_RUNNER_DIR)
    set(UAM_REMOTE_RUNNER_PATH
      "${UAM_REMOTE_RUNNER_ARTIFACT_DIR}/${UAM_REMOTE_RUNNER}")
    set(UAM_REMOTE_RUNNER_SHA256_PATH
      "${UAM_REMOTE_RUNNER_ARTIFACT_DIR}/${UAM_REMOTE_RUNNER_DIR}/uam-runner.sha256")
    set(UAM_REMOTE_RUNNER_VERSION_PATH
      "${UAM_REMOTE_RUNNER_ARTIFACT_DIR}/${UAM_REMOTE_RUNNER_DIR}/uam-runner.version")
    foreach(UAM_REMOTE_RUNNER_FILE
        "${UAM_REMOTE_RUNNER_PATH}"
        "${UAM_REMOTE_RUNNER_SHA256_PATH}"
        "${UAM_REMOTE_RUNNER_VERSION_PATH}")
      if(NOT EXISTS "${UAM_REMOTE_RUNNER_FILE}")
        message(FATAL_ERROR
          "Full app builds require every remote helper. Missing: ${UAM_REMOTE_RUNNER_FILE}")
      endif()
    endforeach()
    file(READ "${UAM_REMOTE_RUNNER_VERSION_PATH}" UAM_REMOTE_RUNNER_VERSION)
    string(STRIP "${UAM_REMOTE_RUNNER_VERSION}" UAM_REMOTE_RUNNER_VERSION)
    if(NOT UAM_REMOTE_RUNNER_VERSION STREQUAL UAM_PRODUCT_VERSION)
      message(FATAL_ERROR
        "Remote helper version mismatch for ${UAM_REMOTE_RUNNER}: "
        "expected ${UAM_PRODUCT_VERSION}, got ${UAM_REMOTE_RUNNER_VERSION}")
    endif()
    file(READ "${UAM_REMOTE_RUNNER_SHA256_PATH}" UAM_REMOTE_RUNNER_EXPECTED_SHA256)
    string(STRIP "${UAM_REMOTE_RUNNER_EXPECTED_SHA256}" UAM_REMOTE_RUNNER_EXPECTED_SHA256)
    file(SHA256 "${UAM_REMOTE_RUNNER_PATH}" UAM_REMOTE_RUNNER_ACTUAL_SHA256)
    if(NOT UAM_REMOTE_RUNNER_ACTUAL_SHA256 STREQUAL UAM_REMOTE_RUNNER_EXPECTED_SHA256)
      message(FATAL_ERROR "Remote helper checksum mismatch: ${UAM_REMOTE_RUNNER}")
    endif()
  endforeach()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_SUPPRESS_REGENERATION ON)

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

foreach(_removed_runtime_flag IN ITEMS
  UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED
  UAM_ENABLE_RUNTIME_OPENCODE_LOCAL
  UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
  UAM_ENABLE_ENGINE_RAG
)
  if(DEFINED ${_removed_runtime_flag})
    message(FATAL_ERROR
      "${_removed_runtime_flag} is no longer supported in this release slice. "
      "Only UAM_ENABLE_RUNTIME_GEMINI_CLI, UAM_ENABLE_RUNTIME_CODEX_CLI, UAM_ENABLE_RUNTIME_CLAUDE_CLI, UAM_ENABLE_RUNTIME_OPENCODE_CLI, and UAM_ENABLE_RUNTIME_COPILOT_CLI are supported."
    )
  endif()
endforeach()

if(NOT UAM_ENABLE_RUNTIME_GEMINI_CLI AND NOT UAM_ENABLE_RUNTIME_CODEX_CLI AND NOT UAM_ENABLE_RUNTIME_CLAUDE_CLI AND NOT UAM_ENABLE_RUNTIME_OPENCODE_CLI AND NOT UAM_ENABLE_RUNTIME_COPILOT_CLI)
  message(FATAL_ERROR "At least one provider runtime must be enabled.")
endif()

set(UAM_FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH
  "Directory used for FetchContent source checkouts.")
if(APPLE)
  option(UAM_MACOS_BUNDLE "Build as a macOS .app bundle (no Terminal window needed)." ON)
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
  )
endif()
if(UAM_ENABLE_RUNTIME_OPENCODE_CLI)
  list(APPEND UAM_PROVIDER_SOURCES
    src/common/provider/opencode/cli/opencode_cli_provider_runtime.cpp
  )
endif()
if(UAM_ENABLE_RUNTIME_COPILOT_CLI)
  list(APPEND UAM_PROVIDER_SOURCES
    src/common/provider/copilot/cli/copilot_cli_provider_runtime.cpp
  )
endif()

# ---------------------------------------------------------------------------
# Platform sources
# ---------------------------------------------------------------------------

set(UAM_PLATFORM_SOURCES)
set(UAM_PLATFORM_IMPL_SOURCES)
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
  )
  list(APPEND UAM_COMPUTER_USE_PLATFORM_SOURCES
    src/computer_use/computer_use_platform_macos.mm
  )
  set_source_files_properties(
    src/common/platform/platform_dictation_macos.mm
    src/computer_use/computer_use_platform_macos.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc"
  )
endif()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------

add_executable(uam_runner
  src/remote/runner_main.cpp
  src/remote/runner_client.cpp
  src/remote/runner_protocol.cpp
  src/remote/runner_proxy.cpp
  src/remote/runner_state.cpp
)
set_target_properties(uam_runner PROPERTIES
  OUTPUT_NAME "uam-runner"
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  FOLDER "Remote"
)
target_include_directories(uam_runner PRIVATE src)
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
  target_link_libraries(uam_runner PRIVATE advapi32 shell32 user32)
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
  NOMINMAX
  UAM_REMOTE_RUNNER_VERSION="${UAM_PRODUCT_VERSION}"
  UAM_HEADLESS_RUNNER=1
  UAM_ENABLE_RUNTIME_GEMINI_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_GEMINI_CLI}>
  UAM_ENABLE_RUNTIME_CODEX_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CODEX_CLI}>
  UAM_ENABLE_RUNTIME_CLAUDE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CLAUDE_CLI}>
  UAM_ENABLE_RUNTIME_OPENCODE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_OPENCODE_CLI}>
  UAM_ENABLE_RUNTIME_COPILOT_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_COPILOT_CLI}>
)
set(UAM_RUNNER_SHA256_FILE "$<TARGET_FILE_DIR:uam_runner>/uam-runner.sha256")
set(UAM_RUNNER_VERSION_FILE "$<TARGET_FILE_DIR:uam_runner>/uam-runner.version")
add_custom_command(TARGET uam_runner POST_BUILD
  COMMAND ${CMAKE_COMMAND}
    -DINPUT=$<TARGET_FILE:uam_runner>
    -DOUTPUT=${UAM_RUNNER_SHA256_FILE}
    -DVERSION=${UAM_PRODUCT_VERSION}
    -DVERSION_OUTPUT=${UAM_RUNNER_VERSION_FILE}
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
  ${UAM_PLATFORM_IMPL_SOURCES}
  ${UAM_COMPUTER_USE_PLATFORM_SOURCES}
  src/main.cpp
	  src/computer_use/computer_use_mcp_config.cpp
	  src/computer_use/computer_use_mcp_server.cpp
	  src/app/agent_definition_service.cpp
	  src/app/agent_run_ledger.cpp
	  src/app/agent_run_scheduler.cpp
	  src/app/uam_control_service.cpp
  src/app/application.cpp
  src/app/chat_domain_service.cpp
  src/app/chat_lifecycle_service.cpp
  src/app/computer_use_service.cpp
  src/app/git_worktree_service.cpp
  src/app/local_chat_bundle_service.cpp
  src/app/markdown_store_service.cpp
  src/app/memory_library_service.cpp
  src/app/memory_service.cpp
  src/app/goal_service.cpp
  src/app/native_session_link_service.cpp
  src/app/persistence_coordinator.cpp
  src/app/provider_resolution_service.cpp
  src/app/resource_collection_service.cpp
    src/app/provider_worker_command.cpp
  src/app/provider_profile_migration_service.cpp
  src/app/provider_model_catalog_service.cpp
  src/app/runtime_orchestration_services.cpp
  src/app/shell_action_service.cpp
  src/app/theme_service.cpp
  src/app/pending_call_service.cpp
  src/app/vcs_commit_service.cpp
  src/cef/uam_cef_app.cpp
  src/cef/uam_cef_command_line_config.cpp
  src/cef/uam_cef_client.cpp
  src/cef/uam_bridge_request.cpp
  src/cef/uam_query_handler.cpp
  src/cef/state_serializer.cpp
  src/cef/cef_push.cpp
  src/cef/chat_handlers.cpp
  src/cef/chat_config_handlers.cpp
  src/cef/settings_handlers.cpp
  src/cef/remote_host_handlers.cpp
  src/cef/local_chat_bundle_handlers.cpp
  src/cef/theme_handlers.cpp
  src/cef/shell_action_handlers.cpp
  src/cef/markdown_handlers.cpp
  src/cef/folder_handlers.cpp
  src/cef/resource_collection_handlers.cpp
  src/cef/memory_handlers.cpp
  src/cef/workspace_handlers.cpp
  src/cef/vcs_handlers.cpp
  src/cef/terminal_handlers.cpp
  src/cef/dictation_handlers.cpp
  src/cef/acp_handlers.cpp
  src/cef/goal_handlers.cpp
  src/common/models/app_models.cpp
  src/common/paths/app_paths.cpp
  src/common/chat/chat_folder_store.cpp
  src/common/chat/chat_repository.cpp
  src/common/chat/chat_branching.cpp
  src/common/chat/native_chat_identity.cpp
  src/core/chat_import_utils.cpp
  src/common/config/settings_store.cpp
  src/common/utils/command_line_words.cpp
  src/common/config/frontend_actions.cpp
  src/common/provider/provider_profile.cpp
  src/common/provider/runtime/provider_runtime_facade.cpp
  src/common/provider/runtime/provider_runtime_registry.cpp
  src/common/provider/runtime/provider_acp_strategy.cpp
  src/common/runtime/acp/acp_claude_message_handlers.cpp
  src/common/runtime/acp/acp_codex_message_handlers.cpp
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
    "-Wl,-framework,ApplicationServices"
    "-Wl,-framework,Carbon"
    "-Wl,-framework,ImageIO"
    "-Wl,-framework,QuartzCore"
    "-Wl,-weak_framework,ScreenCaptureKit"
  )
elseif(WIN32)
  target_link_libraries(universal_agent_manager PRIVATE
    nlohmann_json::nlohmann_json
    libcef_dll_wrapper
    "${cef_binary_SOURCE_DIR}/Release/libcef.lib"
    advapi32
    shell32
    windowsapp
    gdi32
    ole32
    oleaut32
    user32
    uiautomationcore
    windowscodecs
  )
endif()

target_compile_definitions(universal_agent_manager PRIVATE
  NOMINMAX
  UAM_REMOTE_RUNNER_VERSION="${UAM_PRODUCT_VERSION}"
  UAM_ENABLE_RUNTIME_GEMINI_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_GEMINI_CLI}>
  UAM_ENABLE_RUNTIME_CODEX_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CODEX_CLI}>
  UAM_ENABLE_RUNTIME_CLAUDE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CLAUDE_CLI}>
  UAM_ENABLE_RUNTIME_OPENCODE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_OPENCODE_CLI}>
  UAM_ENABLE_RUNTIME_COPILOT_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_COPILOT_CLI}>
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
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CEF_FRAMEWORK_SRC}"
        "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Frameworks/Chromium Embedded Framework.framework"
      COMMENT "Copying CEF framework into .app bundle"
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
function(require_dictation_usage_descriptions plist)
  foreach(key IN ITEMS NSMicrophoneUsageDescription NSSpeechRecognitionUsageDescription)
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
execute_process(COMMAND codesign --force --sign - --entitlements \"${_jit_entitlements}\"
  \"\${UAM_BUNDLE_CONTENTS}/Frameworks/${_hname}.app\")
")
    endforeach()

    # Computer Use is a separate UI-element companion so macOS grants its
    # screen-capture and accessibility permissions to the least-privileged process.
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
file(COPY_FILE \"${CMAKE_SOURCE_DIR}/assets/computer-use/gemini-policy.toml\" \"\${_computer_use_dir}/Resources/gemini-policy.toml\" ONLY_IF_DIFFERENT)
if(NOT EXISTS \"\${_computer_use_dir}/Frameworks/Chromium Embedded Framework.framework\")
  file(CREATE_LINK \"../../../Chromium Embedded Framework.framework\"
       \"\${_computer_use_dir}/Frameworks/Chromium Embedded Framework.framework\"
       SYMBOLIC)
endif()
")

    # Sign the main executable binary with the JIT entitlement.
    # We sign only the binary (not the whole bundle) to avoid the sealed-resources
    # error caused by non-code assets (e.g. .woff fonts) inside the bundle.
    string(APPEND _helper_script_content
"require_dictation_usage_descriptions(\"\${UAM_BUNDLE_CONTENTS}/Info.plist\")
execute_process(COMMAND codesign --force --sign - --entitlements \"${_jit_entitlements}\"
  \"\${UAM_MAIN_EXE}\")
execute_process(COMMAND codesign --force --sign - --entitlements \"${_computer_use_entitlements}\"
  \"\${UAM_BUNDLE_CONTENTS}/Frameworks/${_computer_use_name}.app\"
  RESULT_VARIABLE _computer_use_sign_result)
if(NOT _computer_use_sign_result EQUAL 0)
  message(FATAL_ERROR \"Failed to sign the Computer Use companion\")
endif()
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
      COMMENT "Creating CEF helper bundles and ad-hoc signing"
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
set(UAM_UI_DIST_SRC "${CMAKE_BINARY_DIR}/UI-V2/dist")
if(WIN32)
  unset(UAM_NPM_EXECUTABLE CACHE)
  find_program(UAM_NPM_EXECUTABLE NAMES npm.cmd REQUIRED)
else()
  find_program(UAM_NPM_EXECUTABLE NAMES npm REQUIRED)
endif()
string(TIMESTAMP UAM_UI_BUILD_TIMESTAMP "%Y%m%dT%H%M%SZ" UTC)
set(UAM_UI_BUILD_VERSION "${UAM_PRODUCT_VERSION}")
if(UAM_UI_BUILD_VERSION STREQUAL "")
  set(UAM_UI_BUILD_VERSION "local")
endif()
set(UAM_UI_BUILD_ID "${UAM_UI_BUILD_VERSION}-${CMAKE_SYSTEM_NAME}-${UAM_UI_BUILD_TIMESTAMP}")

add_custom_target(uam_ui_v2_build
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/UI-V2"
  COMMAND ${CMAKE_COMMAND} -E env "VITE_UAM_UI_BUILD_ID=${UAM_UI_BUILD_ID}" "VITE_UAM_OUT_DIR=${UAM_UI_DIST_SRC}" ${UAM_NPM_EXECUTABLE} run build
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/UI-V2"
  COMMENT "Building UI-V2 dist in ${UAM_UI_DIST_SRC} (build id: ${UAM_UI_BUILD_ID})"
  VERBATIM
)
add_dependencies(universal_agent_manager uam_ui_v2_build)
add_dependencies(universal_agent_manager uam_runner)
set_property(TARGET universal_agent_manager APPEND PROPERTY LINK_DEPENDS "${UAM_UI_DIST_SRC}/index.html")

if(APPLE AND UAM_MACOS_BUNDLE)
  set(UAM_REMOTE_ARTIFACT_DEST
    "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/remote")
else()
  set(UAM_REMOTE_ARTIFACT_DEST "$<TARGET_FILE_DIR:universal_agent_manager>/remote")
endif()
set(UAM_REMOTE_ARTIFACT_COPY_COMMAND)
if(UAM_PACKAGE_REMOTE_RUNNERS)
  set(UAM_REMOTE_ARTIFACT_COPY_COMMAND
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${UAM_REMOTE_RUNNER_ARTIFACT_DIR}" "${UAM_REMOTE_ARTIFACT_DEST}")
endif()

if(APPLE AND UAM_MACOS_BUNDLE)
  add_custom_command(TARGET universal_agent_manager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove_directory
      "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/UI-V2/dist"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${UAM_UI_DIST_SRC}"
      "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/UI-V2/dist"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
      "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/markdown-store"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/markdown-store"
      "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/markdown-store"
    COMMAND ${CMAKE_COMMAND} -E rm -f
      "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/markdown-store/skill-builder.uam"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
      "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/markdown-store/bundled"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/markdown-store/bundled"
      "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/markdown-store/bundled"
    COMMAND ${CMAKE_COMMAND} -E echo "Bundled UI-V2 build id: ${UAM_UI_BUILD_ID}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/computer-use/gemini-policy.toml"
      "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/gemini-policy.toml"
    COMMAND ${CMAKE_COMMAND} -E make_directory
      "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/remote"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:uam_runner>"
      "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/remote/uam-runner"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${UAM_RUNNER_SHA256_FILE}"
      "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/remote/uam-runner.sha256"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
	  "${UAM_RUNNER_VERSION_FILE}"
	  "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/remote/uam-runner.version"
	${UAM_REMOTE_ARTIFACT_COPY_COMMAND}
    COMMENT "Copying UI-V2/dist and bundled Markdown Store entries to Contents/Resources/"
  )
else()
  add_custom_command(TARGET universal_agent_manager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove_directory
      "$<TARGET_FILE_DIR:universal_agent_manager>/UI-V2/dist"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${UAM_UI_DIST_SRC}"
      "$<TARGET_FILE_DIR:universal_agent_manager>/UI-V2/dist"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
      "$<TARGET_FILE_DIR:universal_agent_manager>/markdown-store"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/markdown-store"
      "$<TARGET_FILE_DIR:universal_agent_manager>/markdown-store"
    COMMAND ${CMAKE_COMMAND} -E rm -f
      "$<TARGET_FILE_DIR:universal_agent_manager>/markdown-store/skill-builder.uam"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
      "$<TARGET_FILE_DIR:universal_agent_manager>/markdown-store/bundled"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/markdown-store/bundled"
      "$<TARGET_FILE_DIR:universal_agent_manager>/markdown-store/bundled"
    COMMAND ${CMAKE_COMMAND} -E echo "Bundled UI-V2 build id: ${UAM_UI_BUILD_ID}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/computer-use/gemini-policy.toml"
      "$<TARGET_FILE_DIR:universal_agent_manager>/gemini-policy.toml"
	COMMAND ${CMAKE_COMMAND} -E make_directory
	  "$<TARGET_FILE_DIR:universal_agent_manager>/remote"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
	  "$<TARGET_FILE:uam_runner>"
	  "$<TARGET_FILE_DIR:universal_agent_manager>/remote/$<TARGET_FILE_NAME:uam_runner>"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
	  "${UAM_RUNNER_SHA256_FILE}"
	  "$<TARGET_FILE_DIR:universal_agent_manager>/remote/uam-runner.sha256"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
	  "${UAM_RUNNER_VERSION_FILE}"
	  "$<TARGET_FILE_DIR:universal_agent_manager>/remote/uam-runner.version"
	${UAM_REMOTE_ARTIFACT_COPY_COMMAND}
    COMMENT "Copying UI-V2/dist and bundled Markdown Store entries to build output"
  )
endif()

# ---------------------------------------------------------------------------
# Window icon
# ---------------------------------------------------------------------------

set(UAM_WINDOW_ICON_BMP "${CMAKE_CURRENT_SOURCE_DIR}/assets/app_icon.bmp")
set(UAM_WINDOW_ICON_PNG "${CMAKE_CURRENT_SOURCE_DIR}/assets/app_icon.png")
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

if(APPLE AND UAM_MACOS_BUNDLE)
  add_custom_command(TARGET universal_agent_manager POST_BUILD
    COMMAND codesign --force --sign -
      "$<TARGET_BUNDLE_DIR:universal_agent_manager>/Contents/Frameworks/Chromium Embedded Framework.framework"
    COMMAND codesign --force --sign - --entitlements "${_jit_entitlements}"
      "$<TARGET_BUNDLE_DIR:universal_agent_manager>"
    COMMAND codesign --verify --deep --strict --verbose=2
      "$<TARGET_BUNDLE_DIR:universal_agent_manager>"
    COMMENT "Signing and verifying final macOS app bundle"
  )
endif()

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
      MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
      MACOSX_BUNDLE_GUI_IDENTIFIER "com.universalagentmanager.desktop"
      MACOSX_BUNDLE_ICON_FILE "app_icon.icns"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${UAM_PRODUCT_VERSION}"
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
  add_executable(uam_core_tests
    tests/core_tests.cpp
	    tests/computer_use_tests.cpp
    tests/remote_runner_tests.cpp
    tests/memory_tests.cpp
    tests/acp_session_tests.cpp
    tests/terminal_tests.cpp
    tests/goal_tests.cpp
    tests/theme_tests.cpp
    tests/resource_collection_tests.cpp
    ${UAM_PLATFORM_IMPL_SOURCES}
	    ${UAM_COMPUTER_USE_PLATFORM_SOURCES}
	    src/computer_use/computer_use_mcp_config.cpp
	    src/computer_use/computer_use_mcp_server.cpp
	    src/app/agent_definition_service.cpp
	    src/app/agent_run_ledger.cpp
	    src/app/agent_run_scheduler.cpp
	    src/app/uam_control_service.cpp
    src/app/chat_domain_service.cpp
    src/app/chat_lifecycle_service.cpp
    src/app/computer_use_service.cpp
    src/app/git_worktree_service.cpp
    src/app/local_chat_bundle_service.cpp
    src/app/markdown_store_service.cpp
    src/app/memory_library_service.cpp
    src/app/memory_service.cpp
    src/app/goal_service.cpp
    src/app/native_session_link_service.cpp
    src/app/persistence_coordinator.cpp
    src/app/provider_resolution_service.cpp
    src/app/resource_collection_service.cpp
  src/app/provider_worker_command.cpp
    src/app/provider_profile_migration_service.cpp
    src/app/provider_model_catalog_service.cpp
    src/app/runtime_orchestration_services.cpp
    src/app/shell_action_service.cpp
    src/app/theme_service.cpp
    src/app/pending_call_service.cpp
    src/app/vcs_commit_service.cpp
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
    src/common/chat/chat_branching.cpp
    src/common/chat/native_chat_identity.cpp
    src/common/provider/provider_profile.cpp
    src/common/provider/runtime/provider_runtime_facade.cpp
    src/common/provider/runtime/provider_runtime_registry.cpp
    src/common/provider/runtime/provider_acp_strategy.cpp
    src/common/runtime/acp/acp_claude_message_handlers.cpp
    src/common/runtime/acp/acp_codex_message_handlers.cpp
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

  target_include_directories(uam_core_tests
    PRIVATE
      src
      src/common
      ${cef_binary_SOURCE_DIR}
  )
  if(APPLE)
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
      "-Wl,-framework,QuartzCore"
      "-Wl,-weak_framework,ScreenCaptureKit"
    )
  elseif(WIN32)
    target_link_libraries(uam_core_tests PRIVATE
      nlohmann_json::nlohmann_json
      libcef_dll_wrapper
      "${cef_binary_SOURCE_DIR}/Release/libcef.lib"
      advapi32
      shell32
      windowsapp
      gdi32
      ole32
      oleaut32
      user32
      uiautomationcore
      windowscodecs
    )
    add_custom_command(TARGET uam_core_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${cef_binary_SOURCE_DIR}/Release/libcef.dll"
        "$<TARGET_FILE_DIR:uam_core_tests>/libcef.dll"
    )
  endif()
  target_compile_definitions(uam_core_tests PRIVATE
    NOMINMAX
    UAM_ENABLE_RUNTIME_GEMINI_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_GEMINI_CLI}>
    UAM_ENABLE_RUNTIME_CODEX_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CODEX_CLI}>
    UAM_ENABLE_RUNTIME_CLAUDE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CLAUDE_CLI}>
    UAM_ENABLE_RUNTIME_OPENCODE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_OPENCODE_CLI}>
    UAM_ENABLE_RUNTIME_COPILOT_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_COPILOT_CLI}>
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
  if(APPLE)
    add_executable(uam_macos_application_tests
      tests/macos_application_tests.mm
      src/common/platform/platform_application_macos.mm
    )
    target_include_directories(uam_macos_application_tests PRIVATE src ${cef_binary_SOURCE_DIR})
    target_link_libraries(uam_macos_application_tests PRIVATE "-Wl,-framework,AppKit")
    add_test(NAME uam_macos_application_tests COMMAND uam_macos_application_tests)
  endif()
  add_test(
    NAME uam_platform_ifdef_guard
    COMMAND ${CMAKE_COMMAND} -DSOURCE_ROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/cmake/check_platform_ifdefs.cmake
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

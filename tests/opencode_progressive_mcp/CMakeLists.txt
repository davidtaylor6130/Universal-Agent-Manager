cmake_minimum_required(VERSION 3.20)
project(uam_opencode_progressive_mcp_checks LANGUAGES CXX)

# Bounded data-only validation: no CEF, desktop, runner or provider process target.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(UAM_PACKAGE_REMOTE_RUNNERS "Package remote runners" OFF)
if(UAM_PACKAGE_REMOTE_RUNNERS)
    message(FATAL_ERROR "This data-only target does not package remote runners.")
endif()
get_filename_component(UAM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
file(REAL_PATH "${CMAKE_BINARY_DIR}" UAM_CHECK_BINARY_DIR)
if(NOT UAM_CHECK_BINARY_DIR MATCHES "^${UAM_ROOT}/Builds/")
    message(FATAL_ERROR "Configure this target inside the checkout's Builds directory.")
endif()

include(FetchContent)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

add_executable(uam_opencode_progressive_mcp_checks
    safe_tests.cpp
    "${UAM_ROOT}/src/common/config/settings_store.cpp")
target_include_directories(uam_opencode_progressive_mcp_checks PRIVATE "${UAM_ROOT}/src")
target_link_libraries(uam_opencode_progressive_mcp_checks PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(uam_opencode_progressive_mcp_checks PRIVATE NOMINMAX
    UAM_ENABLE_COMPUTER_USE=1 UAM_ENABLE_SSH=1 UAM_ENABLE_MOBILE_COMPANION=1
    UAM_ENABLE_RUNTIME_GEMINI_CLI=1 UAM_ENABLE_RUNTIME_CODEX_CLI=1
    UAM_ENABLE_RUNTIME_CLAUDE_CLI=1 UAM_ENABLE_RUNTIME_OPENCODE_CLI=1
    UAM_ENABLE_RUNTIME_COPILOT_CLI=1 UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI=1)
enable_testing()
add_test(NAME opencode_progressive_mcp_safe COMMAND uam_opencode_progressive_mcp_checks)

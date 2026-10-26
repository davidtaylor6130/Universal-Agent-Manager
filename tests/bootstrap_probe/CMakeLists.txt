cmake_minimum_required(VERSION 3.20)
project(uam_bootstrap_probe_tests LANGUAGES CXX)

option(UAM_PACKAGE_REMOTE_RUNNERS "Remote packaging is unavailable in the isolated probe target." OFF)
if(UAM_PACKAGE_REMOTE_RUNNERS)
  message(FATAL_ERROR "Bootstrap probe tests require UAM_PACKAGE_REMOTE_RUNNERS=OFF.")
endif()

get_filename_component(UAM_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
file(REAL_PATH "${CMAKE_BINARY_DIR}" UAM_BINARY_ROOT)
file(RELATIVE_PATH UAM_BUILD_RELATIVE "${UAM_SOURCE_ROOT}/Builds" "${UAM_BINARY_ROOT}")
if(UAM_BUILD_RELATIVE MATCHES "^\\.\\.")
  message(FATAL_ERROR "Configure bootstrap probe tests inside the checkout's Builds directory.")
endif()

# Only header-only JSON is needed; no CEF, provider, helper or OS process service is built.
include(FetchContent)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

add_executable(uam_bootstrap_probe_tests main.cpp "${UAM_SOURCE_ROOT}/src/remote/runner_bootstrap.cpp")
target_compile_features(uam_bootstrap_probe_tests PRIVATE cxx_std_20)
target_compile_definitions(uam_bootstrap_probe_tests PRIVATE
  UAM_ENABLE_RUNTIME_GEMINI_CLI=1 UAM_ENABLE_RUNTIME_CODEX_CLI=1
  UAM_ENABLE_RUNTIME_CLAUDE_CLI=1 UAM_ENABLE_RUNTIME_OPENCODE_CLI=1
  UAM_ENABLE_RUNTIME_COPILOT_CLI=1 UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI=1
  UAM_ENABLE_COMPUTER_USE=1 UAM_ENABLE_SSH=1 UAM_ENABLE_MOBILE_COMPANION=1)
set_target_properties(uam_bootstrap_probe_tests PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(uam_bootstrap_probe_tests PRIVATE "${UAM_SOURCE_ROOT}/src" "${UAM_SOURCE_ROOT}/tests")
target_link_libraries(uam_bootstrap_probe_tests PRIVATE nlohmann_json::nlohmann_json)
enable_testing()
add_test(NAME bootstrap_probe_regressions COMMAND uam_bootstrap_probe_tests)

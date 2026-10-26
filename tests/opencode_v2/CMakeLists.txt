cmake_minimum_required(VERSION 3.20)
project(uam_opencode_v2_fixtures LANGUAGES CXX)
option(UAM_PACKAGE_REMOTE_RUNNERS "Not built by this focused fixture project" OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(UAM_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
file(REAL_PATH "${CMAKE_BINARY_DIR}" UAM_FIXTURE_BUILD)
string(FIND "${UAM_FIXTURE_BUILD}" "${UAM_SOURCE_ROOT}/Builds/" UAM_BUILD_PREFIX)
if(NOT UAM_BUILD_PREFIX EQUAL 0)
  message(FATAL_ERROR "Use the task's existing Builds/<task> folder.")
endif()
include(FetchContent)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)
add_executable(uam_opencode_v2_tests
  opencode_v2_tests.cpp
  transport_fixture.cpp
  ../../src/common/provider/opencode/v2/opencode_v2_session.cpp
  ../../src/common/provider/opencode/v2/opencode_v2_policy.cpp)
target_include_directories(uam_opencode_v2_tests PRIVATE ../../src)
target_link_libraries(uam_opencode_v2_tests PRIVATE nlohmann_json::nlohmann_json)
# Exercise the production transport with injected process/HTTP controls, without
# building the desktop/helper or launching a process, provider or socket.
add_library(uam_opencode_v2_transport OBJECT ../../src/common/provider/opencode/v2/opencode_v2_bridge.cpp)
target_include_directories(uam_opencode_v2_transport PRIVATE ../../src)
target_link_libraries(uam_opencode_v2_transport PRIVATE nlohmann_json::nlohmann_json)
target_sources(uam_opencode_v2_tests PRIVATE $<TARGET_OBJECTS:uam_opencode_v2_transport>)
target_compile_definitions(uam_opencode_v2_transport PRIVATE NOMINMAX)
target_compile_definitions(uam_opencode_v2_tests PRIVATE NOMINMAX)
foreach(provider GEMINI CODEX CLAUDE OPENCODE COPILOT ANTIGRAVITY)
  target_compile_definitions(uam_opencode_v2_transport PRIVATE UAM_ENABLE_RUNTIME_${provider}_CLI=1)
  target_compile_definitions(uam_opencode_v2_tests PRIVATE UAM_ENABLE_RUNTIME_${provider}_CLI=1)
endforeach()
enable_testing()
add_test(NAME uam_opencode_v2_mocked_native COMMAND uam_opencode_v2_tests)

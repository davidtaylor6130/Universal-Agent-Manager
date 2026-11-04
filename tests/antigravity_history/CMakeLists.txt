cmake_minimum_required(VERSION 3.20)
project(uam_antigravity_history_fixtures LANGUAGES C CXX)
# Focused non-process validation without CEF binaries, desktop/helper compilation or a
# provider installation. The ordinary uam_core_tests target includes these same
# fixtures plus the application import/publication regressions.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(UAM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(UAM_BUILDS_ROOT "${UAM_ROOT}/Builds")
cmake_path(IS_PREFIX UAM_BUILDS_ROOT "${CMAKE_BINARY_DIR}" NORMALIZE UAM_BUILD_INSIDE_ROOT)
get_filename_component(UAM_BUILD_BASENAME "${CMAKE_BINARY_DIR}" NAME)
if(NOT UAM_BUILD_INSIDE_ROOT AND NOT UAM_BUILD_BASENAME MATCHES "^cmake-build-")
  message(FATAL_ERROR "Use Builds/task-1630/parser for focused native-history validation.")
endif()
option(UAM_PACKAGE_REMOTE_RUNNERS "Runner packaging is unavailable in this focused fixture project." OFF)
if(UAM_PACKAGE_REMOTE_RUNNERS)
  message(FATAL_ERROR "Focused history fixtures cannot configure or package helpers.")
endif()
include(FetchContent)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)
include("${UAM_ROOT}/cmake/antigravity_history_crypto.cmake")
# Match the application's pinned SQLite and extension policy, rather than
# silently validating only against the operating system's older library.
FetchContent_Declare(uam_sqlite
  URL https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip
  URL_HASH SHA256=68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(uam_sqlite)
add_library(uam_sqlite STATIC "${uam_sqlite_SOURCE_DIR}/sqlite3.c")
target_include_directories(uam_sqlite PUBLIC "${uam_sqlite_SOURCE_DIR}")
target_compile_definitions(uam_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)
if(MSVC)
  set_property(TARGET uam_sqlite PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
add_executable(uam_antigravity_history_tests
  main.cpp ../antigravity_native_import_tests.cpp
  "${UAM_ROOT}/src/common/provider/antigravity/cli/antigravity_native_history.cpp"
  "${UAM_ROOT}/src/common/platform/private_temp_directory.cpp"
  "${UAM_ROOT}/src/common/platform/platform_native_attachment_assets.cpp")
set(UAM_HISTORY_PROVIDER_DEFINITIONS
  UAM_ENABLE_RUNTIME_GEMINI_CLI=1 UAM_ENABLE_RUNTIME_CODEX_CLI=1
  UAM_ENABLE_RUNTIME_CLAUDE_CLI=1 UAM_ENABLE_RUNTIME_OPENCODE_CLI=1
  UAM_ENABLE_RUNTIME_COPILOT_CLI=1 UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI=1)
target_compile_definitions(uam_antigravity_history_tests PRIVATE
  UAM_ANTIGRAVITY_HISTORY_STANDALONE=1 ${UAM_HISTORY_PROVIDER_DEFINITIONS})
target_include_directories(uam_antigravity_history_tests PRIVATE "${UAM_ROOT}/src")
target_link_libraries(uam_antigravity_history_tests PRIVATE uam_sqlite nlohmann_json::nlohmann_json uam_antigravity_history_crypto)
# Only the pure inline payload path is used from platform_native_attachment_assets;
# unused publication paths belong to the ordinary application tests.
if(APPLE)
  target_link_options(uam_antigravity_history_tests PRIVATE -Wl,-dead_strip)
elseif(WIN32)
  target_link_libraries(uam_antigravity_history_tests PRIVATE advapi32)
  target_compile_options(uam_antigravity_history_tests PRIVATE /Gy)
  target_link_options(uam_antigravity_history_tests PRIVATE /OPT:REF)
  if(MSVC)
    set_property(TARGET uam_antigravity_history_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
else()
  target_compile_options(uam_antigravity_history_tests PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(uam_antigravity_history_tests PRIVATE -Wl,--gc-sections)
endif()
enable_testing()
add_test(NAME antigravity_native_history_fixtures COMMAND uam_antigravity_history_tests)

# Header-only CEF dependency at the exact commit used by the desktop's pinned
# 146.0.10+g8219561 package. SOURCE_SUBDIR deliberately has no CMake project:
# no CEF binary, wrapper, desktop or helper target is configured or compiled.
FetchContent_Declare(uam_cef_headers
  URL https://codeload.github.com/chromiumembedded/cef/tar.gz/82195616d8405e6081a0d90924707b82aa9e4141
  SOURCE_SUBDIR uam_headers_only_no_project
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(uam_cef_headers)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
  set(UAM_CEF_HEADER_CPU arm64)
else()
  set(UAM_CEF_HEADER_CPU x64)
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cef-generated/include")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cef-header-args.gn" "target_cpu=\"${UAM_CEF_HEADER_CPU}\"\n")
execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${uam_cef_headers_SOURCE_DIR}/tools/make_config_header.py"
  "${CMAKE_CURRENT_BINARY_DIR}/cef-generated/include/cef_config.h"
  "${CMAKE_CURRENT_BINARY_DIR}/cef-header-args.gn"
  RESULT_VARIABLE UAM_CEF_HEADER_CONFIG_RESULT)
if(NOT UAM_CEF_HEADER_CONFIG_RESULT EQUAL 0)
  message(FATAL_ERROR "Could not generate the pinned CEF header configuration.")
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${uam_cef_headers_SOURCE_DIR}/tools/make_api_versions_header.py"
  "${CMAKE_CURRENT_BINARY_DIR}/cef-generated/include/cef_api_versions.h"
  "${CMAKE_CURRENT_BINARY_DIR}/cef-generated/cef_api_versions.inc"
  "${uam_cef_headers_SOURCE_DIR}/cef_api_versions.json"
  "${uam_cef_headers_SOURCE_DIR}/cef_api_untracked.json"
  RESULT_VARIABLE UAM_CEF_API_VERSIONS_RESULT)
if(NOT UAM_CEF_API_VERSIONS_RESULT EQUAL 0)
  message(FATAL_ERROR "Could not generate the pinned CEF API version headers.")
endif()
# CEF's source wrapper forwards this one list to Chromium; its binary
# distribution replaces the wrapper with the same public list. Use the exact
# compatibility tag declared by this CEF commit, without fetching Chromium.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/chromium-headers/net/base")
file(DOWNLOAD
  "https://raw.githubusercontent.com/chromium/chromium/refs/tags/146.0.7680.179/net/base/net_error_list.h"
  "${CMAKE_CURRENT_BINARY_DIR}/chromium-headers/net/base/net_error_list.h"
  EXPECTED_HASH SHA256=8d849fe28be03bfdb9345204ad06c6371f561d93b3e00f902b1f31126f5017e5
  TLS_VERIFY ON
  STATUS UAM_CEF_NET_ERROR_DOWNLOAD)
list(GET UAM_CEF_NET_ERROR_DOWNLOAD 0 UAM_CEF_NET_ERROR_DOWNLOAD_RESULT)
if(NOT UAM_CEF_NET_ERROR_DOWNLOAD_RESULT EQUAL 0)
  message(FATAL_ERROR "Could not fetch the pinned Chromium public net error list.")
endif()

# Compile the actual shared discovery/assignment/publication implementation and
# all current task fixtures, without linking/running an app, helper or full native suite.
# This establishes source compatibility only; application fixture execution uses the root project
# opt-in uam_native_import_application_tests target or the ordinary core target.
add_library(uam_native_import_source_checks OBJECT EXCLUDE_FROM_ALL
  "${UAM_ROOT}/src/app/runtime_orchestration_services.cpp"
  "${UAM_ROOT}/src/app/startup_native_import_service.cpp"
  "${UAM_ROOT}/src/cef/state_serializer.cpp"
  "${UAM_ROOT}/src/cef/startup_native_import_handlers.cpp"
  "${UAM_ROOT}/src/cef/uam_query_handler.cpp"
  ../antigravity_native_import_tests.cpp ../startup_native_import_tests.cpp)
target_include_directories(uam_native_import_source_checks PRIVATE "${UAM_ROOT}/src" "${UAM_ROOT}/src/common" "${uam_cef_headers_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/cef-generated" "${CMAKE_CURRENT_BINARY_DIR}/chromium-headers")
target_compile_definitions(uam_native_import_source_checks PRIVATE
  UAM_TEST_HARNESS_WITHOUT_CEF=1 ${UAM_HISTORY_PROVIDER_DEFINITIONS}
  UAM_ENABLE_COMPUTER_USE=1 UAM_ENABLE_SSH=1 UAM_ENABLE_MOBILE_COMPANION=1 NOMINMAX)
target_link_libraries(uam_native_import_source_checks PRIVATE uam_sqlite nlohmann_json::nlohmann_json)
if(MSVC)
  set_property(TARGET uam_native_import_source_checks PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

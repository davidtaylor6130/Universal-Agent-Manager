cmake_minimum_required(VERSION 3.20)
project(uam_bootstrap_metadata_checks LANGUAGES CXX)
get_filename_component(UAM_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
file(REAL_PATH "${CMAKE_BINARY_DIR}" UAM_CHECK_BINARY_ROOT)
file(REAL_PATH "${UAM_SOURCE_ROOT}/Builds" UAM_CHECK_ALLOWED_ROOT)
cmake_path(IS_PREFIX UAM_CHECK_ALLOWED_ROOT "${UAM_CHECK_BINARY_ROOT}" NORMALIZE UAM_CHECK_INSIDE_BUILDS)
if(NOT UAM_CHECK_INSIDE_BUILDS)
  message(FATAL_ERROR "Bootstrap metadata checks must build inside the repository Builds directory.")
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  option(UAM_PACKAGE_REMOTE_RUNNERS "Remote packaging is unavailable in this isolated test target." OFF)
  if(UAM_PACKAGE_REMOTE_RUNNERS)
    message(FATAL_ERROR "This target never builds or packages remote helpers.")
  endif()
  set(UAM_BOOTSTRAP_TEST_PREFIX "")
else()
  set(UAM_BOOTSTRAP_TEST_PREFIX "uam_bootstrap_")
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
  include(FetchContent)
  set(JSON_BuildTests OFF CACHE INTERNAL "")
  set(JSON_Install OFF CACHE INTERNAL "")
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(nlohmann_json)
endif()
add_executable(uam_bootstrap_metadata_tests native_fixture.cpp "${UAM_SOURCE_ROOT}/src/remote/runner_bootstrap.cpp" "${UAM_SOURCE_ROOT}/src/remote/runner_client.cpp"
  "${UAM_SOURCE_ROOT}/src/app/memory_library_service.cpp" "${UAM_SOURCE_ROOT}/src/common/memory/memory_library_store.cpp"
  "${UAM_SOURCE_ROOT}/src/common/utils/diagnostic_log.cpp")
target_compile_features(uam_bootstrap_metadata_tests PRIVATE cxx_std_20)
target_include_directories(uam_bootstrap_metadata_tests PRIVATE "${UAM_SOURCE_ROOT}/src")
target_link_libraries(uam_bootstrap_metadata_tests PRIVATE nlohmann_json::nlohmann_json)
set_target_properties(uam_bootstrap_metadata_tests PROPERTIES
  FOLDER "Tests" CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
foreach(provider IN ITEMS GEMINI CODEX OPENCODE CLAUDE COPILOT ANTIGRAVITY)
  target_compile_definitions(uam_bootstrap_metadata_tests PRIVATE UAM_ENABLE_RUNTIME_${provider}_CLI=1)
endforeach()
# build_features.h requires every feature flag; this isolated check uses the full profile.
target_compile_definitions(uam_bootstrap_metadata_tests PRIVATE UAM_ENABLE_COMPUTER_USE=1 UAM_ENABLE_SSH=1 UAM_ENABLE_MOBILE_COMPANION=1)
# windows.h otherwise defines min/max macros that break std::numeric_limits<T>::max() and std::min/std::max.
target_compile_definitions(uam_bootstrap_metadata_tests PRIVATE NOMINMAX)
# This isolated executable uses client admission/file APIs, not accounting collection.
# Discard unreachable client methods rather than link unrelated product services.
if(APPLE)
  target_link_options(uam_bootstrap_metadata_tests PRIVATE "LINKER:-dead_strip")
elseif(MSVC)
  target_compile_options(uam_bootstrap_metadata_tests PRIVATE /Gy /Gw)
  # MSVC resolves references before /OPT:REF discards code; name the unreachable services instead of linking them.
  target_sources(uam_bootstrap_metadata_tests PRIVATE unreachable_services.cpp)
  target_link_options(uam_bootstrap_metadata_tests PRIVATE /OPT:REF)
else()
  target_compile_options(uam_bootstrap_metadata_tests PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(uam_bootstrap_metadata_tests PRIVATE "LINKER:--gc-sections")
endif()
enable_testing()
foreach(check IN ITEMS ManifestValidation NativeTransactionSequencing NativeFailureRollback NativeCancellation NativeLeaseCompetition WindowsReplacementPlan NativeCommitOutcomes NativeRecoveryAdmission NativeDurableFence NativeReplyIdentity NativeHostClientAdmission WindowsBootstrapLockFitsOpenSshCommandLimit RemoteRunnerBootstrapSelectsAndHardensTheWindowsArtifact)
  add_test(NAME ${UAM_BOOTSTRAP_TEST_PREFIX}${check} COMMAND uam_bootstrap_metadata_tests ${check})
endforeach()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_program(UAM_BOOTSTRAP_POWERSHELL NAMES pwsh powershell)
if(UAM_BOOTSTRAP_POWERSHELL)
  add_test(NAME uam_bootstrap_windows_metadata_filesystem
    COMMAND "${Python3_EXECUTABLE}" "${UAM_SOURCE_ROOT}/scripts/test_bootstrap_metadata.py"
      --powershell "${UAM_BOOTSTRAP_POWERSHELL}" --native-binary "$<TARGET_FILE:uam_bootstrap_metadata_tests>")
endif()
if(UNIX)
  add_test(NAME uam_bootstrap_linux_metadata_filesystem
    COMMAND "${Python3_EXECUTABLE}" "${UAM_SOURCE_ROOT}/scripts/test_bootstrap_metadata_linux.py"
      --native-binary "$<TARGET_FILE:uam_bootstrap_metadata_tests>")
endif()

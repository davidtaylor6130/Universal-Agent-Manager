# Standalone native validation: product IO and transactions, inert process adapter, disposable files only.
add_executable(uam_data_folder_tests
  tests/data_folder_test_main.cpp
  tests/data_folder_preflight_tests.cpp
  tests/data_folder_settings_tests.cpp
  tests/data_folder_transition_tests.cpp
  src/app/data_folder_preflight.cpp
  src/app/data_folder_settings_service.cpp
  src/app/data_folder_transition.cpp
  src/common/config/settings_store_data_folder.cpp
  src/common/chat/chat_folder_data_folder.cpp
  src/common/platform/data_folder_tree.cpp
  src/common/platform/platform_incremental_sha256.cpp
  src/common/chat/chat_repository.cpp
  src/common/chat/chat_message_page_index.cpp
  src/common/platform/platform_native_attachment_assets.cpp
  src/computer_use/computer_use_mcp_config.cpp
  tests/data_folder_repository_runtime_fixture.cpp
  src/common/models/app_models.cpp
  src/common/runtime/json_runtime.cpp
  src/common/paths/app_paths.cpp
  src/common/utils/diagnostic_log.cpp)
target_include_directories(uam_data_folder_tests PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/tests")
target_compile_definitions(uam_data_folder_tests PRIVATE UAM_DATA_FOLDER_STANDALONE=1 UAM_SOURCE_ROOT="${CMAKE_SOURCE_DIR}" NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(uam_data_folder_tests PRIVATE nlohmann_json::nlohmann_json)
# Exercise the actual persistence owner without pulling unused runtime entry points into this fixture.
if(APPLE)
  target_link_options(uam_data_folder_tests PRIVATE -Wl,-dead_strip)
elseif(NOT MSVC)
  target_compile_options(uam_data_folder_tests PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(uam_data_folder_tests PRIVATE -Wl,--gc-sections)
endif()
if(WIN32)
  target_link_libraries(uam_data_folder_tests PRIVATE advapi32)
endif()
add_test(NAME uam_data_folder_disposable COMMAND uam_data_folder_tests)
# Compile changed independent-writer integrations without linking/running any app, helper or provider.
add_library(uam_data_folder_integration_check OBJECT
  src/common/provider/native_session_storage_normalization.cpp
  src/app/shell_action_service.cpp
  src/app/uam_control_service.cpp
  src/app/application.cpp
  src/cef/settings_handlers.cpp
  src/cef/uam_query_handler.cpp)
target_include_directories(uam_data_folder_integration_check PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(uam_data_folder_integration_check PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(uam_data_folder_integration_check PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set(UAM_DATA_FOLDER_CEF_HEADER_ROOT "" CACHE PATH "Existing owned CEF header root for compile-only integration validation; never downloaded by focused tests.")
if(UAM_DATA_FOLDER_CEF_HEADER_ROOT)
  target_include_directories(uam_data_folder_integration_check PRIVATE "${UAM_DATA_FOLDER_CEF_HEADER_ROOT}")
  target_include_directories(uam_data_folder_tests PRIVATE "${UAM_DATA_FOLDER_CEF_HEADER_ROOT}")
endif()
add_library(uam_data_folder_writer_compile_check OBJECT src/computer_use/computer_use_mcp_server.cpp)
target_include_directories(uam_data_folder_writer_compile_check PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(uam_data_folder_writer_compile_check PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(uam_data_folder_writer_compile_check PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
foreach(target uam_data_folder_tests uam_data_folder_integration_check uam_data_folder_writer_compile_check)
  foreach(provider GEMINI CODEX CLAUDE OPENCODE COPILOT ANTIGRAVITY)
    target_compile_definitions(${target} PRIVATE UAM_ENABLE_RUNTIME_${provider}_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_${provider}_CLI}>)
  endforeach()
  target_compile_definitions(${target} PRIVATE
    UAM_ENABLE_COMPUTER_USE=$<BOOL:${UAM_ENABLE_COMPUTER_USE}>
    UAM_ENABLE_SSH=$<BOOL:${UAM_ENABLE_SSH}>
    UAM_ENABLE_MOBILE_COMPANION=$<BOOL:${UAM_ENABLE_MOBILE_COMPANION}>)
endforeach()
if(APPLE)
  enable_language(OBJCXX)
  target_sources(uam_data_folder_integration_check PRIVATE src/common/platform/platform_process_macos.cpp)
  target_sources(uam_data_folder_writer_compile_check PRIVATE src/computer_use/computer_use_install_macos.mm)
  set_property(TARGET uam_data_folder_writer_compile_check PROPERTY OBJCXX_STANDARD 20)
endif()
if(WIN32)
  target_sources(uam_data_folder_integration_check PRIVATE src/common/platform/platform_process_windows.cpp)
endif()
add_executable(uam_crash_supervisor_tests tests/app_crash_supervisor_tests.cpp src/common/platform/app_crash_supervisor.cpp)
target_include_directories(uam_crash_supervisor_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
foreach(mode normal bypass restart restart_many folder_restart folder_unconfirmed)
  add_test(NAME uam_crash_supervisor_${mode} COMMAND uam_crash_supervisor_tests ${mode})
endforeach()

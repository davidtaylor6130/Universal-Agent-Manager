if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  message(FATAL_ERROR "Configure the repository root inside Builds/ with UAM_BUILD_TESTS=ON (add UAM_CUSTOM_ACP_TESTS_ONLY=ON to build only these tests).")
endif()
# The opt-in Custom ACP agent's configuration and launch checks, plus a real stdio session with the scripted ACP
# agent fixture. No CEF, app or provider CLI is built or launched, so this also runs where the desktop app cannot.
add_executable(uam_custom_acp_agent_tests
  ../custom_acp_agent_tests.cpp
  ../../src/common/provider/custom_acp/custom_acp_config.cpp)
target_include_directories(uam_custom_acp_agent_tests PRIVATE ../../src)
target_link_libraries(uam_custom_acp_agent_tests PRIVATE nlohmann_json::nlohmann_json)
target_compile_features(uam_custom_acp_agent_tests PRIVATE cxx_std_20)
target_compile_definitions(uam_custom_acp_agent_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(WIN32)
  target_link_libraries(uam_custom_acp_agent_tests PRIVATE advapi32)
endif()
set_target_properties(uam_custom_acp_agent_tests PROPERTIES FOLDER "Tests")
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME uam_custom_acp_agent
    COMMAND uam_custom_acp_agent_tests "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/fixtures/custom_acp_agent.py")
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  message(FATAL_ERROR "Configure the repository root inside Builds/ with UAM_BUILD_TESTS=ON and UAM_SVN_BRANCH_TESTS_ONLY=ON.")
endif()
add_executable(uam_svn_branch_tests ../svn_branch_service_tests.cpp ../../src/app/svn_branch_service.cpp)
target_include_directories(uam_svn_branch_tests PRIVATE ../../src)
target_compile_features(uam_svn_branch_tests PRIVATE cxx_std_20)
# The isolated model fixtures cover all six providers without launching any of them.
target_compile_definitions(uam_svn_branch_tests PRIVATE
  UAM_ENABLE_RUNTIME_GEMINI_CLI=1 UAM_ENABLE_RUNTIME_CODEX_CLI=1
  UAM_ENABLE_RUNTIME_CLAUDE_CLI=1 UAM_ENABLE_RUNTIME_OPENCODE_CLI=1
  UAM_ENABLE_RUNTIME_COPILOT_CLI=1 UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI=1
  UAM_ENABLE_COMPUTER_USE=1 UAM_ENABLE_SSH=1 UAM_ENABLE_MOBILE_COMPANION=1)
add_test(NAME uam_svn_branch_service COMMAND uam_svn_branch_tests)

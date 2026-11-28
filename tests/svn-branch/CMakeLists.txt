if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  message(FATAL_ERROR "Configure the repository root inside Builds/ with UAM_BUILD_TESTS=ON and UAM_SVN_BRANCH_TESTS_ONLY=ON.")
endif()
add_executable(uam_svn_branch_tests ../svn_branch_service_tests.cpp ../../src/app/svn_branch_service.cpp ../../src/app/svn_failure.cpp)
target_include_directories(uam_svn_branch_tests PRIVATE ../../src)
target_compile_features(uam_svn_branch_tests PRIVATE cxx_std_20)
# The isolated model fixtures cover all six providers without launching any of them.
target_compile_definitions(uam_svn_branch_tests PRIVATE
  UAM_ENABLE_RUNTIME_GEMINI_CLI=1 UAM_ENABLE_RUNTIME_CODEX_CLI=1
  UAM_ENABLE_RUNTIME_CLAUDE_CLI=1 UAM_ENABLE_RUNTIME_OPENCODE_CLI=1
  UAM_ENABLE_RUNTIME_COPILOT_CLI=1 UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI=1
  UAM_ENABLE_COMPUTER_USE=1 UAM_ENABLE_SSH=1 UAM_ENABLE_MOBILE_COMPANION=1 NOMINMAX)
# SVN branch creation (#520): its own program, so its fake-repository cases and its real-svn script stay apart from switching.
# The script uses disposable file:// repositories only and reports Skipped (77) without svn and svnadmin, or fails under UAM_REQUIRE_SVN=1.
add_executable(uam_svn_branch_create_tests ../svn_branch_create_tests.cpp ../../src/app/svn_branch_service.cpp)
target_include_directories(uam_svn_branch_create_tests PRIVATE ../../src)
target_compile_features(uam_svn_branch_create_tests PRIVATE cxx_std_20)
get_target_property(_uam_svn_branch_definitions uam_svn_branch_tests COMPILE_DEFINITIONS)
target_compile_definitions(uam_svn_branch_create_tests PRIVATE ${_uam_svn_branch_definitions})
add_test(NAME uam_svn_branch_create COMMAND uam_svn_branch_create_tests)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME uam_svn_branch_create_integration
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/svn_branch_create_integration.py" "$<TARGET_FILE:uam_svn_branch_create_tests>")
  set_tests_properties(uam_svn_branch_create_integration PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()
add_test(NAME uam_svn_branch_service COMMAND uam_svn_branch_tests)
# Disposable file:// repositories only. Without svn and svnadmin the case reports Skipped, and fails when
# UAM_REQUIRE_SVN=1 (set in CI, which installs them).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME uam_svn_branch_integration
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/svn_branch_integration.py" "$<TARGET_FILE:uam_svn_branch_tests>")
  set_tests_properties(uam_svn_branch_integration PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
elseif("$ENV{UAM_REQUIRE_SVN}" STREQUAL "1")
  message(FATAL_ERROR "UAM_REQUIRE_SVN=1 needs Python 3 to run the SVN branch integration test.")
endif()

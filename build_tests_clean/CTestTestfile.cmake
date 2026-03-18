# CMake generated Testfile for 
# Source directory: C:/Users/david/Documents/GitHub/Universal-Agent-Manager
# Build directory: C:/Users/david/Documents/GitHub/Universal-Agent-Manager/build_tests_clean
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test([=[uam_core_tests]=] "C:/Users/david/Documents/GitHub/Universal-Agent-Manager/build_tests_clean/Debug/uam_core_tests.exe")
  set_tests_properties([=[uam_core_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/david/Documents/GitHub/Universal-Agent-Manager/CMakeLists.txt;165;add_test;C:/Users/david/Documents/GitHub/Universal-Agent-Manager/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test([=[uam_core_tests]=] "C:/Users/david/Documents/GitHub/Universal-Agent-Manager/build_tests_clean/Release/uam_core_tests.exe")
  set_tests_properties([=[uam_core_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/david/Documents/GitHub/Universal-Agent-Manager/CMakeLists.txt;165;add_test;C:/Users/david/Documents/GitHub/Universal-Agent-Manager/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test([=[uam_core_tests]=] "C:/Users/david/Documents/GitHub/Universal-Agent-Manager/build_tests_clean/MinSizeRel/uam_core_tests.exe")
  set_tests_properties([=[uam_core_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/david/Documents/GitHub/Universal-Agent-Manager/CMakeLists.txt;165;add_test;C:/Users/david/Documents/GitHub/Universal-Agent-Manager/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test([=[uam_core_tests]=] "C:/Users/david/Documents/GitHub/Universal-Agent-Manager/build_tests_clean/RelWithDebInfo/uam_core_tests.exe")
  set_tests_properties([=[uam_core_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/david/Documents/GitHub/Universal-Agent-Manager/CMakeLists.txt;165;add_test;C:/Users/david/Documents/GitHub/Universal-Agent-Manager/CMakeLists.txt;0;")
else()
  add_test([=[uam_core_tests]=] NOT_AVAILABLE)
endif()
subdirs("_deps/sdl2-build")

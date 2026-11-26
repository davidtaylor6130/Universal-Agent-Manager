# Fails when universal_agent_manager.dll imports a library at load time that a locked-down CEF subprocess must not
# map. CEF's bootstrap.exe loads the DLL into every renderer, GPU and utility process, possibly after sandbox
# lockdown, so everything except the core loader libraries must be delay-loaded (CEF's own client rule).
# Inputs: DLL (path), DUMP_TOOL (dumpbin.exe or link.exe), DUMP_MODE (empty, or /dump for link.exe).

cmake_policy(VERSION 3.20)

foreach(_input IN ITEMS DLL DUMP_TOOL)
  if(NOT DEFINED ${_input} OR "${${_input}}" STREQUAL "")
    message(FATAL_ERROR "check_windows_dll_imports.cmake requires ${_input}.")
  endif()
endforeach()

execute_process(COMMAND "${DUMP_TOOL}" ${DUMP_MODE} /nologo /imports "${DLL}"
  RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _error)
if(NOT _result EQUAL 0)
  message(FATAL_ERROR "Listing the imports of ${DLL} failed (${_result}):\n${_output}${_error}")
endif()
string(REPLACE "\r" "" _output "${_output}")

set(_direct_marker "Section contains the following imports:")
string(FIND "${_output}" "${_direct_marker}" _direct_start)
if(_direct_start EQUAL -1)
  message(FATAL_ERROR "Could not read the direct imports of ${DLL}:\n${_output}")
endif()
set(_delay_marker "Section contains the following delay load imports:")
string(FIND "${_output}" "${_delay_marker}" _delay_start)
if(_delay_start EQUAL -1)
  message(FATAL_ERROR "${DLL} has no delay-load imports; CEF's delay-load list was not applied:\n${_output}")
endif()
string(SUBSTRING "${_output}" 0 ${_delay_start} _direct)
string(SUBSTRING "${_output}" ${_delay_start} -1 _delayed)

# Library names sit on their own line, indented four spaces, in each section.
# Any extension counts: winspool.drv is an import too.
string(REGEX MATCHALL "\n    [A-Za-z0-9_.-]+\\.[A-Za-z]+\n" _direct_entries "${_direct}")
string(REGEX MATCHALL "\n    [A-Za-z0-9_.-]+\\.[A-Za-z]+\n" _delayed_entries "${_delayed}")
set(_direct_names)
foreach(_entry IN LISTS _direct_entries)
  string(STRIP "${_entry}" _name)
  string(TOLOWER "${_name}" _name)
  list(APPEND _direct_names "${_name}")
endforeach()
set(_delayed_names)
foreach(_entry IN LISTS _delayed_entries)
  string(STRIP "${_entry}" _name)
  string(TOLOWER "${_name}" _name)
  list(APPEND _delayed_names "${_name}")
endforeach()

if(NOT "libcef.dll" IN_LIST _delayed_names)
  message(FATAL_ERROR "${DLL} does not delay-load libcef.dll:\n${_output}")
endif()

set(_forbidden)
foreach(_name IN LISTS _direct_names)
  if(NOT _name MATCHES "^(kernel32|ntdll)\\.dll$" AND NOT _name MATCHES "^api-ms-win-core-.*\\.dll$")
    list(APPEND _forbidden "${_name}")
  endif()
endforeach()
if(_forbidden)
  string(REPLACE ";" ", " _forbidden_text "${_forbidden}")
  message(FATAL_ERROR "${DLL} imports ${_forbidden_text} at load time. Sandboxed CEF subprocesses load this DLL, so "
    "add each library to the /DELAYLOAD options in CMakeLists.txt (or stop linking it through an umbrella library).")
endif()
list(LENGTH _delayed_names _delayed_count)
message(STATUS "universal_agent_manager.dll imports only core loader libraries directly; ${_delayed_count} libraries are delay-loaded.")

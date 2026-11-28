# Creates universal_agent_manager.exe from CEF's bootstrap.exe (the Windows Chromium sandbox layout).
# bootstrap.exe loads the DLL named after itself from its own folder and calls its exported RunWinMain.
# Inputs: BOOTSTRAP, CLIENT_DLL, EXECUTABLE, RESOURCE_TOOL, MT, MANIFEST, WORK_DIRECTORY.

foreach(_input IN ITEMS BOOTSTRAP CLIENT_DLL EXECUTABLE RESOURCE_TOOL MT MANIFEST WORK_DIRECTORY)
  if(NOT DEFINED ${_input} OR "${${_input}}" STREQUAL "")
    message(FATAL_ERROR "windows_bootstrap_executable.cmake requires ${_input}.")
  endif()
endforeach()

get_filename_component(_client_name "${CLIENT_DLL}" NAME_WE)
get_filename_component(_executable_name "${EXECUTABLE}" NAME_WE)
if(NOT _client_name STREQUAL _executable_name)
  message(FATAL_ERROR "bootstrap.exe loads the DLL named after itself: ${_executable_name}.dll, not ${_client_name}.dll.")
endif()

function(run_step description)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${description} failed (${result}):\n${output}")
  endif()
endfunction()

file(MAKE_DIRECTORY "${WORK_DIRECTORY}")
# Start from the unmodified bootstrap every time, so the resource and manifest edits below never stack.
run_step("Copying CEF's bootstrap.exe" "${CMAKE_COMMAND}" -E copy "${BOOTSTRAP}" "${EXECUTABLE}")
# UAM's icon replaces the bootstrap's, and its "CEF Bootstrap Application" version text is removed so Explorer and
# Task Manager show universal_agent_manager.exe as before. The tool refuses a signed bootstrap.exe.
run_step("Copying UAM's icon into universal_agent_manager.exe" "${RESOURCE_TOOL}" "${CLIENT_DLL}" "${EXECUTABLE}")

# Only the process executable's manifest applies, so merge UAM's long-path setting into the bootstrap's own manifest
# (which already declares Windows 10 compatibility for Chromium).
# mt.exe names a resource as "<file>;#1"; these calls stay inline because passing that through a function's
# argument list would split it at the semicolon.
set(_merged "${WORK_DIRECTORY}/universal_agent_manager.exe.manifest")
set(_embedded "${WORK_DIRECTORY}/embedded.manifest")
execute_process(COMMAND "${MT}" -nologo "-inputresource:${EXECUTABLE};#1" -manifest "${MANIFEST}" "-out:${_merged}"
  RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _output)
if(NOT _result EQUAL 0)
  message(FATAL_ERROR "Merging the bootstrap manifest failed (${_result}):\n${_output}")
endif()
execute_process(COMMAND "${MT}" -nologo -manifest "${_merged}" "-outputresource:${EXECUTABLE};#1"
  RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _output)
if(NOT _result EQUAL 0)
  message(FATAL_ERROR "Embedding the merged manifest failed (${_result}):\n${_output}")
endif()
execute_process(COMMAND "${MT}" -nologo "-inputresource:${EXECUTABLE};#1" "-out:${_embedded}"
  RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _output)
if(NOT _result EQUAL 0)
  message(FATAL_ERROR "Reading the embedded manifest failed (${_result}):\n${_output}")
endif()
file(READ "${_embedded}" _embedded_text)
foreach(_required IN ITEMS "longPathAware" "8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a")
  string(FIND "${_embedded_text}" "${_required}" _found)
  if(_found EQUAL -1)
    message(FATAL_ERROR "universal_agent_manager.exe's manifest lacks ${_required} after merging.")
  endif()
endforeach()

# CEF's documented LPAC requirement for the output folder (see SET_LPAC_ACLS in CEF's cef_macros.cmake). The app
# re-checks this at startup because ZIP extraction does not keep folder permissions.
get_filename_component(_output_directory "${EXECUTABLE}" DIRECTORY)
file(TO_NATIVE_PATH "${_output_directory}" _output_directory_native)
run_step("Granting LPAC read access to the output folder" icacls "${_output_directory_native}" /grant "*S-1-15-2-2:(OI)(CI)(RX)")

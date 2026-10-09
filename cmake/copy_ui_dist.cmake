if(NOT DEFINED UAM_UI_SOURCE OR UAM_UI_SOURCE STREQUAL "" OR
   NOT DEFINED UAM_UI_DEST OR UAM_UI_DEST STREQUAL "")
  message(FATAL_ERROR "UI bundle source and destination paths are required.")
endif()

if(NOT IS_DIRECTORY "${UAM_UI_SOURCE}")
  message(FATAL_ERROR "The generated UI bundle is missing.")
endif()

file(REAL_PATH "${UAM_UI_SOURCE}" UAM_UI_SOURCE_REAL)
file(REAL_PATH "${UAM_UI_DEST}" UAM_UI_DEST_REAL)
if(WIN32)
  string(TOLOWER "${UAM_UI_SOURCE_REAL}" UAM_UI_SOURCE_REAL)
  string(TOLOWER "${UAM_UI_DEST_REAL}" UAM_UI_DEST_REAL)
endif()

if(NOT UAM_UI_SOURCE_REAL STREQUAL UAM_UI_DEST_REAL)
  file(REMOVE_RECURSE "${UAM_UI_DEST}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${UAM_UI_SOURCE}" "${UAM_UI_DEST}"
    RESULT_VARIABLE UAM_UI_COPY_RESULT
  )
  if(NOT UAM_UI_COPY_RESULT EQUAL 0)
    message(FATAL_ERROR "The generated UI bundle could not be copied.")
  endif()
endif()

# Reassert target archive tools after cached compiler metadata is loaded by project().
# Command-line cache values alone cannot replace normal variables in older build trees.
if(NOT CMAKE_CROSSCOMPILING OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "Zig archive tools are reserved for Linux cross-builds.")
endif()

foreach(prefix IN ITEMS "" "C_COMPILER_" "CXX_COMPILER_")
  set(CMAKE_${prefix}AR "${CMAKE_CURRENT_LIST_DIR}/zig-ar.sh" CACHE FILEPATH
    "Zig target archiver" FORCE)
  set(CMAKE_${prefix}RANLIB "${CMAKE_CURRENT_LIST_DIR}/zig-ranlib.sh" CACHE FILEPATH
    "Zig target archive indexer" FORCE)
  set(CMAKE_${prefix}AR "${CMAKE_CURRENT_LIST_DIR}/zig-ar.sh")
  set(CMAKE_${prefix}RANLIB "${CMAKE_CURRENT_LIST_DIR}/zig-ranlib.sh")
endforeach()

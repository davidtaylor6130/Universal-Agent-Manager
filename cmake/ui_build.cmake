# Keep frontend outputs local to each build tree and rebuild only when inputs change.
set(UAM_UI_DIST_SRC "${CMAKE_BINARY_DIR}/UI-V2/dist")
if(WIN32)
  unset(UAM_NPM_EXECUTABLE CACHE)
  find_program(UAM_NPM_EXECUTABLE NAMES npm.cmd REQUIRED)
else()
  find_program(UAM_NPM_EXECUTABLE NAMES npm REQUIRED)
endif()

file(GLOB_RECURSE UAM_UI_INPUTS CONFIGURE_DEPENDS LIST_DIRECTORIES false
  "${CMAKE_SOURCE_DIR}/UI-V2/src/*"
  "${CMAKE_SOURCE_DIR}/UI-V2/public/*")
file(GLOB UAM_UI_CONFIG_INPUTS CONFIGURE_DEPENDS LIST_DIRECTORIES false
  "${CMAKE_SOURCE_DIR}/UI-V2/*.json"
  "${CMAKE_SOURCE_DIR}/UI-V2/*.js"
  "${CMAKE_SOURCE_DIR}/UI-V2/*.cjs"
  "${CMAKE_SOURCE_DIR}/UI-V2/*.mjs"
  "${CMAKE_SOURCE_DIR}/UI-V2/*.ts"
  "${CMAKE_SOURCE_DIR}/UI-V2/.env*"
  "${CMAKE_SOURCE_DIR}/UI-V2/node_modules/.package-lock.json")
list(APPEND UAM_UI_INPUTS ${UAM_UI_CONFIG_INPUTS}
  "${CMAKE_SOURCE_DIR}/UI-V2/index.html")
# The list itself invalidates the bundle when an input is deleted or added with an old timestamp.
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/UI-V2/inputs.txt" CONTENT "${UAM_UI_INPUTS}\n")
# Every normalised build flag reaches Vite as a compile-time constant (see cmake/build_features.cmake).
if(NOT DEFINED UAM_UI_FEATURE_ENV)
  message(FATAL_ERROR "Include cmake/build_features.cmake before cmake/ui_build.cmake.")
endif()
set(UAM_UI_FEATURE_ENV_ARGS "")
foreach(_uam_ui_feature IN LISTS UAM_UI_FEATURE_ENV)
  string(APPEND UAM_UI_FEATURE_ENV_ARGS "    \"${_uam_ui_feature}\"\n")
endforeach()
configure_file("${CMAKE_CURRENT_LIST_DIR}/build_ui.cmake.in"
  "${CMAKE_BINARY_DIR}/UI-V2/build.cmake" @ONLY)

add_custom_command(OUTPUT "${UAM_UI_DIST_SRC}/index.html"
  BYPRODUCTS "${UAM_UI_DIST_SRC}/.vite/manifest.json"
  COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_BINARY_DIR}/UI-V2/build.cmake"
  DEPENDS ${UAM_UI_INPUTS}
    "${CMAKE_BINARY_DIR}/UI-V2/inputs.txt"
    "${CMAKE_BINARY_DIR}/UI-V2/build.cmake"
  COMMENT "Building UI-V2 dist in ${UAM_UI_DIST_SRC}"
  VERBATIM)
add_custom_target(uam_ui_v2_build DEPENDS "${UAM_UI_DIST_SRC}/index.html")

# Resource-only changes must still run POST_BUILD copying and signing.
file(GLOB_RECURSE UAM_PACKAGE_INPUTS CONFIGURE_DEPENDS LIST_DIRECTORIES false
  "${CMAKE_SOURCE_DIR}/assets/*"
  "${CMAKE_SOURCE_DIR}/markdown-store/*")
if(UAM_PACKAGE_REMOTE_RUNNERS)
  file(GLOB_RECURSE UAM_REMOTE_PACKAGE_INPUTS CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${UAM_REMOTE_RUNNER_ARTIFACT_DIR}/*")
  list(APPEND UAM_PACKAGE_INPUTS ${UAM_REMOTE_PACKAGE_INPUTS})
endif()
list(APPEND UAM_PACKAGE_INPUTS "${CMAKE_SOURCE_DIR}/CMakeLists.txt")
if(UAM_ENABLE_SSH)
  list(APPEND UAM_PACKAGE_INPUTS "$<TARGET_FILE:uam_runner>")
endif()
if(UAM_ENABLE_MOBILE_COMPANION AND UAM_CADDY_EXECUTABLE)
  list(APPEND UAM_PACKAGE_INPUTS "${UAM_CADDY_EXECUTABLE}" "${UAM_CADDY_LICENSE}")
endif()
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/package-inputs-$<CONFIG>.txt" CONTENT "${UAM_PACKAGE_INPUTS}\n")
# A generated translation unit also invalidates Visual Studio builds, which ignore LINK_DEPENDS.
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/uam-package-inputs-$<CONFIG>.cpp"
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/uam-package-inputs-$<CONFIG>.cpp"
  DEPENDS ${UAM_PACKAGE_INPUTS} "${UAM_UI_DIST_SRC}/index.html"
    "${CMAKE_BINARY_DIR}/package-inputs-$<CONFIG>.txt" "${CMAKE_CURRENT_LIST_FILE}"
    "${CMAKE_CURRENT_LIST_DIR}/copy_ui_dist.cmake" "${CMAKE_CURRENT_LIST_DIR}/Info.plist.in"
  COMMENT "Refreshing packaged resource dependencies"
  VERBATIM)
target_sources(universal_agent_manager PRIVATE "${CMAKE_BINARY_DIR}/uam-package-inputs-$<CONFIG>.cpp")

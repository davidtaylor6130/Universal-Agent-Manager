# Resource-only changes must still run POST_BUILD copying and signing.
file(GLOB_RECURSE UAM_PACKAGE_INPUTS CONFIGURE_DEPENDS LIST_DIRECTORIES false
  "${CMAKE_SOURCE_DIR}/assets/*"
  "${CMAKE_SOURCE_DIR}/markdown-store/bundled/*")
if(UAM_PACKAGE_REMOTE_RUNNERS)
  file(GLOB_RECURSE UAM_REMOTE_PACKAGE_INPUTS CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${UAM_REMOTE_RUNNER_ARTIFACT_DIR}/*")
  list(APPEND UAM_PACKAGE_INPUTS ${UAM_REMOTE_PACKAGE_INPUTS})
endif()
list(APPEND UAM_PACKAGE_INPUTS "${CMAKE_SOURCE_DIR}/CMakeLists.txt")
if(UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI AND uam_antigravity_mbedtls_SOURCE_DIR)
  list(APPEND UAM_PACKAGE_INPUTS "${uam_antigravity_mbedtls_SOURCE_DIR}/LICENSE")
endif()
if(UAM_ENABLE_SSH)
  list(APPEND UAM_PACKAGE_INPUTS "$<TARGET_FILE:uam_runner>")
endif()
if(UAM_ENABLE_MOBILE_COMPANION AND uam_caddy_SOURCE_DIR)
  list(APPEND UAM_PACKAGE_INPUTS "${uam_caddy_SOURCE_DIR}/caddy" "${uam_caddy_SOURCE_DIR}/LICENSE")
endif()
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/package-inputs-$<CONFIG>.txt" CONTENT "${UAM_PACKAGE_INPUTS}\n")
# A generated translation unit also invalidates Visual Studio builds, which ignore LINK_DEPENDS.
# CMake 4.5 resolves a $<CONFIG> source path with an empty configuration on single-config generators,
# so those generators name the per-configuration files with CMAKE_BUILD_TYPE instead.
get_property(UAM_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(UAM_MULTI_CONFIG)
  set(UAM_PACKAGE_INPUTS_CONFIG "$<CONFIG>")
else()
  set(UAM_PACKAGE_INPUTS_CONFIG "${CMAKE_BUILD_TYPE}")
endif()
set(UAM_PACKAGE_INPUTS_SOURCE "${CMAKE_BINARY_DIR}/uam-package-inputs-${UAM_PACKAGE_INPUTS_CONFIG}.cpp")
add_custom_command(OUTPUT "${UAM_PACKAGE_INPUTS_SOURCE}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${UAM_PACKAGE_INPUTS_SOURCE}"
  DEPENDS ${UAM_PACKAGE_INPUTS} "${UAM_UI_DIST_SRC}/index.html"
    "${CMAKE_BINARY_DIR}/package-inputs-${UAM_PACKAGE_INPUTS_CONFIG}.txt" "${CMAKE_CURRENT_LIST_FILE}"
    "${CMAKE_CURRENT_LIST_DIR}/copy_ui_dist.cmake" "${CMAKE_CURRENT_LIST_DIR}/Info.plist.in"
  COMMENT "Refreshing packaged resource dependencies"
  VERBATIM)
target_sources(universal_agent_manager PRIVATE "${UAM_PACKAGE_INPUTS_SOURCE}")

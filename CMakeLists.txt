cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
project(universal_agent_manager LANGUAGES C CXX)

if(NOT WIN32 AND NOT APPLE)
  message(FATAL_ERROR "universal_agent_manager supports only Windows and macOS builds.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_SUPPRESS_REGENERATION ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "_CMake")

# ---------------------------------------------------------------------------
# Build-directory enforcement
# ---------------------------------------------------------------------------

set(UAM_REQUIRED_BUILD_ROOT "${CMAKE_SOURCE_DIR}/Builds")
file(MAKE_DIRECTORY "${UAM_REQUIRED_BUILD_ROOT}")
file(REAL_PATH "${CMAKE_SOURCE_DIR}" UAM_SOURCE_DIR_REAL)
file(REAL_PATH "${UAM_REQUIRED_BUILD_ROOT}" UAM_REQUIRED_BUILD_ROOT_REAL)
file(REAL_PATH "${CMAKE_BINARY_DIR}" UAM_BINARY_DIR_REAL)
cmake_path(
  RELATIVE_PATH UAM_BINARY_DIR_REAL
  BASE_DIRECTORY "${UAM_REQUIRED_BUILD_ROOT_REAL}"
  OUTPUT_VARIABLE UAM_BINARY_DIR_RELATIVE_TO_REQUIRED_ROOT
)
cmake_path(
  RELATIVE_PATH UAM_BINARY_DIR_REAL
  BASE_DIRECTORY "${UAM_SOURCE_DIR_REAL}"
  OUTPUT_VARIABLE UAM_BINARY_DIR_RELATIVE_TO_SOURCE_ROOT
)

set(UAM_BUILD_DIR_IN_REQUIRED_ROOT TRUE)
if(UAM_BINARY_DIR_RELATIVE_TO_REQUIRED_ROOT MATCHES "^\\.\\.")
  set(UAM_BUILD_DIR_IN_REQUIRED_ROOT FALSE)
endif()

set(UAM_BUILD_DIR_IS_CLION_DEFAULT FALSE)
if(NOT UAM_BINARY_DIR_RELATIVE_TO_SOURCE_ROOT MATCHES "^\\.\\." AND
   UAM_BINARY_DIR_RELATIVE_TO_SOURCE_ROOT MATCHES "^cmake-build-")
  set(UAM_BUILD_DIR_IS_CLION_DEFAULT TRUE)
endif()

if(NOT UAM_BUILD_DIR_IN_REQUIRED_ROOT)
  if(UAM_BUILD_DIR_IS_CLION_DEFAULT)
    message(STATUS
      "Using CLion default build directory '${CMAKE_BINARY_DIR}'. "
      "Preferred location is '${UAM_REQUIRED_BUILD_ROOT}'."
    )
  else()
    message(FATAL_ERROR
      "Build directory must be inside '${UAM_REQUIRED_BUILD_ROOT}'.\n"
      "Use one of:\n"
      "  cmake -S . -B Builds\n"
      "  cmake -S . -B Builds/<name>\n"
      "\n"
      "CLion default profiles are also accepted (for example: cmake-build-debug)."
    )
  endif()
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(UAM_BUILD_TESTS "Build UAM core tests executable." OFF)
set(UAM_FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH
  "Directory used for FetchContent source checkouts.")
if(APPLE)
  option(UAM_MACOS_BUNDLE "Build as a macOS .app bundle (no Terminal window needed)." ON)
endif()

file(MAKE_DIRECTORY "${UAM_FETCHCONTENT_BASE_DIR}")
set(FETCHCONTENT_BASE_DIR "${UAM_FETCHCONTENT_BASE_DIR}")

function(uam_set_target_folder target_name folder_name)
  if(TARGET ${target_name})
    set_property(TARGET ${target_name} PROPERTY FOLDER ${folder_name})
  endif()
endfunction()

include(FetchContent)

# ---------------------------------------------------------------------------
# nlohmann/json (header-only)
# ---------------------------------------------------------------------------

set(CMAKE_FOLDER "ThirdParty/nlohmann_json")
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)
set(CMAKE_FOLDER "")

# ---------------------------------------------------------------------------
# CEF (Chromium Embedded Framework) — platform-aware prebuilt download
# ---------------------------------------------------------------------------

# Platform-specific CEF versions (latest stable from cef-builds.spotifycdn.com).
if(WIN32)
  set(CEF_PLATFORM "windows64")
  set(CEF_VERSION "146.0.10+g8219561+chromium-146.0.7680.179")
elseif(APPLE)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    set(CEF_PLATFORM "macosarm64")
  else()
    set(CEF_PLATFORM "macosx64")
  endif()
  set(CEF_VERSION "138.0.61+g664e74a+chromium-138.0.7204.308")
else()
  set(CEF_PLATFORM "linux64")
  set(CEF_VERSION "138.0.61+g664e74a+chromium-138.0.7204.308")
endif()

# Encode the + characters as %2B for the URL.
string(REPLACE "+" "%2B" CEF_VERSION_ENCODED "${CEF_VERSION}")

set(CMAKE_FOLDER "ThirdParty/CEF")
FetchContent_Declare(
  cef_binary
  URL "https://cef-builds.spotifycdn.com/cef_binary_${CEF_VERSION_ENCODED}_${CEF_PLATFORM}_minimal.tar.bz2"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# Force libcef_dll_wrapper to be a STATIC library regardless of BUILD_SHARED_LIBS.
# Shared wrapper builds can hide symbols and leave the app with unresolved CEF
# references at link time.
set(_saved_bsl ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cef_binary)
set(BUILD_SHARED_LIBS ${_saved_bsl} CACHE BOOL "" FORCE)
set(CMAKE_FOLDER "")

# FetchContent_MakeAvailable already processes the CEF CMakeLists.txt, which
# builds libcef_dll_wrapper and creates the libcef_dll_wrapper target.
# No explicit add_subdirectory needed.

# ---------------------------------------------------------------------------
# Provider sources
# ---------------------------------------------------------------------------

set(UAM_PROVIDER_SOURCES
  src/common/provider/codex/cli/codex_cli_provider_runtime.cpp
  src/common/provider/gemini/cli/gemini_cli_provider_runtime.cpp
  src/common/provider/gemini/base/gemini_history_loader.cpp
)

# ---------------------------------------------------------------------------
# Platform sources
# ---------------------------------------------------------------------------

set(UAM_PLATFORM_SOURCES)
set(UAM_PLATFORM_IMPL_SOURCES)
if(WIN32)
  list(APPEND UAM_PLATFORM_SOURCES src/uam.rc)
  list(APPEND UAM_PLATFORM_IMPL_SOURCES
    src/common/platform/platform_services_windows_impl.cpp
  )
elseif(APPLE)
  list(APPEND UAM_PLATFORM_IMPL_SOURCES
    src/common/platform/platform_services_macos_impl.cpp
  )
endif()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------

set(UAM_EXECUTABLE_KIND "")
if(APPLE AND UAM_MACOS_BUNDLE)
  set(UAM_EXECUTABLE_KIND MACOSX_BUNDLE)
elseif(WIN32)
  set(UAM_EXECUTABLE_KIND WIN32)
endif()

add_executable(universal_agent_manager ${UAM_EXECUTABLE_KIND}
  ${UAM_PLATFORM_SOURCES}
  ${UAM_PLATFORM_IMPL_SOURCES}
  src/main.cpp
  src/app/application.cpp
  src/app/application_core_helpers.cpp
  src/app/chat_domain_service.cpp
  src/app/chat_lifecycle_service.cpp
  src/app/native_session_link_service.cpp
  src/app/persistence_coordinator.cpp
  src/app/provider_resolution_service.cpp
  src/app/provider_profile_migration_service.cpp
  src/app/runtime_orchestration_services.cpp
  src/cef/uam_cef_app.cpp
  src/cef/uam_cef_client.cpp
  src/cef/uam_bridge_request.cpp
  src/cef/uam_query_handler.cpp
  src/cef/state_serializer.cpp
  src/cef/cef_push.cpp
  src/common/models/app_models.cpp
  src/common/paths/app_paths.cpp
  src/common/chat/chat_folder_store.cpp
  src/common/chat/chat_repository.cpp
  src/common/chat/chat_branching.cpp
  src/common/config/settings_store.cpp
  src/common/utils/command_line_words.cpp
  src/common/config/frontend_actions.cpp
  src/common/provider/provider_profile.cpp
  src/common/provider/runtime/provider_runtime_facade.cpp
  src/common/provider/runtime/provider_runtime_registry.cpp
  src/common/runtime/acp/acp_session_runtime.cpp
  src/core/gemini_cli_compat.cpp
  ${UAM_PROVIDER_SOURCES}
  src/common/runtime/provider_cli_compatibility_service.cpp
  src/common/platform/platform_services.cpp
)

target_include_directories(universal_agent_manager
  PRIVATE
    src
    src/common
    ${cef_binary_SOURCE_DIR}
    "${CMAKE_BINARY_DIR}/_deps/nlohmann_json-src/include"
)

# CEF link targets depend on platform.
# The CEF distribution's own CMakeLists.txt exposes 'libcef_dll_wrapper'.
# The actual CEF library must be linked by name.
if(APPLE)
  # Use LINK_FLAGS (a raw string) so the shell can parse the double-quoted path
  # that contains spaces (the project root contains spaces).
  set_property(TARGET universal_agent_manager APPEND PROPERTY
    LINK_FLAGS
    "-F\"${cef_binary_SOURCE_DIR}/Release\" -framework \"Chromium Embedded Framework\""
  )
  target_link_libraries(universal_agent_manager PRIVATE
    libcef_dll_wrapper
    "-Wl,-framework,Security"
    "-Wl,-framework,WebKit"
  )
elseif(WIN32)
  target_link_libraries(universal_agent_manager PRIVATE
    libcef_dll_wrapper
    "${cef_binary_SOURCE_DIR}/Release/libcef.lib"
  )
endif()

target_compile_definitions(universal_agent_manager PRIVATE
  NOMINMAX
)

set_target_properties(universal_agent_manager PROPERTIES FOLDER "App")
if(MSVC)
  set_property(TARGET universal_agent_manager PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  )
  set_target_properties(universal_agent_manager PROPERTIES
    VS_GLOBAL_VcpkgEnabled "false"
    VS_GLOBAL_VcpkgApplocalDeps "false"
  )
endif()

# ---------------------------------------------------------------------------
# CEF binary resources — copy to build output
# ---------------------------------------------------------------------------

# macOS: copy the .framework into the .app bundle or next to the binary,
#        and create the four CEF helper app bundles required by the macOS
#        multi-process architecture.  Without these, CefInitialize() crashes
#        with EXC_BREAKPOINT (SIGTRAP) because Chromium cannot spawn its
#        renderer / GPU / plugin subprocesses.
if(APPLE)
  set(CEF_FRAMEWORK_SRC "${cef_binary_SOURCE_DIR}/Release/Chromium Embedded Framework.framework")
  if(UAM_MACOS_BUNDLE)
    add_custom_command(TARGET universal_agent_manager POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CEF_FRAMEWORK_SRC}"
        "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Frameworks/Chromium Embedded Framework.framework"
      COMMENT "Copying CEF framework into .app bundle"
    )

    # -----------------------------------------------------------------------
    # CEF helper app bundles
    # Each helper is a minimal .app containing a copy of the main executable.
    # CEF discovers them by convention:
    #   Contents/Frameworks/<ExeName> Helper[<suffix>].app/Contents/MacOS/<ExeName> Helper[<suffix>]
    #
    # Helper names contain spaces and parentheses which break shell quoting in
    # add_custom_command COMMAND lists.  We avoid the issue entirely by writing
    # a small CMake script at configure time and executing it via cmake -P at
    # build time.  Generator expressions are expanded via a GENEX_EVAL wrapper.
    # -----------------------------------------------------------------------
    set(_cef_plists_dir "${CMAKE_BINARY_DIR}/cef_helper_plists")

    # JIT entitlement plist (needed on macOS 12+ for V8 to work).
    set(_jit_entitlements "${_cef_plists_dir}/jit.entitlements")
    file(WRITE "${_jit_entitlements}"
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
  <key>com.apple.security.cs.allow-jit</key><true/>
  <key>com.apple.security.cs.allow-unsigned-executable-memory</key><true/>
  <key>com.apple.security.cs.disable-library-validation</key><true/>
</dict>
</plist>")

    # Write Info.plists and accumulate cmake_script content at configure time.
    set(_helper_script_content
"# Auto-generated by CMakeLists.txt — do not edit.
cmake_minimum_required(VERSION 3.20)
# Remove legacy UI-V2/dist from Contents/MacOS/ — it was moved to Contents/Resources/.
# If left in MacOS/, codesign finds the .woff fonts and produces a broken signature.
file(REMOVE_RECURSE \"\${UAM_BUNDLE_CONTENTS}/MacOS/UI-V2\")
")

    foreach(_hname IN ITEMS
        "universal_agent_manager Helper"
        "universal_agent_manager Helper (GPU)"
        "universal_agent_manager Helper (Plugin)"
        "universal_agent_manager Helper (Renderer)")

      # DNS-safe bundle identifier suffix.
      string(REGEX REPLACE "[() ]" "" _hid "${_hname}")
      string(TOLOWER "${_hid}" _hid)

      # Info.plist written at configure time; script copies it at build time.
      file(WRITE "${_cef_plists_dir}/${_hid}/Info.plist"
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>${_hname}</string>
  <key>CFBundleIdentifier</key><string>com.universalagentmanager.desktop.${_hid}</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>${_hname}</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleVersion</key><string>1.0.0</string>
  <key>LSBackgroundOnly</key><true/>
  <key>LSMinimumSystemVersion</key><string>10.15.0</string>
</dict>
</plist>")

      # Append to the cmake script that will run POST_BUILD.
      # UAM_BUNDLE_CONTENTS and UAM_MAIN_EXE are passed as -D args at build time.
      string(APPEND _helper_script_content
"
set(_hdir \"\${UAM_BUNDLE_CONTENTS}/Frameworks/${_hname}.app/Contents\")
file(MAKE_DIRECTORY \"\${_hdir}/MacOS\")
file(MAKE_DIRECTORY \"\${_hdir}/Frameworks\")
file(COPY_FILE \"\${UAM_MAIN_EXE}\" \"\${_hdir}/MacOS/${_hname}\" ONLY_IF_DIFFERENT)
file(COPY_FILE \"${_cef_plists_dir}/${_hid}/Info.plist\" \"\${_hdir}/Info.plist\" ONLY_IF_DIFFERENT)
# Symlink to the CEF framework in the parent bundle so that
# @executable_path/../Frameworks/Chromium Embedded Framework.framework resolves.
# Relative path: Contents/Frameworks/ -> ../../../ -> Contents/Frameworks/ (parent)
if(NOT EXISTS \"\${_hdir}/Frameworks/Chromium Embedded Framework.framework\")
  file(CREATE_LINK \"../../../Chromium Embedded Framework.framework\"
       \"\${_hdir}/Frameworks/Chromium Embedded Framework.framework\"
       SYMBOLIC)
endif()
execute_process(COMMAND codesign --force --sign - --entitlements \"${_jit_entitlements}\"
  \"\${UAM_BUNDLE_CONTENTS}/Frameworks/${_hname}.app\")
")
    endforeach()

    # Sign the main executable binary with the JIT entitlement.
    # We sign only the binary (not the whole bundle) to avoid the sealed-resources
    # error caused by non-code assets (e.g. .woff fonts) inside the bundle.
    string(APPEND _helper_script_content
"execute_process(COMMAND codesign --force --sign - --entitlements \"${_jit_entitlements}\"
  \"\${UAM_MAIN_EXE}\")
")

    # Write the script to the build dir.
    set(_helper_cmake_script "${_cef_plists_dir}/create_helpers.cmake")
    file(WRITE "${_helper_cmake_script}" "${_helper_script_content}")

    # POST_BUILD: run the generated script, passing bundle paths via -D.
    add_custom_command(TARGET universal_agent_manager POST_BUILD
      COMMAND ${CMAKE_COMMAND}
        -DUAM_BUNDLE_CONTENTS=$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>
        -DUAM_BUNDLE_DIR=$<TARGET_BUNDLE_DIR:universal_agent_manager>
        -DUAM_MAIN_EXE=$<TARGET_FILE:universal_agent_manager>
        -P "${_helper_cmake_script}"
      VERBATIM
      COMMENT "Creating CEF helper bundles and ad-hoc signing"
    )

  else()
    add_custom_command(TARGET universal_agent_manager POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CEF_FRAMEWORK_SRC}"
        "$<TARGET_FILE_DIR:universal_agent_manager>/Chromium Embedded Framework.framework"
      COMMENT "Copying CEF framework next to binary"
    )
  endif()
endif()

# Windows: copy CEF runtime DLLs and resources.
if(WIN32)
  set(CEF_RELEASE_DIR "${cef_binary_SOURCE_DIR}/Release")
  set(CEF_RESOURCES_DIR "${cef_binary_SOURCE_DIR}/Resources")

  if(DEFINED CEF_BINARY_FILES)
    set(UAM_CEF_BINARY_FILES ${CEF_BINARY_FILES})
  else()
    set(UAM_CEF_BINARY_FILES
      chrome_elf.dll
      d3dcompiler_47.dll
      libcef.dll
      libEGL.dll
      libGLESv2.dll
      v8_context_snapshot.bin
      vk_swiftshader.dll
      vk_swiftshader_icd.json
      vulkan-1.dll
      dxil.dll
      dxcompiler.dll
    )
  endif()

  foreach(_cef_binary IN LISTS UAM_CEF_BINARY_FILES)
    if(EXISTS "${CEF_RELEASE_DIR}/${_cef_binary}")
      add_custom_command(TARGET universal_agent_manager POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CEF_RELEASE_DIR}/${_cef_binary}"
          "$<TARGET_FILE_DIR:universal_agent_manager>/${_cef_binary}"
      )
    endif()
  endforeach()

  if(DEFINED CEF_RESOURCE_FILES)
    set(UAM_CEF_RESOURCE_FILES ${CEF_RESOURCE_FILES})
  else()
    set(UAM_CEF_RESOURCE_FILES
      chrome_100_percent.pak
      chrome_200_percent.pak
      resources.pak
      icudtl.dat
      locales
    )
  endif()

  foreach(_cef_resource IN LISTS UAM_CEF_RESOURCE_FILES)
    if(IS_DIRECTORY "${CEF_RESOURCES_DIR}/${_cef_resource}")
      add_custom_command(TARGET universal_agent_manager POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CEF_RESOURCES_DIR}/${_cef_resource}"
          "$<TARGET_FILE_DIR:universal_agent_manager>/${_cef_resource}"
        COMMENT "Copying CEF resource directory ${_cef_resource}"
      )
    elseif(EXISTS "${CEF_RESOURCES_DIR}/${_cef_resource}")
      add_custom_command(TARGET universal_agent_manager POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CEF_RESOURCES_DIR}/${_cef_resource}"
          "$<TARGET_FILE_DIR:universal_agent_manager>/${_cef_resource}"
        COMMENT "Copying CEF resource file ${_cef_resource}"
      )
    endif()
  endforeach()
endif()

# Build + copy React UI bundle into the app bundle.
# On macOS: place in Contents/Resources/ so font assets (.woff etc.) don't
#           live beside the binary in Contents/MacOS/, which would cause
#           codesign to scan them as code objects and produce a broken signature.
# On Windows: flat layout next to the executable.
set(UAM_UI_DIST_SRC "${CMAKE_BINARY_DIR}/UI-V2/dist")
if(WIN32)
  unset(UAM_NPM_EXECUTABLE CACHE)
  find_program(UAM_NPM_EXECUTABLE NAMES npm.cmd REQUIRED)
else()
  find_program(UAM_NPM_EXECUTABLE NAMES npm REQUIRED)
endif()
string(TIMESTAMP UAM_UI_BUILD_TIMESTAMP "%Y%m%dT%H%M%SZ" UTC)
set(UAM_UI_BUILD_VERSION "${PROJECT_VERSION}")
if(UAM_UI_BUILD_VERSION STREQUAL "")
  set(UAM_UI_BUILD_VERSION "local")
endif()
set(UAM_UI_BUILD_ID "${UAM_UI_BUILD_VERSION}-${CMAKE_SYSTEM_NAME}-${UAM_UI_BUILD_TIMESTAMP}")

add_custom_target(uam_ui_v2_build
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/UI-V2"
  COMMAND ${CMAKE_COMMAND} -E env "VITE_UAM_UI_BUILD_ID=${UAM_UI_BUILD_ID}" "VITE_UAM_OUT_DIR=${UAM_UI_DIST_SRC}" ${UAM_NPM_EXECUTABLE} run build
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/UI-V2"
  COMMENT "Building UI-V2 dist in ${UAM_UI_DIST_SRC} (build id: ${UAM_UI_BUILD_ID})"
  VERBATIM
)
add_dependencies(universal_agent_manager uam_ui_v2_build)

if(APPLE AND UAM_MACOS_BUNDLE)
  add_custom_command(TARGET universal_agent_manager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove_directory
      "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/UI-V2/dist"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${UAM_UI_DIST_SRC}"
      "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/UI-V2/dist"
    COMMAND ${CMAKE_COMMAND} -E echo "Bundled UI-V2 build id: ${UAM_UI_BUILD_ID}"
    COMMENT "Copying UI-V2/dist to Contents/Resources/"
  )
else()
  add_custom_command(TARGET universal_agent_manager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove_directory
      "$<TARGET_FILE_DIR:universal_agent_manager>/UI-V2/dist"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${UAM_UI_DIST_SRC}"
      "$<TARGET_FILE_DIR:universal_agent_manager>/UI-V2/dist"
    COMMAND ${CMAKE_COMMAND} -E echo "Bundled UI-V2 build id: ${UAM_UI_BUILD_ID}"
    COMMENT "Copying UI-V2/dist to build output"
  )
endif()

# ---------------------------------------------------------------------------
# Window icon
# ---------------------------------------------------------------------------

set(UAM_WINDOW_ICON_BMP "${CMAKE_CURRENT_SOURCE_DIR}/assets/app_icon.bmp")
set(UAM_WINDOW_ICON_PNG "${CMAKE_CURRENT_SOURCE_DIR}/assets/app_icon.png")
if(APPLE AND UAM_MACOS_BUNDLE)
  add_custom_command(TARGET universal_agent_manager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f
      "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/MacOS/app_icon.bmp"
      "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/MacOS/app_icon.png"
    COMMENT "Removing stale executable-directory icon asset"
  )
endif()
if(EXISTS "${UAM_WINDOW_ICON_BMP}")
  if(APPLE AND UAM_MACOS_BUNDLE)
    add_custom_command(TARGET universal_agent_manager POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${UAM_WINDOW_ICON_BMP}"
        "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/app_icon.bmp"
    )
  else()
    add_custom_command(TARGET universal_agent_manager POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${UAM_WINDOW_ICON_BMP}"
        "$<TARGET_FILE_DIR:universal_agent_manager>/app_icon.bmp"
    )
  endif()
endif()
if(EXISTS "${UAM_WINDOW_ICON_PNG}")
  if(APPLE AND UAM_MACOS_BUNDLE)
    add_custom_command(TARGET universal_agent_manager POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${UAM_WINDOW_ICON_PNG}"
        "$<TARGET_BUNDLE_CONTENT_DIR:universal_agent_manager>/Resources/app_icon.png"
    )
  else()
    add_custom_command(TARGET universal_agent_manager POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${UAM_WINDOW_ICON_PNG}"
        "$<TARGET_FILE_DIR:universal_agent_manager>/app_icon.png"
    )
  endif()
endif()

if(APPLE AND UAM_MACOS_BUNDLE)
  add_custom_command(TARGET universal_agent_manager POST_BUILD
    COMMAND codesign --force --deep --sign - --entitlements "${_jit_entitlements}"
      "$<TARGET_BUNDLE_DIR:universal_agent_manager>"
    COMMAND codesign --verify --deep --strict --verbose=2
      "$<TARGET_BUNDLE_DIR:universal_agent_manager>"
    COMMENT "Signing and verifying final macOS app bundle"
  )
endif()

# ---------------------------------------------------------------------------
# Platform-specific settings
# ---------------------------------------------------------------------------

if(MSVC)
  set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT universal_agent_manager)
endif()

if(APPLE)
  target_compile_definitions(universal_agent_manager PRIVATE GL_SILENCE_DEPRECATION)
  if(UAM_MACOS_BUNDLE)
    set_target_properties(universal_agent_manager PROPERTIES
      MACOSX_BUNDLE_BUNDLE_NAME "Universal Agent Manager"
      MACOSX_BUNDLE_GUI_IDENTIFIER "com.universalagentmanager.desktop"
    )
  endif()
endif()

foreach(_cmake_target IN ITEMS ALL_BUILD ZERO_CHECK RUN_TESTS Experimental Nightly NightlyMemoryCheck Continuous)
  uam_set_target_folder(${_cmake_target} "_CMake")
endforeach()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(UAM_BUILD_TESTS)
  enable_testing()
  add_executable(uam_core_tests
    tests/core_tests.cpp
    ${UAM_PLATFORM_IMPL_SOURCES}
    src/app/application_core_helpers.cpp
    src/app/chat_domain_service.cpp
    src/app/chat_lifecycle_service.cpp
    src/app/native_session_link_service.cpp
    src/app/persistence_coordinator.cpp
    src/app/provider_resolution_service.cpp
    src/app/provider_profile_migration_service.cpp
    src/app/runtime_orchestration_services.cpp
    src/cef/uam_bridge_request.cpp
    src/cef/state_serializer.cpp
    src/common/models/app_models.cpp
    src/common/paths/app_paths.cpp
    src/common/chat/chat_folder_store.cpp
    src/common/config/frontend_actions.cpp
    src/common/utils/command_line_words.cpp
    src/common/chat/chat_repository.cpp
    src/common/chat/chat_branching.cpp
    src/common/provider/provider_profile.cpp
    src/common/provider/runtime/provider_runtime_facade.cpp
    src/common/provider/runtime/provider_runtime_registry.cpp
    src/common/runtime/acp/acp_session_runtime.cpp
    src/core/gemini_cli_compat.cpp
    ${UAM_PROVIDER_SOURCES}
    src/common/config/settings_store.cpp
    src/common/platform/platform_services.cpp
  )
  target_include_directories(uam_core_tests
    PRIVATE
      src
      src/common
      ${cef_binary_SOURCE_DIR}
      "${CMAKE_BINARY_DIR}/_deps/nlohmann_json-src/include"
  )
  if(APPLE)
    target_link_libraries(uam_core_tests PRIVATE
      libcef_dll_wrapper
      "-Wl,-framework,Security"
    )
  elseif(WIN32)
    target_link_libraries(uam_core_tests PRIVATE
      libcef_dll_wrapper
      "${cef_binary_SOURCE_DIR}/Release/libcef.lib"
    )
  endif()
  target_compile_definitions(uam_core_tests PRIVATE
    NOMINMAX
  )
  if(MSVC)
    set_property(TARGET uam_core_tests PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    set_target_properties(uam_core_tests PROPERTIES
      VS_GLOBAL_VcpkgEnabled "false"
      VS_GLOBAL_VcpkgApplocalDeps "false"
    )
  endif()
  set_target_properties(uam_core_tests PROPERTIES FOLDER "Tests")
  add_test(NAME uam_core_tests COMMAND uam_core_tests)
  add_test(
    NAME uam_platform_ifdef_guard
    COMMAND ${CMAKE_COMMAND} -DSOURCE_ROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/cmake/check_platform_ifdefs.cmake
  )
endif()

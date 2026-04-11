cmake_minimum_required(VERSION 3.20)
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
option(UAM_BUILD_UI_V2 "Build UI-V2 (React) before packaging/copying dist assets." ON)
set(UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED OFF)
set(UAM_ENABLE_RUNTIME_GEMINI_CLI ON)
set(UAM_ENABLE_RUNTIME_CODEX_CLI OFF)
set(UAM_ENABLE_RUNTIME_CLAUDE_CLI OFF)
set(UAM_ENABLE_RUNTIME_OPENCODE_CLI OFF)
set(UAM_ENABLE_RUNTIME_OPENCODE_LOCAL OFF)
set(UAM_ENABLE_RUNTIME_OLLAMA_ENGINE OFF)

set(UAM_ENABLE_ENGINE_RAG OFF)
set(UAM_ENABLE_ANY_GEMINI_PROVIDER ON)

set(UAM_OLLAMA_ENGINE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/subprojects/ollama_engine" CACHE PATH
  "Path to the ollama_engine source repository.")
set(UAM_THIRD_PARTY_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party" CACHE PATH
  "Directory that stores local third-party source checkouts.")
set(UAM_FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH
  "Directory used for FetchContent source checkouts.")
set(UAM_LLAMA_CPP_SOURCE_DIR "${UAM_THIRD_PARTY_SOURCE_DIR}/llama.cpp-uam" CACHE PATH
  "Path to a local llama.cpp fork source tree.")
if(APPLE)
  option(UAM_MACOS_BUNDLE "Build as a macOS .app bundle (no Terminal window needed)." ON)
endif()

set(UAM_LLAMA_CPP_GIT_TAG "990e4d96980d0b016a2b07049cc9031642fb9903" CACHE STRING
  "Pinned git commit for llama.cpp fork.")

file(MAKE_DIRECTORY "${UAM_FETCHCONTENT_BASE_DIR}")
set(FETCHCONTENT_BASE_DIR "${UAM_FETCHCONTENT_BASE_DIR}")

function(uam_set_target_folder target_name folder_name)
  if(TARGET ${target_name})
    set_property(TARGET ${target_name} PROPERTY FOLDER ${folder_name})
  endif()
endfunction()

# ---------------------------------------------------------------------------
# llama.cpp (disabled for the Gemini CLI-only slice)
# ---------------------------------------------------------------------------

option(UAM_FETCH_LLAMA_CPP "Fetch pinned llama.cpp fork during configure." ON)

if(UAM_ENABLE_RUNTIME_OLLAMA_ENGINE AND UAM_FETCH_LLAMA_CPP)
  set(GGML_CCACHE OFF CACHE BOOL "" FORCE)
  set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
  set(LLAMA_TOOLS_INSTALL OFF CACHE BOOL "" FORCE)
  set(LLAMA_TESTS_INSTALL OFF CACHE BOOL "" FORCE)
  set(LLAMA_OPENSSL OFF CACHE BOOL "" FORCE)

  set(CMAKE_FOLDER "ThirdParty/llama.cpp")
  if(EXISTS "${UAM_LLAMA_CPP_SOURCE_DIR}/CMakeLists.txt")
    add_subdirectory("${UAM_LLAMA_CPP_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/llama_cpp_uam")
  else()
    include(FetchContent)
    FetchContent_Declare(
      llama_cpp_uam
      URL "https://github.com/davidtaylor6130/llama.cpp-UAM-Fork/archive/${UAM_LLAMA_CPP_GIT_TAG}.tar.gz"
    )
    FetchContent_MakeAvailable(llama_cpp_uam)
    set(UAM_LLAMA_CPP_SOURCE_DIR "${llama_cpp_uam_SOURCE_DIR}" CACHE PATH
      "Path to a local llama.cpp fork source tree." FORCE)
  endif()
  set(CMAKE_FOLDER "")
endif()

# ---------------------------------------------------------------------------
# libcurl
# ---------------------------------------------------------------------------

include(FetchContent)
set(CMAKE_FOLDER "ThirdParty/curl")
FetchContent_Declare(
  curl
  URL https://github.com/curl/curl/archive/refs/tags/curl-8_11_1.tar.gz
)
FetchContent_MakeAvailable(curl)
set(UAM_CURL_SOURCE_DIR "${curl_SOURCE_DIR}" CACHE PATH
  "Path to a local curl source tree." FORCE)
set(CMAKE_FOLDER "")

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
# If built as a shared lib (llama.cpp sets BUILD_SHARED_LIBS=ON) the C++ wrapper
# symbols are hidden by -fvisibility=hidden and the linker cannot resolve them.
set(_saved_bsl ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cef_binary)
set(BUILD_SHARED_LIBS ${_saved_bsl} CACHE BOOL "" FORCE)
set(CMAKE_FOLDER "")

# FetchContent_MakeAvailable already processes the CEF CMakeLists.txt, which
# builds libcef_dll_wrapper and creates the libcef_dll_wrapper target.
# No explicit add_subdirectory needed.

# ---------------------------------------------------------------------------
# ollama engine (optional)
# ---------------------------------------------------------------------------

set(UAM_ENGINE_LINK_TARGET "")
if(UAM_ENABLE_RUNTIME_OLLAMA_ENGINE)
  if(EXISTS "${UAM_OLLAMA_ENGINE_SOURCE_DIR}/CMakeLists.txt")
    add_subdirectory("${UAM_OLLAMA_ENGINE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/ollama_engine")
    set(UAM_ENGINE_LINK_TARGET uam_ollama_engine)
  else()
    message(FATAL_ERROR "ollama_engine source not found at ${UAM_OLLAMA_ENGINE_SOURCE_DIR}.")
  endif()
else()
  message(STATUS "UAM_ENABLE_RUNTIME_OLLAMA_ENGINE=OFF: skipping ollama_engine subproject/link targets.")
endif()

# ---------------------------------------------------------------------------
# Provider sources
# ---------------------------------------------------------------------------

set(UAM_PROVIDER_SOURCES)

list(APPEND UAM_PROVIDER_SOURCES
  src/common/provider/gemini/cli/gemini_cli_provider_runtime.cpp
)

if(UAM_ENABLE_ANY_GEMINI_PROVIDER)
  list(APPEND UAM_PROVIDER_SOURCES
    src/common/provider/gemini/base/gemini_history_loader.cpp
  )
endif()

list(APPEND UAM_PROVIDER_SOURCES
  src/common/provider/custom/unknown_provider_runtime.cpp
)

set(UAM_RAG_SOURCES
  src/common/rag/rag_index_service.cpp
  src/common/rag/ollama_engine_client.cpp
  src/common/rag/ollama_engine_service.cpp
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
  src/app/native_session_link_service.cpp
  src/app/persistence_coordinator.cpp
  src/app/provider_resolution_service.cpp
  src/app/provider_profile_migration_service.cpp
  src/app/runtime_local_service.cpp
  src/app/runtime_orchestration_services.cpp
  src/app/template_runtime_service.cpp
  src/cef/uam_cef_app.cpp
  src/cef/uam_cef_client.cpp
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
  src/common/provider/markdown_template_catalog.cpp
  src/common/config/frontend_actions.cpp
  src/common/provider/provider_profile.cpp
  src/common/provider/runtime/provider_runtime_facade.cpp
  src/common/provider/runtime/provider_runtime_registry.cpp
  ${UAM_PROVIDER_SOURCES}
  src/common/runtime/local_engine_runtime_service.cpp
  src/common/runtime/provider_cli_compatibility_service.cpp
  src/common/platform/platform_services.cpp
  src/common/vcs/vcs_workspace_service.cpp
  ${UAM_RAG_SOURCES}
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
    CURL::libcurl
    ${UAM_ENGINE_LINK_TARGET}
    "-Wl,-framework,Security"
    "-Wl,-framework,WebKit"
  )
elseif(WIN32)
  target_link_libraries(universal_agent_manager PRIVATE
    libcef_dll_wrapper
    "${cef_binary_SOURCE_DIR}/Release/libcef.lib"
    CURL::libcurl
    ${UAM_ENGINE_LINK_TARGET}
  )
endif()

target_compile_definitions(universal_agent_manager PRIVATE
  NOMINMAX
  UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED=$<BOOL:${UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED}>
  UAM_ENABLE_RUNTIME_GEMINI_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_GEMINI_CLI}>
  UAM_ENABLE_RUNTIME_CODEX_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CODEX_CLI}>
  UAM_ENABLE_RUNTIME_CLAUDE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CLAUDE_CLI}>
  UAM_ENABLE_RUNTIME_OPENCODE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_OPENCODE_CLI}>
  UAM_ENABLE_RUNTIME_OPENCODE_LOCAL=$<BOOL:${UAM_ENABLE_RUNTIME_OPENCODE_LOCAL}>
  UAM_ENABLE_RUNTIME_OLLAMA_ENGINE=$<BOOL:${UAM_ENABLE_RUNTIME_OLLAMA_ENGINE}>
  UAM_ENABLE_ENGINE_RAG=$<BOOL:${UAM_ENABLE_ENGINE_RAG}>
  UAM_ENABLE_ANY_GEMINI_PROVIDER=$<BOOL:${UAM_ENABLE_ANY_GEMINI_PROVIDER}>
)

set_target_properties(universal_agent_manager PROPERTIES FOLDER "App")

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
  foreach(_dll
    libcef.dll
    chrome_elf.dll
    d3dcompiler_47.dll
    libEGL.dll
    libGLESv2.dll
    snapshot_blob.bin
    v8_context_snapshot.bin
    icudtl.dat
  )
    if(EXISTS "${CEF_RELEASE_DIR}/${_dll}")
      add_custom_command(TARGET universal_agent_manager POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CEF_RELEASE_DIR}/${_dll}"
          "$<TARGET_FILE_DIR:universal_agent_manager>/${_dll}"
      )
    endif()
  endforeach()

  set(CEF_RESOURCES_DIR "${cef_binary_SOURCE_DIR}/Resources")
  add_custom_command(TARGET universal_agent_manager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CEF_RESOURCES_DIR}"
      "$<TARGET_FILE_DIR:universal_agent_manager>/Resources"
    COMMENT "Copying CEF resources"
  )

  add_custom_command(TARGET universal_agent_manager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CEF_RELEASE_DIR}/locales"
      "$<TARGET_FILE_DIR:universal_agent_manager>/locales"
    COMMENT "Copying CEF locales"
  )
endif()

# Build + copy React UI bundle into the app bundle.
# On macOS: place in Contents/Resources/ so font assets (.woff etc.) don't
#           live beside the binary in Contents/MacOS/, which would cause
#           codesign to scan them as code objects and produce a broken signature.
# On Windows: flat layout next to the executable.
set(UAM_UI_DIST_SRC "${CMAKE_SOURCE_DIR}/UI-V2/dist")
set(UAM_UI_BUILD_ID "manual-dist")

if(UAM_BUILD_UI_V2)
  find_program(UAM_NPM_EXECUTABLE NAMES npm npm.cmd REQUIRED)
  string(TIMESTAMP UAM_UI_BUILD_TIMESTAMP "%Y%m%dT%H%M%SZ" UTC)
  set(UAM_UI_BUILD_VERSION "${PROJECT_VERSION}")
  if(UAM_UI_BUILD_VERSION STREQUAL "")
    set(UAM_UI_BUILD_VERSION "local")
  endif()
  set(UAM_UI_BUILD_ID "${UAM_UI_BUILD_VERSION}-${CMAKE_SYSTEM_NAME}-${UAM_UI_BUILD_TIMESTAMP}")

  file(GLOB_RECURSE UAM_UI_V2_SOURCE_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/UI-V2/src/*"
    "${CMAKE_SOURCE_DIR}/UI-V2/public/*"
    "${CMAKE_SOURCE_DIR}/UI-V2/index.html"
    "${CMAKE_SOURCE_DIR}/UI-V2/package.json"
    "${CMAKE_SOURCE_DIR}/UI-V2/package-lock.json"
    "${CMAKE_SOURCE_DIR}/UI-V2/tsconfig*.json"
    "${CMAKE_SOURCE_DIR}/UI-V2/vite.config.*"
    "${CMAKE_SOURCE_DIR}/UI-V2/postcss.config.*"
    "${CMAKE_SOURCE_DIR}/UI-V2/tailwind.config.*"
  )

  set(UAM_UI_BUILD_STAMP "${CMAKE_BINARY_DIR}/UI-V2/ui-v2-build.stamp")
  add_custom_command(
    OUTPUT "${UAM_UI_BUILD_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/UI-V2"
    COMMAND ${CMAKE_COMMAND} -E env "VITE_UAM_UI_BUILD_ID=${UAM_UI_BUILD_ID}" ${UAM_NPM_EXECUTABLE} run build
    COMMAND ${CMAKE_COMMAND} -E touch "${UAM_UI_BUILD_STAMP}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/UI-V2"
    DEPENDS ${UAM_UI_V2_SOURCE_FILES}
    COMMENT "Building UI-V2/dist (build id: ${UAM_UI_BUILD_ID})"
    VERBATIM
  )
  add_custom_target(uam_ui_v2_build DEPENDS "${UAM_UI_BUILD_STAMP}")
  add_dependencies(universal_agent_manager uam_ui_v2_build)
elseif(NOT EXISTS "${UAM_UI_DIST_SRC}/index.html")
  message(FATAL_ERROR "UI-V2/dist not found and UAM_BUILD_UI_V2=OFF. Enable UAM_BUILD_UI_V2 or run 'npm run build' in UI-V2/.")
endif()

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
# ollama bridge helper copy
# ---------------------------------------------------------------------------

if(TARGET uam_ollama_engine_bridge)
  add_dependencies(universal_agent_manager uam_ollama_engine_bridge)
  add_custom_command(TARGET universal_agent_manager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:uam_ollama_engine_bridge>"
      "$<TARGET_FILE_DIR:universal_agent_manager>/"
    COMMENT "Copying OpenCode local bridge next to universal_agent_manager"
  )
endif()

# ---------------------------------------------------------------------------
# Window icon
# ---------------------------------------------------------------------------

set(UAM_WINDOW_ICON_BMP "${CMAKE_CURRENT_SOURCE_DIR}/assets/app_icon.bmp")
if(EXISTS "${UAM_WINDOW_ICON_BMP}")
  add_custom_command(TARGET universal_agent_manager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${UAM_WINDOW_ICON_BMP}"
      "$<TARGET_FILE_DIR:universal_agent_manager>/app_icon.bmp"
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
    src/app/native_session_link_service.cpp
    src/app/persistence_coordinator.cpp
    src/app/provider_resolution_service.cpp
    src/app/provider_profile_migration_service.cpp
    src/app/runtime_local_service.cpp
    src/app/runtime_orchestration_services.cpp
    src/app/template_runtime_service.cpp
    src/cef/state_serializer.cpp
    src/common/models/app_models.cpp
    src/common/paths/app_paths.cpp
    src/common/chat/chat_folder_store.cpp
    src/common/config/frontend_actions.cpp
    src/common/utils/command_line_words.cpp
    src/common/chat/chat_repository.cpp
    src/common/chat/chat_branching.cpp
    src/common/provider/markdown_template_catalog.cpp
    src/common/provider/provider_profile.cpp
    src/common/provider/runtime/provider_runtime_facade.cpp
    src/common/provider/runtime/provider_runtime_registry.cpp
    ${UAM_PROVIDER_SOURCES}
    src/common/config/settings_store.cpp
    src/common/runtime/local_engine_runtime_service.cpp
    src/common/platform/platform_services.cpp
    src/common/vcs/vcs_workspace_service.cpp
    ${UAM_RAG_SOURCES}
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
      CURL::libcurl
      ${UAM_ENGINE_LINK_TARGET}
      "-Wl,-framework,Security"
    )
  elseif(WIN32)
    target_link_libraries(uam_core_tests PRIVATE
      libcef_dll_wrapper
      "${cef_binary_SOURCE_DIR}/Release/libcef.lib"
      CURL::libcurl
      ${UAM_ENGINE_LINK_TARGET}
    )
  endif()
  target_compile_definitions(uam_core_tests PRIVATE
    NOMINMAX
    UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED=$<BOOL:${UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED}>
    UAM_ENABLE_RUNTIME_GEMINI_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_GEMINI_CLI}>
    UAM_ENABLE_RUNTIME_CODEX_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CODEX_CLI}>
    UAM_ENABLE_RUNTIME_CLAUDE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CLAUDE_CLI}>
    UAM_ENABLE_RUNTIME_OPENCODE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_OPENCODE_CLI}>
    UAM_ENABLE_RUNTIME_OPENCODE_LOCAL=$<BOOL:${UAM_ENABLE_RUNTIME_OPENCODE_LOCAL}>
    UAM_ENABLE_RUNTIME_OLLAMA_ENGINE=$<BOOL:${UAM_ENABLE_RUNTIME_OLLAMA_ENGINE}>
    UAM_ENABLE_ENGINE_RAG=$<BOOL:${UAM_ENABLE_ENGINE_RAG}>
    UAM_ENABLE_ANY_GEMINI_PROVIDER=$<BOOL:${UAM_ENABLE_ANY_GEMINI_PROVIDER}>
  )
  set_target_properties(uam_core_tests PROPERTIES FOLDER "Tests")
  add_test(NAME uam_core_tests COMMAND uam_core_tests)
  add_test(
    NAME uam_platform_ifdef_guard
    COMMAND ${CMAKE_COMMAND} -DSOURCE_ROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/cmake/check_platform_ifdefs.cmake
  )
endif()

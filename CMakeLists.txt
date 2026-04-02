cmake_minimum_required(VERSION 3.20)
project(universal_agent_manager LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_SUPPRESS_REGENERATION ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "_CMake")

set(UAM_REQUIRED_BUILD_ROOT "${CMAKE_SOURCE_DIR}/Builds")
file(MAKE_DIRECTORY "${UAM_REQUIRED_BUILD_ROOT}")
file(REAL_PATH "${UAM_REQUIRED_BUILD_ROOT}" UAM_REQUIRED_BUILD_ROOT_REAL)
file(REAL_PATH "${CMAKE_BINARY_DIR}" UAM_BINARY_DIR_REAL)
cmake_path(
  RELATIVE_PATH UAM_BINARY_DIR_REAL
  BASE_DIRECTORY "${UAM_REQUIRED_BUILD_ROOT_REAL}"
  OUTPUT_VARIABLE UAM_BINARY_DIR_RELATIVE_TO_REQUIRED_ROOT
)
if(UAM_BINARY_DIR_RELATIVE_TO_REQUIRED_ROOT MATCHES "^\\.\\.")
  message(FATAL_ERROR
    "Build directory must be inside '${UAM_REQUIRED_BUILD_ROOT}'.\n"
    "Use one of:\n"
    "  cmake -S . -B Builds\n"
    "  cmake -S . -B Builds/<name>\n"
  )
endif()

option(UAM_FETCH_DEPS "Fetch SDL2 and Dear ImGui from GitHub during configure." ON)
option(UAM_FETCH_LLAMA_CPP "Fetch pinned llama.cpp fork during configure." ON)
option(UAM_BUILD_TESTS "Build UAM core tests executable." OFF)
option(UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED "Enable gemini-structured runtime backend." ON)
option(UAM_ENABLE_RUNTIME_GEMINI_CLI "Enable gemini-cli runtime backend." ON)
option(UAM_ENABLE_RUNTIME_CODEX_CLI "Enable codex-cli runtime backend." ON)
option(UAM_ENABLE_RUNTIME_CLAUDE_CLI "Enable claude-cli runtime backend." ON)
option(UAM_ENABLE_RUNTIME_OPENCODE_CLI "Enable opencode-cli runtime backend." ON)
option(UAM_ENABLE_RUNTIME_OPENCODE_LOCAL "Enable opencode-local runtime backend." ON)
option(UAM_ENABLE_RUNTIME_OLLAMA_ENGINE "Enable ollama-engine runtime backend." ON)

if(NOT UAM_ENABLE_RUNTIME_OLLAMA_ENGINE AND UAM_ENABLE_RUNTIME_OPENCODE_LOCAL)
  message(STATUS "UAM_ENABLE_RUNTIME_OLLAMA_ENGINE=OFF, forcing UAM_ENABLE_RUNTIME_OPENCODE_LOCAL=OFF.")
  set(UAM_ENABLE_RUNTIME_OPENCODE_LOCAL OFF CACHE BOOL "Enable opencode-local runtime backend." FORCE)
endif()

if(UAM_ENABLE_RUNTIME_OPENCODE_LOCAL AND NOT UAM_ENABLE_RUNTIME_OPENCODE_CLI)
  message(FATAL_ERROR "UAM_ENABLE_RUNTIME_OPENCODE_LOCAL requires UAM_ENABLE_RUNTIME_OPENCODE_CLI=ON.")
endif()

if(UAM_ENABLE_RUNTIME_OPENCODE_LOCAL AND NOT UAM_ENABLE_RUNTIME_OLLAMA_ENGINE)
  message(FATAL_ERROR "UAM_ENABLE_RUNTIME_OPENCODE_LOCAL requires UAM_ENABLE_RUNTIME_OLLAMA_ENGINE=ON.")
endif()

set(_uam_runtime_enabled_count 0)
foreach(_runtime_toggle
  UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED
  UAM_ENABLE_RUNTIME_GEMINI_CLI
  UAM_ENABLE_RUNTIME_CODEX_CLI
  UAM_ENABLE_RUNTIME_CLAUDE_CLI
  UAM_ENABLE_RUNTIME_OPENCODE_CLI
  UAM_ENABLE_RUNTIME_OPENCODE_LOCAL
  UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
)
  if(${_runtime_toggle})
    math(EXPR _uam_runtime_enabled_count "${_uam_runtime_enabled_count} + 1")
  endif()
endforeach()

if(_uam_runtime_enabled_count EQUAL 0)
  message(FATAL_ERROR "At least one UAM runtime backend must be enabled.")
endif()

set(UAM_ENABLE_ENGINE_RAG ${UAM_ENABLE_RUNTIME_OLLAMA_ENGINE})
set(UAM_OLLAMA_ENGINE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/subprojects/ollama_engine" CACHE PATH
  "Path to the ollama_engine source repository.")
set(UAM_THIRD_PARTY_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party" CACHE PATH
  "Directory that stores local third-party source checkouts.")
set(UAM_FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH
  "Directory used for FetchContent source checkouts.")
set(UAM_SDL2_SOURCE_DIR "${UAM_THIRD_PARTY_SOURCE_DIR}/SDL" CACHE PATH
  "Path to a local SDL source tree.")
set(UAM_IMGUI_SOURCE_DIR "${UAM_THIRD_PARTY_SOURCE_DIR}/imgui" CACHE PATH
  "Path to a local Dear ImGui source tree.")
set(UAM_LLAMA_CPP_SOURCE_DIR "${UAM_THIRD_PARTY_SOURCE_DIR}/llama.cpp-uam" CACHE PATH
  "Path to a local llama.cpp fork source tree.")
if(APPLE)
  option(UAM_MACOS_BUNDLE "Build as a macOS .app bundle (no Terminal window needed)." ON)
endif()

set(UAM_LLAMA_CPP_REPOSITORY "https://github.com/davidtaylor6130/llama.cpp-UAM-Fork.git" CACHE STRING
  "Git repository URL for llama.cpp fork.")
set(UAM_LLAMA_CPP_GIT_TAG "990e4d96980d0b016a2b07049cc9031642fb9903" CACHE STRING
  "Pinned git commit for llama.cpp fork.")

file(MAKE_DIRECTORY "${UAM_FETCHCONTENT_BASE_DIR}")
set(FETCHCONTENT_BASE_DIR "${UAM_FETCHCONTENT_BASE_DIR}")

function(uam_set_target_folder target_name folder_name)
  if(TARGET ${target_name})
    set_property(TARGET ${target_name} PROPERTY FOLDER ${folder_name})
  endif()
endfunction()

if(UAM_FETCH_LLAMA_CPP)
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
      GIT_REPOSITORY ${UAM_LLAMA_CPP_REPOSITORY}
      GIT_TAG ${UAM_LLAMA_CPP_GIT_TAG}
      GIT_SHALLOW TRUE
      GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(llama_cpp_uam)
    set(UAM_LLAMA_CPP_SOURCE_DIR "${llama_cpp_uam_SOURCE_DIR}" CACHE PATH
      "Path to a local llama.cpp fork source tree." FORCE)
  endif()
  set(CMAKE_FOLDER "")
endif()

if(UAM_FETCH_DEPS)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  set(SDL_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
  set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)

  set(CMAKE_FOLDER "ThirdParty/SDL2")
  if(EXISTS "${UAM_SDL2_SOURCE_DIR}/CMakeLists.txt")
    add_subdirectory("${UAM_SDL2_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/sdl2")
  else()
    include(FetchContent)
    FetchContent_Declare(
      sdl2
      GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
      GIT_TAG release-2.30.11
    )
    FetchContent_MakeAvailable(sdl2)
    set(UAM_SDL2_SOURCE_DIR "${sdl2_SOURCE_DIR}" CACHE PATH
      "Path to a local SDL source tree." FORCE)
  endif()

  set(CMAKE_FOLDER "ThirdParty/ImGui")
  if(EXISTS "${UAM_IMGUI_SOURCE_DIR}/imgui.cpp")
    set(IMGUI_DIR "${UAM_IMGUI_SOURCE_DIR}")
  else()
    include(FetchContent)
    FetchContent_Declare(
      imgui
      GIT_REPOSITORY https://github.com/ocornut/imgui.git
      GIT_TAG v1.91.8
    )
    FetchContent_MakeAvailable(imgui)
    set(IMGUI_DIR "${imgui_SOURCE_DIR}")
    set(UAM_IMGUI_SOURCE_DIR "${imgui_SOURCE_DIR}" CACHE PATH
      "Path to a local Dear ImGui source tree." FORCE)
  endif()
  set(CMAKE_FOLDER "")
else()
  find_package(SDL2 REQUIRED)
  set(IMGUI_DIR "${UAM_IMGUI_SOURCE_DIR}" CACHE PATH "Path to Dear ImGui source directory")
  if(IMGUI_DIR STREQUAL "" OR NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR "Set IMGUI_DIR (or UAM_IMGUI_SOURCE_DIR) to a local Dear ImGui source directory when UAM_FETCH_DEPS=OFF.")
  endif()
endif()

find_package(OpenGL REQUIRED)
find_package(CURL REQUIRED)

if(TARGET SDL2::SDL2)
  set(UAM_SDL_TARGET SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
  set(UAM_SDL_TARGET SDL2::SDL2-static)
else()
  message(FATAL_ERROR "Could not find SDL2 target.")
endif()

set(UAM_IMGUI_SOURCES
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

set(VTERM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libvterm")
set(UAM_VTERM_SOURCES
  ${VTERM_DIR}/src/encoding.c
  ${VTERM_DIR}/src/keyboard.c
  ${VTERM_DIR}/src/mouse.c
  ${VTERM_DIR}/src/parser.c
  ${VTERM_DIR}/src/pen.c
  ${VTERM_DIR}/src/screen.c
  ${VTERM_DIR}/src/state.c
  ${VTERM_DIR}/src/unicode.c
  ${VTERM_DIR}/src/vterm.c
)

set(UAM_EXECUTABLE_KIND "")
if(APPLE AND UAM_MACOS_BUNDLE)
  set(UAM_EXECUTABLE_KIND MACOSX_BUNDLE)
endif()

set(UAM_PLATFORM_SOURCES)
if(WIN32)
  list(APPEND UAM_PLATFORM_SOURCES
    src/uam.rc
  )
endif()

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

add_executable(universal_agent_manager ${UAM_EXECUTABLE_KIND}
  ${UAM_IMGUI_SOURCES}
  ${UAM_VTERM_SOURCES}
  ${UAM_PLATFORM_SOURCES}
  src/main.cpp
  src/common/models/app_models.cpp
  src/common/paths/app_paths.cpp
  src/common/chat/chat_folder_store.cpp
  src/common/chat/chat_repository.cpp
  src/common/chat/local_chat_store.cpp
  src/common/chat/gemini_native_history_store.cpp
  src/common/chat/chat_branching.cpp
  src/common/config/settings_store.cpp
  src/common/utils/command_line_words.cpp
  src/common/provider/gemini_command_builder.cpp
  src/common/provider/gemini_template_catalog.cpp
  src/common/config/frontend_actions.cpp
  src/common/provider/provider_profile.cpp
  src/common/provider/provider_runtime.cpp
  src/common/platform/platform_services.cpp
  src/common/vcs/vcs_workspace_service.cpp
  src/common/rag/rag_index_service.cpp
  src/common/rag/ollama_engine_client.cpp
  src/common/rag/ollama_engine_service.cpp
  src/common/provider/provider_profiles.cpp
)
target_include_directories(universal_agent_manager
  PRIVATE
    src
    src/common
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUI_DIR}/misc/cpp
    ${VTERM_DIR}/include
    ${VTERM_DIR}/src
)
target_link_libraries(universal_agent_manager PRIVATE ${UAM_SDL_TARGET} OpenGL::GL CURL::libcurl ${UAM_ENGINE_LINK_TARGET})
target_compile_definitions(universal_agent_manager PRIVATE
  UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED=$<BOOL:${UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED}>
  UAM_ENABLE_RUNTIME_GEMINI_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_GEMINI_CLI}>
  UAM_ENABLE_RUNTIME_CODEX_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CODEX_CLI}>
  UAM_ENABLE_RUNTIME_CLAUDE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CLAUDE_CLI}>
  UAM_ENABLE_RUNTIME_OPENCODE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_OPENCODE_CLI}>
  UAM_ENABLE_RUNTIME_OPENCODE_LOCAL=$<BOOL:${UAM_ENABLE_RUNTIME_OPENCODE_LOCAL}>
  UAM_ENABLE_RUNTIME_OLLAMA_ENGINE=$<BOOL:${UAM_ENABLE_RUNTIME_OLLAMA_ENGINE}>
  UAM_ENABLE_ENGINE_RAG=$<BOOL:${UAM_ENABLE_ENGINE_RAG}>
)
set_target_properties(universal_agent_manager PROPERTIES FOLDER "App")
if(TARGET uam_ollama_engine_bridge)
  add_dependencies(universal_agent_manager uam_ollama_engine_bridge)
  add_custom_command(TARGET universal_agent_manager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:uam_ollama_engine_bridge>"
      "$<TARGET_FILE_DIR:universal_agent_manager>/"
    COMMENT "Copying OpenCode local bridge next to universal_agent_manager"
  )
endif()

set(UAM_WINDOW_ICON_BMP "${CMAKE_CURRENT_SOURCE_DIR}/assets/app_icon.bmp")
if(EXISTS "${UAM_WINDOW_ICON_BMP}")
  add_custom_command(TARGET universal_agent_manager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${UAM_WINDOW_ICON_BMP}"
      "$<TARGET_FILE_DIR:universal_agent_manager>/app_icon.bmp"
  )
endif()

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

uam_set_target_folder(SDL2-static "ThirdParty/SDL2")
foreach(_sdl_internal_target IN ITEMS SDL2main sdl_headers_copy uninstall)
  uam_set_target_folder(${_sdl_internal_target} "ThirdParty/SDL2/Internal")
endforeach()
foreach(_cmake_target IN ITEMS ALL_BUILD ZERO_CHECK RUN_TESTS Experimental Nightly NightlyMemoryCheck Continuous)
  uam_set_target_folder(${_cmake_target} "_CMake")
endforeach()

if(UAM_BUILD_TESTS)
  enable_testing()
  add_executable(uam_core_tests
    tests/core_tests.cpp
    src/common/models/app_models.cpp
    src/common/paths/app_paths.cpp
    src/common/config/frontend_actions.cpp
    src/common/utils/command_line_words.cpp
    src/common/provider/gemini_command_builder.cpp
    src/common/provider/gemini_template_catalog.cpp
    src/common/chat/chat_repository.cpp
    src/common/chat/local_chat_store.cpp
    src/common/chat/gemini_native_history_store.cpp
    src/common/chat/chat_branching.cpp
    src/common/provider/provider_profile.cpp
    src/common/provider/provider_runtime.cpp
    src/common/config/settings_store.cpp
    src/common/platform/platform_services.cpp
    src/common/vcs/vcs_workspace_service.cpp
    src/common/rag/rag_index_service.cpp
    src/common/rag/ollama_engine_client.cpp
    src/common/rag/ollama_engine_service.cpp
    src/common/provider/provider_profiles.cpp
  )
  target_include_directories(uam_core_tests
    PRIVATE
      src
      src/common
      ${VTERM_DIR}/include
      ${VTERM_DIR}/src
      $<TARGET_PROPERTY:${UAM_SDL_TARGET},INTERFACE_INCLUDE_DIRECTORIES>
  )
  target_link_libraries(uam_core_tests PRIVATE ${UAM_SDL_TARGET} ${UAM_ENGINE_LINK_TARGET})
  target_compile_definitions(uam_core_tests PRIVATE
    UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED=$<BOOL:${UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED}>
    UAM_ENABLE_RUNTIME_GEMINI_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_GEMINI_CLI}>
    UAM_ENABLE_RUNTIME_CODEX_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CODEX_CLI}>
    UAM_ENABLE_RUNTIME_CLAUDE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_CLAUDE_CLI}>
    UAM_ENABLE_RUNTIME_OPENCODE_CLI=$<BOOL:${UAM_ENABLE_RUNTIME_OPENCODE_CLI}>
    UAM_ENABLE_RUNTIME_OPENCODE_LOCAL=$<BOOL:${UAM_ENABLE_RUNTIME_OPENCODE_LOCAL}>
    UAM_ENABLE_RUNTIME_OLLAMA_ENGINE=$<BOOL:${UAM_ENABLE_RUNTIME_OLLAMA_ENGINE}>
    UAM_ENABLE_ENGINE_RAG=$<BOOL:${UAM_ENABLE_ENGINE_RAG}>
  )
  set_target_properties(uam_core_tests PROPERTIES FOLDER "Tests")
  add_test(NAME uam_core_tests COMMAND uam_core_tests)
  add_test(
    NAME uam_platform_ifdef_guard
    COMMAND ${CMAKE_COMMAND} -DSOURCE_ROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/cmake/check_platform_ifdefs.cmake
  )
endif()

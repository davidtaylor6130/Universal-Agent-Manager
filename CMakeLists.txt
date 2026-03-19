cmake_minimum_required(VERSION 3.20)
project(universal_agent_manager LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_SUPPRESS_REGENERATION ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "_CMake")

option(UAM_FETCH_DEPS "Fetch SDL2 and Dear ImGui from GitHub during configure." ON)
option(UAM_BUILD_TESTS "Build UAM core tests executable." OFF)
if(APPLE)
  option(UAM_MACOS_BUNDLE "Build as a macOS .app bundle (no Terminal window needed)." ON)
endif()

function(uam_set_target_folder target_name folder_name)
  if(TARGET ${target_name})
    set_property(TARGET ${target_name} PROPERTY FOLDER ${folder_name})
  endif()
endfunction()

if(UAM_FETCH_DEPS)
  include(FetchContent)

  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  set(SDL_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
  set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)

  set(CMAKE_FOLDER "ThirdParty/SDL2")
  FetchContent_Declare(
    sdl2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.11
  )
  FetchContent_MakeAvailable(sdl2)

  set(CMAKE_FOLDER "ThirdParty/ImGui")
  FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.8
  )
  FetchContent_MakeAvailable(imgui)
  set(CMAKE_FOLDER "")

  set(IMGUI_DIR "${imgui_SOURCE_DIR}")
else()
  find_package(SDL2 REQUIRED)
  set(IMGUI_DIR "" CACHE PATH "Path to Dear ImGui source directory")
  if(IMGUI_DIR STREQUAL "")
    message(FATAL_ERROR "Set IMGUI_DIR when UAM_FETCH_DEPS=OFF.")
  endif()
endif()

find_package(OpenGL REQUIRED)

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

add_executable(universal_agent_manager ${UAM_EXECUTABLE_KIND}
  ${UAM_IMGUI_SOURCES}
  ${UAM_VTERM_SOURCES}
  src/main.cpp
  src/core/app_models.cpp
  src/core/app_paths.cpp
  src/core/chat_folder_store.cpp
  src/core/chat_repository.cpp
  src/core/settings_store.cpp
  src/core/gemini_command_builder.cpp
  src/core/gemini_template_catalog.cpp
  src/core/frontend_actions.cpp
  src/core/provider_profile.cpp
  src/core/provider_runtime.cpp
)
target_include_directories(universal_agent_manager
  PRIVATE
    src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUI_DIR}/misc/cpp
    ${VTERM_DIR}/include
    ${VTERM_DIR}/src
)
target_link_libraries(universal_agent_manager PRIVATE ${UAM_SDL_TARGET} OpenGL::GL)
set_target_properties(universal_agent_manager PROPERTIES FOLDER "App")

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
    src/core/app_models.cpp
    src/core/app_paths.cpp
    src/core/frontend_actions.cpp
    src/core/gemini_command_builder.cpp
    src/core/gemini_template_catalog.cpp
    src/core/chat_repository.cpp
    src/core/provider_profile.cpp
    src/core/provider_runtime.cpp
    src/core/settings_store.cpp
  )
  target_include_directories(uam_core_tests PRIVATE src)
  set_target_properties(uam_core_tests PROPERTIES FOLDER "Tests")
  add_test(NAME uam_core_tests COMMAND uam_core_tests)
endif()

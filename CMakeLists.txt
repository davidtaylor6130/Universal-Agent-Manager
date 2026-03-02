cmake_minimum_required(VERSION 3.20)
project(universal_agent_manager LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(UAM_FETCH_DEPS "Fetch SDL2 and Dear ImGui from GitHub during configure." ON)
if(APPLE)
  option(UAM_MACOS_BUNDLE "Build as a macOS .app bundle (no Terminal window needed)." ON)
endif()

if(UAM_FETCH_DEPS)
  include(FetchContent)

  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  FetchContent_Declare(
    sdl2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.11
  )
  FetchContent_MakeAvailable(sdl2)

  FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.8
  )
  FetchContent_MakeAvailable(imgui)

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

add_library(imgui_core STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_core
  PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUI_DIR}/misc/cpp
)
target_link_libraries(imgui_core PUBLIC ${UAM_SDL_TARGET} OpenGL::GL)

set(VTERM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libvterm")
add_library(vterm_core STATIC
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
target_include_directories(vterm_core
  PUBLIC
    ${VTERM_DIR}/include
  PRIVATE
    ${VTERM_DIR}/src
)

set(UAM_EXECUTABLE_KIND "")
if(APPLE AND UAM_MACOS_BUNDLE)
  set(UAM_EXECUTABLE_KIND MACOSX_BUNDLE)
endif()

add_executable(universal_agent_manager ${UAM_EXECUTABLE_KIND}
  src/main.cpp
  src/core/app_models.cpp
  src/core/app_paths.cpp
  src/core/chat_folder_store.cpp
  src/core/chat_repository.cpp
  src/core/settings_store.cpp
  src/core/gemini_command_builder.cpp
)
target_include_directories(universal_agent_manager PRIVATE src)
target_link_libraries(universal_agent_manager PRIVATE imgui_core vterm_core ${UAM_SDL_TARGET} OpenGL::GL)

if(APPLE)
  target_compile_definitions(universal_agent_manager PRIVATE GL_SILENCE_DEPRECATION)
  if(UAM_MACOS_BUNDLE)
    set_target_properties(universal_agent_manager PROPERTIES
      MACOSX_BUNDLE_BUNDLE_NAME "Universal Agent Manager"
      MACOSX_BUNDLE_GUI_IDENTIFIER "com.universalagentmanager.desktop"
    )
  endif()
endif()

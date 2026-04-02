cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(ollama_engine LANGUAGES C CXX)
endif()

set(_uam_default_fetch_llama ON)
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_uam_default_fetch_llama OFF)
endif()
option(UAM_FETCH_LLAMA_CPP "Fetch pinned llama.cpp fork during configure." ${_uam_default_fetch_llama})

set(UAM_OLLAMA_THIRD_PARTY_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party" CACHE PATH
  "Directory that stores local ollama_engine third-party source checkouts.")
set(UAM_LLAMA_CPP_SOURCE_DIR "${UAM_OLLAMA_THIRD_PARTY_SOURCE_DIR}/llama.cpp-uam" CACHE PATH
  "Path to a local llama.cpp fork source tree.")
set(UAM_TREE_SITTER_SOURCE_DIR "${UAM_OLLAMA_THIRD_PARTY_SOURCE_DIR}/tree-sitter" CACHE PATH
  "Path to a local tree-sitter source tree.")
set(UAM_TREE_SITTER_CPP_SOURCE_DIR "${UAM_OLLAMA_THIRD_PARTY_SOURCE_DIR}/tree-sitter-cpp" CACHE PATH
  "Path to a local tree-sitter-cpp source tree.")
set(UAM_CPP_HTTPLIB_SOURCE_DIR "${UAM_OLLAMA_THIRD_PARTY_SOURCE_DIR}/cpp-httplib" CACHE PATH
  "Path to a local cpp-httplib source tree.")
set(UAM_LLAMA_CPP_REPOSITORY "https://github.com/davidtaylor6130/llama.cpp-UAM-Fork.git" CACHE STRING
  "Git repository URL for llama.cpp fork.")
set(UAM_LLAMA_CPP_GIT_TAG "990e4d96980d0b016a2b07049cc9031642fb9903" CACHE STRING
  "Pinned git commit for llama.cpp fork.")

set(_uam_fetch_tree_sitter FALSE)
set(_uam_fetch_tree_sitter_cpp FALSE)
set(_uam_fetch_cpp_httplib FALSE)

if(UAM_FETCH_LLAMA_CPP AND NOT TARGET llama)
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

  include(FetchContent)
  if(EXISTS "${UAM_LLAMA_CPP_SOURCE_DIR}/CMakeLists.txt")
    add_subdirectory("${UAM_LLAMA_CPP_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/llama_cpp_uam")
  else()
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
endif()

if(NOT EXISTS "${UAM_TREE_SITTER_SOURCE_DIR}/lib/src/lib.c")
  set(_uam_fetch_tree_sitter TRUE)
endif()
if(NOT EXISTS "${UAM_TREE_SITTER_CPP_SOURCE_DIR}/src/parser.c")
  set(_uam_fetch_tree_sitter_cpp TRUE)
endif()
if(NOT EXISTS "${UAM_CPP_HTTPLIB_SOURCE_DIR}/httplib.h")
  set(_uam_fetch_cpp_httplib TRUE)
endif()

if(_uam_fetch_tree_sitter OR _uam_fetch_tree_sitter_cpp OR _uam_fetch_cpp_httplib)
  include(FetchContent)
endif()

if(_uam_fetch_tree_sitter)
  FetchContent_Declare(
    tree_sitter
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
    GIT_TAG v0.22.6
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(tree_sitter)
  set(UAM_TREE_SITTER_SOURCE_DIR "${tree_sitter_SOURCE_DIR}" CACHE PATH
    "Path to a local tree-sitter source tree." FORCE)
endif()

if(_uam_fetch_tree_sitter_cpp)
  FetchContent_Declare(
    tree_sitter_cpp
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-cpp.git
    GIT_TAG v0.22.3
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(tree_sitter_cpp)
  set(UAM_TREE_SITTER_CPP_SOURCE_DIR "${tree_sitter_cpp_SOURCE_DIR}" CACHE PATH
    "Path to a local tree-sitter-cpp source tree." FORCE)
endif()

if(_uam_fetch_cpp_httplib)
  FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.20.1
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(cpp_httplib)
  set(UAM_CPP_HTTPLIB_SOURCE_DIR "${cpp_httplib_SOURCE_DIR}" CACHE PATH
    "Path to a local cpp-httplib source tree." FORCE)
elseif(EXISTS "${UAM_CPP_HTTPLIB_SOURCE_DIR}/CMakeLists.txt")
  add_subdirectory("${UAM_CPP_HTTPLIB_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/cpp_httplib")
else()
  add_library(httplib INTERFACE)
  target_include_directories(httplib INTERFACE "${UAM_CPP_HTTPLIB_SOURCE_DIR}")
endif()

if(NOT TARGET httplib::httplib)
  if(TARGET httplib)
    add_library(httplib::httplib ALIAS httplib)
  else()
    message(FATAL_ERROR "Could not create httplib::httplib target from ${UAM_CPP_HTTPLIB_SOURCE_DIR}.")
  endif()
endif()

set(UAM_TREE_SITTER_CPP_SOURCES
  ${UAM_TREE_SITTER_CPP_SOURCE_DIR}/src/parser.c
)
if(EXISTS "${UAM_TREE_SITTER_CPP_SOURCE_DIR}/src/scanner.c")
  list(APPEND UAM_TREE_SITTER_CPP_SOURCES ${UAM_TREE_SITTER_CPP_SOURCE_DIR}/src/scanner.c)
elseif(EXISTS "${UAM_TREE_SITTER_CPP_SOURCE_DIR}/src/scanner.cc")
  list(APPEND UAM_TREE_SITTER_CPP_SOURCES ${UAM_TREE_SITTER_CPP_SOURCE_DIR}/src/scanner.cc)
endif()

add_library(uam_tree_sitter_runtime STATIC
  ${UAM_TREE_SITTER_SOURCE_DIR}/lib/src/lib.c
)
target_include_directories(uam_tree_sitter_runtime
  PUBLIC
    ${UAM_TREE_SITTER_SOURCE_DIR}/lib/include
)
set_target_properties(uam_tree_sitter_runtime PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  FOLDER "Subprojects/OllamaEngine/ThirdParty"
)

add_library(uam_tree_sitter_cpp_grammar STATIC
  ${UAM_TREE_SITTER_CPP_SOURCES}
)
target_include_directories(uam_tree_sitter_cpp_grammar
  PUBLIC
    ${UAM_TREE_SITTER_SOURCE_DIR}/lib/include
    ${UAM_TREE_SITTER_CPP_SOURCE_DIR}/src
)
target_link_libraries(uam_tree_sitter_cpp_grammar PUBLIC uam_tree_sitter_runtime)
set_target_properties(uam_tree_sitter_cpp_grammar PROPERTIES
  FOLDER "Subprojects/OllamaEngine/ThirdParty"
)

find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(uam_ollama_engine STATIC
  src/engine_factory.cpp
  src/internal/determanistic_hash/determanistic_hash.cpp
  src/internal/embedding_utils.cpp
  src/internal/local_ollama_engine.cpp
  src/internal/vectorised_rag/vectorised_rag.cpp
)

target_include_directories(uam_ollama_engine
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${UAM_TREE_SITTER_SOURCE_DIR}/lib/include
    ${UAM_TREE_SITTER_CPP_SOURCE_DIR}/src
)

target_link_libraries(uam_ollama_engine
  PRIVATE
    SQLite::SQLite3
    uam_tree_sitter_runtime
    uam_tree_sitter_cpp_grammar
)

if(TARGET llama)
  target_link_libraries(uam_ollama_engine PRIVATE llama)
  target_compile_definitions(uam_ollama_engine PRIVATE UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP=1)
endif()

set_target_properties(uam_ollama_engine PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  FOLDER "Subprojects/OllamaEngine"
)

add_library(uam_ollama_engine_cli_common STATIC
  src/ollama_engine_cli_common.cpp
  src/ollama_engine_cli_tests.cpp
  src/ollama_engine_cli_autotune.cpp
  src/ollama_engine_cli_modes.cpp
)
target_include_directories(uam_ollama_engine_cli_common
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(uam_ollama_engine_cli_common PRIVATE uam_ollama_engine)
set_target_properties(uam_ollama_engine_cli_common PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  FOLDER "Subprojects/OllamaEngine"
)

add_executable(uam_ollama_engine_cli
  src/ollama_engine_cli_main.cpp
)
add_executable(uam_ollama_engine_cli_finetune_wizard
  src/ollama_engine_cli_finetune_wizard_main.cpp
)
add_executable(uam_ollama_engine_cli_auto_test
  src/ollama_engine_cli_auto_test_main.cpp
)
add_executable(uam_ollama_engine_cli_custom_tests
  src/ollama_engine_cli_custom_tests_main.cpp
)
add_executable(uam_ollama_engine_cli_eval_suite
  src/ollama_engine_cli_eval_suite_main.cpp
)

set(UAM_OLLAMA_ENGINE_CLI_TARGETS
  uam_ollama_engine_cli
  uam_ollama_engine_cli_finetune_wizard
  uam_ollama_engine_cli_auto_test
  uam_ollama_engine_cli_custom_tests
  uam_ollama_engine_cli_eval_suite
)

foreach(lTargetName IN LISTS UAM_OLLAMA_ENGINE_CLI_TARGETS)
  target_link_libraries(${lTargetName} PRIVATE uam_ollama_engine_cli_common)
  set_target_properties(${lTargetName} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    FOLDER "Subprojects/OllamaEngine"
  )
endforeach()

add_executable(uam_ollama_engine_bridge
  src/ollama_engine_bridge_main.cpp
)
target_link_libraries(uam_ollama_engine_bridge
  PRIVATE
    uam_ollama_engine
    httplib::httplib
    OpenSSL::SSL
    OpenSSL::Crypto
)
target_compile_definitions(uam_ollama_engine_bridge PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
set_target_properties(uam_ollama_engine_bridge PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  FOLDER "Subprojects/OllamaEngine"
)
if(APPLE)
  # Keep the app-bundled bridge binary in sync even when only the bridge target rebuilds.
  add_custom_command(TARGET uam_ollama_engine_bridge POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
      "${CMAKE_BINARY_DIR}/universal_agent_manager.app/Contents/MacOS"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:uam_ollama_engine_bridge>"
      "${CMAKE_BINARY_DIR}/universal_agent_manager.app/Contents/MacOS/uam_ollama_engine_bridge"
    COMMENT "Syncing OpenCode bridge into app bundle"
  )
endif()

set(UAM_OLLAMA_ENGINE_RUNTIME_TARGETS
  ${UAM_OLLAMA_ENGINE_CLI_TARGETS}
  uam_ollama_engine_bridge
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/models")
  add_custom_target(uam_ollama_engine_copy_models ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/models"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/models"
      "${CMAKE_BINARY_DIR}/models"
    COMMENT "Copying ollama_engine models to ${CMAKE_BINARY_DIR}/models"
  )
  foreach(lTargetName IN LISTS UAM_OLLAMA_ENGINE_RUNTIME_TARGETS)
    add_dependencies(${lTargetName} uam_ollama_engine_copy_models)
    add_custom_command(TARGET ${lTargetName} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${lTargetName}>/models"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/models"
        "$<TARGET_FILE_DIR:${lTargetName}>/models"
      COMMENT "Copying ollama_engine models next to ${lTargetName} binary"
    )
  endforeach()
else()
  add_custom_target(uam_ollama_engine_copy_models ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/models"
    COMMENT "Source models folder is missing; creating empty ${CMAKE_BINARY_DIR}/models directory"
  )
  foreach(lTargetName IN LISTS UAM_OLLAMA_ENGINE_RUNTIME_TARGETS)
    add_dependencies(${lTargetName} uam_ollama_engine_copy_models)
  endforeach()
endif()

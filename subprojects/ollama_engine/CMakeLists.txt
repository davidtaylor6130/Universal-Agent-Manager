cmake_minimum_required(VERSION 3.20)

include(FetchContent)

FetchContent_Declare(
  tree_sitter
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
  GIT_TAG v0.22.6
  GIT_SHALLOW TRUE
)
FetchContent_Declare(
  tree_sitter_cpp
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-cpp.git
  GIT_TAG v0.22.3
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(tree_sitter tree_sitter_cpp)

set(UAM_TREE_SITTER_CPP_SOURCES
  ${tree_sitter_cpp_SOURCE_DIR}/src/parser.c
)
if(EXISTS "${tree_sitter_cpp_SOURCE_DIR}/src/scanner.c")
  list(APPEND UAM_TREE_SITTER_CPP_SOURCES ${tree_sitter_cpp_SOURCE_DIR}/src/scanner.c)
elseif(EXISTS "${tree_sitter_cpp_SOURCE_DIR}/src/scanner.cc")
  list(APPEND UAM_TREE_SITTER_CPP_SOURCES ${tree_sitter_cpp_SOURCE_DIR}/src/scanner.cc)
endif()

add_library(uam_tree_sitter_runtime STATIC
  ${tree_sitter_SOURCE_DIR}/lib/src/lib.c
)
target_include_directories(uam_tree_sitter_runtime
  PUBLIC
    ${tree_sitter_SOURCE_DIR}/lib/include
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
    ${tree_sitter_SOURCE_DIR}/lib/include
    ${tree_sitter_cpp_SOURCE_DIR}/src
)
target_link_libraries(uam_tree_sitter_cpp_grammar PUBLIC uam_tree_sitter_runtime)
set_target_properties(uam_tree_sitter_cpp_grammar PROPERTIES
  FOLDER "Subprojects/OllamaEngine/ThirdParty"
)

find_package(SQLite3 REQUIRED)

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
    ${tree_sitter_SOURCE_DIR}/lib/include
    ${tree_sitter_cpp_SOURCE_DIR}/src
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

add_executable(uam_ollama_engine_cli
  src/ollama_engine_cli_main.cpp
)
target_link_libraries(uam_ollama_engine_cli PRIVATE uam_ollama_engine)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/models")
  add_custom_target(uam_ollama_engine_copy_models ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/models"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/models"
      "${CMAKE_BINARY_DIR}/models"
    COMMENT "Copying ollama_engine models to build/models"
  )
  add_dependencies(uam_ollama_engine_cli uam_ollama_engine_copy_models)

  add_custom_command(TARGET uam_ollama_engine_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:uam_ollama_engine_cli>/models"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/models"
      "$<TARGET_FILE_DIR:uam_ollama_engine_cli>/models"
    COMMENT "Copying ollama_engine models next to uam_ollama_engine_cli binary"
  )
else()
  add_custom_target(uam_ollama_engine_copy_models ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/models"
    COMMENT "Source models folder is missing; creating empty build/models directory"
  )
  add_dependencies(uam_ollama_engine_cli uam_ollama_engine_copy_models)
endif()

set_target_properties(uam_ollama_engine_cli PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  FOLDER "Subprojects/OllamaEngine"
)

cmake_minimum_required(VERSION 3.20)

add_library(uam_ollama_engine STATIC
  src/engine_factory.cpp
  src/internal/embedding_utils.cpp
  src/internal/local_ollama_engine.cpp
)

target_include_directories(uam_ollama_engine
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
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

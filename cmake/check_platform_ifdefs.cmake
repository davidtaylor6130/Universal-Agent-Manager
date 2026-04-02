if(NOT DEFINED SOURCE_ROOT)
  message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

file(GLOB_RECURSE UAM_SOURCE_FILES
  "${SOURCE_ROOT}/src/*.h"
  "${SOURCE_ROOT}/src/*.cpp"
)

set(UAM_ALLOWED_PREFIXES
  "src/common/platform/"
  "src/common/paths/"
)

set(UAM_ALLOWED_FILES
  "src/common/chat/chat_repository.cpp"
  "src/common/chat/gemini_json_history_store.cpp"
  "src/common/provider/gemini_command_builder.cpp"
  "src/common/provider/markdown_template_catalog.cpp"
  "src/common/utils/command_line_words.cpp"
)

set(UAM_VIOLATIONS "")

foreach(UAM_FILE IN LISTS UAM_SOURCE_FILES)
  file(RELATIVE_PATH UAM_REL_PATH "${SOURCE_ROOT}" "${UAM_FILE}")

  set(UAM_ALLOWED FALSE)
  foreach(UAM_PREFIX IN LISTS UAM_ALLOWED_PREFIXES)
    if(UAM_REL_PATH MATCHES "^${UAM_PREFIX}")
      set(UAM_ALLOWED TRUE)
      break()
    endif()
  endforeach()

  if(NOT UAM_ALLOWED)
    foreach(UAM_ALLOWED_FILE IN LISTS UAM_ALLOWED_FILES)
      if(UAM_REL_PATH STREQUAL UAM_ALLOWED_FILE)
        set(UAM_ALLOWED TRUE)
        break()
      endif()
    endforeach()
  endif()

  if(UAM_ALLOWED)
    continue()
  endif()

  file(READ "${UAM_FILE}" UAM_CONTENTS)

  string(REGEX MATCH "(#[ \t]*if[^\\n]*(_WIN32|__APPLE__)|\\b(_WIN32|__APPLE__)\\b)" UAM_HAS_PLATFORM_MACRO "${UAM_CONTENTS}")

  if(UAM_HAS_PLATFORM_MACRO)
    list(APPEND UAM_VIOLATIONS "${UAM_REL_PATH}")
  endif()
endforeach()

if(UAM_VIOLATIONS)
  list(REMOVE_DUPLICATES UAM_VIOLATIONS)
  string(REPLACE ";" "\n  - " UAM_VIOLATION_TEXT "${UAM_VIOLATIONS}")
  message(FATAL_ERROR
    "Platform preprocessor guard failed. Move OS conditionals into approved adapter paths.\n"
    "Violations:\n  - ${UAM_VIOLATION_TEXT}\n"
  )
endif()

message(STATUS "Platform preprocessor guard passed.")

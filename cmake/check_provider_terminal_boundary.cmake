if(NOT DEFINED SOURCE_ROOT)
  message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

# Migrated shared consumers and tests must use the public provider interface.
# Other shared subsystems are migrated separately before widening this guard.
file(GLOB_RECURSE UAM_CONSUMER_FILES
  "${SOURCE_ROOT}/tests/*.h"
  "${SOURCE_ROOT}/tests/*.cpp"
  "${SOURCE_ROOT}/src/common/runtime/terminal/*.h"
  "${SOURCE_ROOT}/src/common/runtime/terminal/*.cpp"
)
file(GLOB UAM_TERMINAL_ENTRY_FILES
  "${SOURCE_ROOT}/src/cef/terminal_handlers.cpp"
  "${SOURCE_ROOT}/src/common/runtime/terminal_polling.*"
)
list(APPEND UAM_CONSUMER_FILES ${UAM_TERMINAL_ENTRY_FILES}
  "${SOURCE_ROOT}/src/app/provider_model_catalog_service.cpp"
  "${SOURCE_ROOT}/src/app/agent_run_ledger.cpp"
  "${SOURCE_ROOT}/src/app/native_session_link_service.cpp"
  "${SOURCE_ROOT}/src/common/chat/chat_repository.cpp"
  "${SOURCE_ROOT}/src/common/runtime/acp/acp_resume_rules.cpp"
  "${SOURCE_ROOT}/src/common/runtime/acp/acp_request_builders.cpp"
  "${SOURCE_ROOT}/src/common/runtime/acp/acp_session_runtime.cpp"
  "${SOURCE_ROOT}/src/common/runtime/acp/acp_polling.cpp"
  "${SOURCE_ROOT}/src/common/runtime/acp/acp_response_handlers.cpp"
)

set(UAM_VIOLATIONS "")
foreach(UAM_FILE IN LISTS UAM_CONSUMER_FILES)
  file(READ "${UAM_FILE}" UAM_CONTENTS)
  if(UAM_CONTENTS MATCHES "#[ \t]*include[ \t]*[\"<]common/provider/[^\"<>\r\n]*/")
    file(RELATIVE_PATH UAM_RELATIVE_PATH "${SOURCE_ROOT}" "${UAM_FILE}")
    list(APPEND UAM_VIOLATIONS "${UAM_RELATIVE_PATH}")
  endif()
endforeach()

if(UAM_VIOLATIONS)
  list(JOIN UAM_VIOLATIONS "\n  " UAM_DETAILS)
  message(FATAL_ERROR "Use the public provider runtime interface; implementation imports found:\n  ${UAM_DETAILS}")
endif()
message(STATUS "Provider implementation import boundary passed.")

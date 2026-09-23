file(READ "${SOURCE_ROOT}/src/computer_use/computer_use_mcp_server.cpp" mcp_server)
file(READ "${SOURCE_ROOT}/src/computer_use/computer_use_platform_macos.mm" macos)
file(READ "${SOURCE_ROOT}/src/cef/chat_config_handlers.cpp" settings_handler)

# Helper diagnostics must not corrupt the JSON read by the Computer Use settings UI.
string(FIND "${settings_handler}" "StartStdioProcess(process, {}, argv, &error)" separate_output)
string(FIND "${settings_handler}" "CloseStdioProcessInput(process)" closed_input)
string(FIND "${settings_handler}" "StartStdioProcessWithInput" merged_output)
if(separate_output EQUAL -1 OR closed_input EQUAL -1 OR NOT merged_output EQUAL -1)
  message(FATAL_ERROR "Computer Use settings must isolate helper stdout and close its input.")
endif()

foreach(token IN ITEMS "CGRequestScreenCaptureAccess(" "AXIsProcessTrustedWithOptions(")
  string(FIND "${mcp_server}" "${token}" interactive_request)
  if(NOT interactive_request EQUAL -1)
    message(FATAL_ERROR "Computer-use model tool calls must not request macOS permissions: ${token}")
  endif()
endforeach()

foreach(token IN ITEMS "CGPreflightScreenCaptureAccess()" "AXIsProcessTrusted()")
  string(FIND "${macos}" "${token}" preflight)
  if(preflight EQUAL -1)
    message(FATAL_ERROR "Computer-use macOS permission preflight lost: ${token}")
  endif()
endforeach()

string(FIND "${mcp_server}" "EnsureCapturePermission(&permission_error)" capture_preflight)
string(FIND "${mcp_server}" "EnsureActionPermission(&permission_error)" action_preflight)
string(FIND "${mcp_server}" "{\"state\", \"running\"}" running_state)
if(capture_preflight EQUAL -1 OR action_preflight EQUAL -1 OR running_state EQUAL -1 OR
   capture_preflight GREATER running_state OR action_preflight GREATER running_state)
  message(FATAL_ERROR "Computer-use target approval must preflight capture and input permissions before running state.")
endif()


file(READ "${SOURCE_ROOT}/assets/computer-use/gemini-policy.toml" gemini_policy)
foreach(token IN ITEMS
  "mcpName = \"uam-computer\""
  "toolName = \"computer_observe\""
  "toolName = \"computer_action\""
  "decision = \"allow\""
)
  string(FIND "${gemini_policy}" "${token}" exact_rule)
  if(exact_rule EQUAL -1)
    message(FATAL_ERROR "Gemini's exact UAM computer-use policy lost: ${token}")
  endif()
endforeach()

foreach(token IN ITEMS "mcpName = \"*\"" "toolName = \"*\"")
  string(FIND "${gemini_policy}" "${token}" wildcard_rule)
  if(NOT wildcard_rule EQUAL -1)
    message(FATAL_ERROR "Gemini's UAM computer-use policy must not allow wildcard tools: ${token}")
  endif()
endforeach()

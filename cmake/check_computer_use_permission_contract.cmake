file(READ "${SOURCE_ROOT}/src/computer_use/computer_use_platform_macos.mm" macos)

foreach(token IN ITEMS "CGRequestScreenCaptureAccess(" "AXIsProcessTrustedWithOptions(")
  string(FIND "${macos}" "${token}" interactive_request)
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

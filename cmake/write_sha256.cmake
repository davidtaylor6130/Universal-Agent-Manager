if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED VERSION OR
   NOT DEFINED VERSION_OUTPUT)
  message(FATAL_ERROR "INPUT, OUTPUT, VERSION, and VERSION_OUTPUT are required")
endif()
file(SHA256 "${INPUT}" hash)
file(WRITE "${OUTPUT}" "${hash}\n")
file(WRITE "${VERSION_OUTPUT}" "${VERSION}\n")

if(DEFINED MANIFEST_OUTPUT)
  if(NOT DEFINED SOURCE_FINGERPRINT OR NOT DEFINED PROTOCOL_VERSION)
    message(FATAL_ERROR "The runner source fingerprint and protocol version are required.")
  endif()
  file(WRITE "${MANIFEST_OUTPUT}" "{\"sourceFingerprint\":\"${SOURCE_FINGERPRINT}\",\"protocolVersion\":${PROTOCOL_VERSION}}\n")
endif()

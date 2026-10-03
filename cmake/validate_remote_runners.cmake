if(NOT DEFINED UAM_RUNNER_SOURCE_FINGERPRINT OR NOT DEFINED UAM_RUNNER_PROTOCOL_VERSION)
  include("${CMAKE_CURRENT_LIST_DIR}/runner_source_contract.cmake")
endif()
set(UAM_REQUIRED_REMOTE_RUNNERS
  "linux-arm64/uam-runner"
  "linux-x86_64/uam-runner"
  "windows-x86_64/uam-runner.exe"
)
foreach(UAM_REMOTE_RUNNER IN LISTS UAM_REQUIRED_REMOTE_RUNNERS)
  cmake_path(GET UAM_REMOTE_RUNNER PARENT_PATH UAM_REMOTE_RUNNER_DIR)
  set(UAM_REMOTE_RUNNER_PATH
    "${UAM_REMOTE_RUNNER_ARTIFACT_DIR}/${UAM_REMOTE_RUNNER}")
  set(UAM_REMOTE_RUNNER_SHA256_PATH
    "${UAM_REMOTE_RUNNER_ARTIFACT_DIR}/${UAM_REMOTE_RUNNER_DIR}/uam-runner.sha256")
  set(UAM_REMOTE_RUNNER_VERSION_PATH
    "${UAM_REMOTE_RUNNER_ARTIFACT_DIR}/${UAM_REMOTE_RUNNER_DIR}/uam-runner.version")
  foreach(UAM_REMOTE_RUNNER_FILE
      "${UAM_REMOTE_RUNNER_PATH}"
      "${UAM_REMOTE_RUNNER_SHA256_PATH}"
      "${UAM_REMOTE_RUNNER_VERSION_PATH}"
      "${UAM_REMOTE_RUNNER_ARTIFACT_DIR}/${UAM_REMOTE_RUNNER_DIR}/uam-runner.manifest.json")
    if(NOT EXISTS "${UAM_REMOTE_RUNNER_FILE}")
      message(FATAL_ERROR
        "Full app builds require every remote helper. Missing: ${UAM_REMOTE_RUNNER_FILE}")
    endif()
  endforeach()

  file(READ "${UAM_REMOTE_RUNNER_ARTIFACT_DIR}/${UAM_REMOTE_RUNNER_DIR}/uam-runner.manifest.json" manifest)
  string(JSON fingerprint ERROR_VARIABLE manifest_error GET "${manifest}" sourceFingerprint)
  if(manifest_error OR NOT fingerprint STREQUAL UAM_RUNNER_SOURCE_FINGERPRINT)
    message(FATAL_ERROR "Remote helper source mismatch for ${UAM_REMOTE_RUNNER}. Build or acquire helpers from this source revision's CI artifacts.")
  endif()
  string(JSON protocol ERROR_VARIABLE manifest_error GET "${manifest}" protocolVersion)
  if(manifest_error OR NOT protocol STREQUAL UAM_RUNNER_PROTOCOL_VERSION)
    message(FATAL_ERROR "Remote helper protocol mismatch for ${UAM_REMOTE_RUNNER}.")
  endif()
  file(READ "${UAM_REMOTE_RUNNER_VERSION_PATH}" UAM_REMOTE_RUNNER_VERSION)
  string(STRIP "${UAM_REMOTE_RUNNER_VERSION}" UAM_REMOTE_RUNNER_VERSION)
  if(NOT UAM_REMOTE_RUNNER_VERSION STREQUAL UAM_PRODUCT_VERSION)
    message(FATAL_ERROR
      "Remote helper version mismatch for ${UAM_REMOTE_RUNNER}: "
      "expected ${UAM_PRODUCT_VERSION}, got ${UAM_REMOTE_RUNNER_VERSION}")
  endif()
  file(READ "${UAM_REMOTE_RUNNER_SHA256_PATH}" UAM_REMOTE_RUNNER_EXPECTED_SHA256)
  string(STRIP "${UAM_REMOTE_RUNNER_EXPECTED_SHA256}" UAM_REMOTE_RUNNER_EXPECTED_SHA256)
  file(SHA256 "${UAM_REMOTE_RUNNER_PATH}" UAM_REMOTE_RUNNER_ACTUAL_SHA256)
  if(NOT UAM_REMOTE_RUNNER_ACTUAL_SHA256 STREQUAL UAM_REMOTE_RUNNER_EXPECTED_SHA256)
    message(FATAL_ERROR "Remote helper checksum mismatch: ${UAM_REMOTE_RUNNER}")
  endif()
endforeach()

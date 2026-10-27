include(FetchContent)
# Authentication for the documented native legacy file envelope only. Build the
# six required crypto modules, with no TLS, networking, programs or test suite.
FetchContent_Declare(uam_antigravity_mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
  URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
  SOURCE_SUBDIR uam_native_history_crypto_only
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(uam_antigravity_mbedtls)
get_filename_component(UAM_HISTORY_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
add_library(uam_antigravity_history_crypto STATIC EXCLUDE_FROM_ALL
  "${UAM_HISTORY_SOURCE_ROOT}/src/common/provider/antigravity/cli/antigravity_legacy_crypto.cpp"
  "${uam_antigravity_mbedtls_SOURCE_DIR}/library/aes.c"
  "${uam_antigravity_mbedtls_SOURCE_DIR}/library/cipher.c"
  "${uam_antigravity_mbedtls_SOURCE_DIR}/library/cipher_wrap.c"
  "${uam_antigravity_mbedtls_SOURCE_DIR}/library/gcm.c"
  "${uam_antigravity_mbedtls_SOURCE_DIR}/library/constant_time.c"
  "${uam_antigravity_mbedtls_SOURCE_DIR}/library/platform_util.c")
target_include_directories(uam_antigravity_history_crypto PRIVATE
  "${UAM_HISTORY_SOURCE_ROOT}/src/common/provider/antigravity/cli"
  "${uam_antigravity_mbedtls_SOURCE_DIR}/include"
  "${uam_antigravity_mbedtls_SOURCE_DIR}/library")
target_compile_definitions(uam_antigravity_history_crypto PRIVATE
  MBEDTLS_CONFIG_FILE="uam_antigravity_crypto_config.h")
set_target_properties(uam_antigravity_history_crypto PROPERTIES
  CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF FOLDER "Providers")
if(MSVC)
  set_property(TARGET uam_antigravity_history_crypto PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

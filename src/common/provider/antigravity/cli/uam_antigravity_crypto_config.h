#pragma once
// Minimal Mbed TLS configuration for authenticated legacy transcript decoding.
// No account-key discovery, TLS, sockets, random generation or custom crypto.
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_GCM_C

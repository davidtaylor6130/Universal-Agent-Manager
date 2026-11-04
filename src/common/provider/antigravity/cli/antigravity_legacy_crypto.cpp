#include "antigravity_legacy_crypto.h"
#include "antigravity_legacy_format_key.h"
#include <mbedtls/gcm.h>
#include <mbedtls/platform_util.h>

namespace uam::antigravity::history::detail
{
	LegacyPlaintext::~LegacyPlaintext()
	{
		mbedtls_platform_zeroize(bytes.data(), bytes.size());
	}
	std::unique_ptr<LegacyPlaintext> DecryptLegacy(std::string_view encrypted)
	{
		constexpr std::size_t nonce = 12, tag = 16, limit = 64U * 1024U * 1024U;
		if (encrypted.size() < nonce + tag || encrypted.size() - nonce - tag > limit)
			return {};
		mbedtls_gcm_context context;
		mbedtls_gcm_init(&context);
		struct Cleanup
		{
			mbedtls_gcm_context* context;
			~Cleanup()
			{
				mbedtls_gcm_free(context);
			}
		} cleanup{&context};
		if (mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, kLegacyFormatKey.data(), 256) != 0)
			return {};
		auto plain = std::make_unique<LegacyPlaintext>();
		plain->bytes.resize(encrypted.size() - nonce - tag);
		const auto* input = reinterpret_cast<const unsigned char*>(encrypted.data());
		if (mbedtls_gcm_auth_decrypt(&context, plain->bytes.size(), input, nonce, nullptr, 0,
		        input + encrypted.size() - tag, tag, input + nonce,
		        reinterpret_cast<unsigned char*>(plain->bytes.data())) != 0)
			return {};
		return plain;
	}
}

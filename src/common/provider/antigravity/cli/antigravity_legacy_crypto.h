#pragma once
#include <memory>
#include <string>
#include <string_view>

namespace uam::antigravity::history::detail
{
	struct LegacyPlaintext
	{
		std::string bytes;
		~LegacyPlaintext();
	};
	// Authenticates the entire nonce/ciphertext/tag before exposing any plaintext.
	// The fixed public format key is not a user/account credential. No file IO.
	std::unique_ptr<LegacyPlaintext> DecryptLegacy(std::string_view encrypted);
}

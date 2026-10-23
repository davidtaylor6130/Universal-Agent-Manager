#include "common/platform/platform_incremental_sha256.h"

#include <algorithm>
#include <array>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#elif defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#endif

namespace uam::platform
{
	struct IncrementalSha256::Impl
	{
		bool ok = false;
#if defined(__APPLE__)
		CC_SHA256_CTX context{};
#elif defined(_WIN32)
		HCRYPTPROV provider = 0;
		HCRYPTHASH hash = 0;
#endif
		Impl()
		{
#if defined(__APPLE__)
			ok = CC_SHA256_Init(&context) == 1;
#elif defined(_WIN32)
			ok = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
			    CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash);
#endif
		}
		~Impl()
		{
#if defined(_WIN32)
			if (hash) CryptDestroyHash(hash);
			if (provider) CryptReleaseContext(provider, 0);
#endif
		}
	};

	IncrementalSha256::IncrementalSha256() : m_impl(std::make_unique<Impl>()) {}
	IncrementalSha256::~IncrementalSha256() = default;

	bool IncrementalSha256::Add(std::string_view bytes)
	{
		if (!m_impl->ok) return false;
		for (std::size_t offset = 0; offset < bytes.size();)
		{
			const std::size_t count = std::min<std::size_t>(bytes.size() - offset, 64U * 1024U);
#if defined(__APPLE__)
			m_impl->ok = CC_SHA256_Update(&m_impl->context, bytes.data() + offset, static_cast<CC_LONG>(count)) == 1;
#elif defined(_WIN32)
			m_impl->ok = CryptHashData(m_impl->hash, reinterpret_cast<const BYTE*>(bytes.data() + offset), static_cast<DWORD>(count), 0) != 0;
#endif
			if (!m_impl->ok) return false;
			offset += count;
		}
		return true;
	}

	std::string IncrementalSha256::Finish()
	{
		if (!m_impl->ok) return {};
		std::array<unsigned char, 32> bytes{};
#if defined(__APPLE__)
		m_impl->ok = CC_SHA256_Final(bytes.data(), &m_impl->context) == 1;
#elif defined(_WIN32)
		DWORD size = static_cast<DWORD>(bytes.size());
		m_impl->ok = CryptGetHashParam(m_impl->hash, HP_HASHVAL, bytes.data(), &size, 0) && size == bytes.size();
#endif
		if (!m_impl->ok) return {};
		constexpr char hex[] = "0123456789abcdef";
		std::string result;
		result.reserve(64);
		for (unsigned char byte : bytes)
		{
			result += hex[byte >> 4];
			result += hex[byte & 15];
		}
		m_impl->ok = false;
		return result;
	}
}

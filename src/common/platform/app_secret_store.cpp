#include "common/platform/credential_store.h"

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#include <vector>
#endif

namespace uam::platform
{
	namespace
	{
		constexpr std::size_t kMaxAppSecretBytes = 4096;
#if defined(__APPLE__)
		struct CfRelease
		{
			CFTypeRef value;
			~CfRelease() { if (value) CFRelease(value); }
		};

		CFStringRef CfString(std::string_view text)
		{
			return CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(text.data()), static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false);
		}

		CFMutableDictionaryRef ItemQuery(CFStringRef service, CFStringRef account)
		{
			CFMutableDictionaryRef query = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
			CFDictionarySetValue(query, kSecAttrService, service);
			CFDictionarySetValue(query, kSecAttrAccount, account);
			return query;
		}
#elif defined(_WIN32)
		std::string WindowsTarget(std::string_view service, std::string_view account)
		{
			return std::string(service) + "/" + std::string(account);
		}
#endif
	}

	std::optional<std::string> ReadAppSecret(std::string_view service, std::string_view account)
	{
#if defined(__APPLE__)
		const CFStringRef service_name = CfString(service), account_name = CfString(account);
		const CfRelease service_guard{service_name}, account_guard{account_name};
		if (!service_name || !account_name) return std::nullopt;
		const CFMutableDictionaryRef query = ItemQuery(service_name, account_name);
		const CfRelease query_guard{query};
		CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
		CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
		CFTypeRef result = nullptr;
		// A changed (re-signed) UAM binary may show the Keychain access dialog once; that dialog is the protection.
		if (SecItemCopyMatching(query, &result) != errSecSuccess || !result) return std::nullopt;
		const CfRelease result_guard{result};
		if (CFGetTypeID(result) != CFDataGetTypeID()) return std::nullopt;
		const CFDataRef data = static_cast<CFDataRef>(result);
		const CFIndex length = CFDataGetLength(data);
		if (length <= 0 || static_cast<std::size_t>(length) > kMaxAppSecretBytes) return std::nullopt;
		return std::string(reinterpret_cast<const char*>(CFDataGetBytePtr(data)), static_cast<std::size_t>(length));
#elif defined(_WIN32)
		StoredCredential stored = ReadWindowsGenericCredential(WindowsTarget(service, account), kMaxAppSecretBytes);
		if (stored.status != StoredCredentialStatus::Found) return std::nullopt;
		return std::move(stored.secret);
#else
		(void)service;
		(void)account;
		return std::nullopt;
#endif
	}

	bool WriteAppSecret(std::string_view service, std::string_view account, std::string_view secret)
	{
		if (secret.empty() || secret.size() > kMaxAppSecretBytes) return false;
#if defined(__APPLE__)
		const CFStringRef service_name = CfString(service), account_name = CfString(account);
		const CfRelease service_guard{service_name}, account_guard{account_name};
		const CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(secret.data()), static_cast<CFIndex>(secret.size()));
		const CfRelease data_guard{data};
		if (!service_name || !account_name || !data) return false;
		const CFMutableDictionaryRef query = ItemQuery(service_name, account_name);
		const CfRelease query_guard{query};
		CFMutableDictionaryRef update = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		const CfRelease update_guard{update};
		CFDictionarySetValue(update, kSecValueData, data);
		const OSStatus updated = SecItemUpdate(query, update);
		if (updated == errSecSuccess) return true;
		if (updated != errSecItemNotFound) return false;
		CFDictionarySetValue(query, kSecValueData, data);
		CFDictionarySetValue(query, kSecAttrLabel, service_name);
		return SecItemAdd(query, nullptr) == errSecSuccess;
#elif defined(_WIN32)
		// ponytail: ANSI target suits the ASCII service/account names callers pass; use CredWriteW for non-ASCII.
		std::string target = WindowsTarget(service, account);
		std::vector<char> blob(secret.begin(), secret.end());
		CREDENTIALA credential{};
		credential.Type = CRED_TYPE_GENERIC;
		credential.TargetName = target.data();
		credential.CredentialBlobSize = static_cast<DWORD>(blob.size());
		credential.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
		credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
		const bool written = CredWriteA(&credential, 0) != FALSE;
		SecureZeroMemory(blob.data(), blob.size());
		return written;
#else
		(void)service;
		(void)account;
		return false;
#endif
	}
}

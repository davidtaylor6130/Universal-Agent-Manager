#include "common/secrets/secret_keychain.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <string>

// macOS: Security.framework inside UAM itself, so the item's access list trusts only this app. An agent's own
// osascript or security process asking for the item gets the system prompt instead of the value.
namespace uam::secrets::keychain
{
	namespace
	{
		template <typename T>
		struct Owned
		{
			T ref = nullptr;
			~Owned() { if (ref != nullptr) CFRelease(ref); }
		};

		CFStringRef String(std::string_view value)
		{
			return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(value.data()), static_cast<CFIndex>(value.size()), kCFStringEncodingUTF8, false);
		}

		CFMutableDictionaryRef Query(CFStringRef service, CFStringRef account)
		{
			CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
			CFDictionarySetValue(query, kSecAttrService, service);
			CFDictionarySetValue(query, kSecAttrAccount, account);
			return query;
		}

		bool Fail(std::string* error, OSStatus status)
		{
			if (error != nullptr) *error = "Keychain error " + std::to_string(static_cast<int>(status)) + ".";
			return false;
		}
	}

	bool Available()
	{
		return true;
	}

	bool Save(std::string_view account, std::string_view value, std::string* error)
	{
		Owned<CFStringRef> service{String(kService)};
		Owned<CFStringRef> name{String(account)};
		Owned<CFDataRef> data{CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(value.data()), static_cast<CFIndex>(value.size()))};
		if (service.ref == nullptr || name.ref == nullptr || data.ref == nullptr) return Fail(error, errSecAllocate);
		Owned<CFMutableDictionaryRef> query{Query(service.ref, name.ref)};
		Owned<CFMutableDictionaryRef> update{CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)};
		CFDictionarySetValue(update.ref, kSecValueData, data.ref);
		OSStatus status = SecItemUpdate(query.ref, update.ref);
		if (status == errSecItemNotFound)
		{
			CFDictionarySetValue(query.ref, kSecValueData, data.ref);
			status = SecItemAdd(query.ref, nullptr);
		}
		return status == errSecSuccess || Fail(error, status);
	}

	std::optional<std::string> Load(std::string_view account, std::string* error)
	{
		if (error != nullptr) error->clear();
		Owned<CFStringRef> service{String(kService)};
		Owned<CFStringRef> name{String(account)};
		if (service.ref == nullptr || name.ref == nullptr)
		{
			(void)Fail(error, errSecAllocate);
			return std::nullopt;
		}
		Owned<CFMutableDictionaryRef> query{Query(service.ref, name.ref)};
		CFDictionarySetValue(query.ref, kSecReturnData, kCFBooleanTrue);
		CFDictionarySetValue(query.ref, kSecMatchLimit, kSecMatchLimitOne);
		Owned<CFTypeRef> result;
		const OSStatus status = SecItemCopyMatching(query.ref, &result.ref);
		if (status == errSecItemNotFound) return std::nullopt;
		if (status != errSecSuccess || result.ref == nullptr || CFGetTypeID(result.ref) != CFDataGetTypeID())
		{
			(void)Fail(error, status == errSecSuccess ? errSecDecode : status);
			return std::nullopt;
		}
		const CFDataRef bytes = static_cast<CFDataRef>(result.ref);
		return std::string(reinterpret_cast<const char*>(CFDataGetBytePtr(bytes)), static_cast<std::size_t>(CFDataGetLength(bytes)));
	}

	bool Forget(std::string_view account, std::string* error)
	{
		Owned<CFStringRef> service{String(kService)};
		Owned<CFStringRef> name{String(account)};
		if (service.ref == nullptr || name.ref == nullptr) return Fail(error, errSecAllocate);
		Owned<CFMutableDictionaryRef> query{Query(service.ref, name.ref)};
		const OSStatus status = SecItemDelete(query.ref);
		return status == errSecSuccess || status == errSecItemNotFound || Fail(error, status);
	}
}

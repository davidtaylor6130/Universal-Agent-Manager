#include "common/platform/credential_store.h"

#include <filesystem>

namespace uam::platform
{
	namespace
	{
		// No Apple events or UI automation. kSecUseAuthenticationUIFail does not stop login-keychain ACL dialogs,
		// so automatic reads also disable Keychain interaction for this helper process.
		// The fixed system host lets cancellation reap a slow Keychain request without
		// leaving an uninterruptible Security.framework call in the desktop process.
		constexpr std::string_view kKeychainReadScript = R"JXA(ObjC.import('Foundation');
ObjC.import('Security');
ObjC.bindFunction('SecItemCopyMatching', ['int', ['id', 'id*']]);
function run(argv)
{
    function object(value) { return ObjC.castRefToObject(value); }
    const prompt = argv[2] === 'prompt';
    if (!prompt)
    {
        ObjC.bindFunction('SecKeychainSetUserInteractionAllowed', ['int', ['bool']]);
        if ($.SecKeychainSetUserInteractionAllowed(false) !== 0) return JSON.stringify({ status: -25308 });
    }
    const query = $.NSMutableDictionary.alloc.init;
    query.setObjectForKey(object($.kSecClassGenericPassword), object($.kSecClass));
    query.setObjectForKey($(argv[0]), object($.kSecAttrService));
    query.setObjectForKey($(argv[1]), object($.kSecAttrAccount));
    query.setObjectForKey($.NSNumber.numberWithBool(true), object($.kSecReturnData));
    query.setObjectForKey(object($.kSecMatchLimitOne), object($.kSecMatchLimit));
    if (!prompt) query.setObjectForKey(object($.kSecUseAuthenticationUIFail), object($.kSecUseAuthenticationUI));
    const result = Ref();
    const status = $.SecItemCopyMatching(query, result);
    if (status !== 0) return JSON.stringify({ status: status });
    const credential = ObjC.unwrap($.NSString.alloc.initWithDataEncoding(result[0], $.NSUTF8StringEncoding));
    if (typeof credential !== 'string' || credential.length > 65536) return JSON.stringify({ status: -1 });
    return JSON.stringify({ status: 0, credential: credential });
})JXA";
	}

	std::vector<std::string> MacKeychainReadArguments(std::string_view service, std::string_view account, bool allow_prompt)
	{
		return {"/usr/bin/osascript", "-l", "JavaScript", "-e", std::string(kKeychainReadScript), std::string(service), std::string(account), allow_prompt ? "prompt" : "silent"};
	}

	std::vector<std::string> SecretServiceSearchArguments(std::string_view service, std::string_view account, bool allow_prompt)
	{
		// Prefer the system binary over a PATH entry; go-keyring items carry "service" and "username" attributes.
		std::error_code error;
		std::vector<std::string> arguments{std::filesystem::is_regular_file("/usr/bin/secret-tool", error) ? "/usr/bin/secret-tool" : "secret-tool", "search"};
		// Without --unlock, search returns a locked item's details but not its secret and never shows an unlock prompt.
		if (allow_prompt) arguments.push_back("--unlock");
		arguments.insert(arguments.end(), {"service", std::string(service), "username", std::string(account)});
		return arguments;
	}
}

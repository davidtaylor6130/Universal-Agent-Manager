#include "common/secrets/secret_keychain.h"

// Windows Credential Manager lets any program running as the user read a generic credential, an agent's shell
// included, so UAM does not save secrets on Windows (David, 2026-11-12): every Windows secret is one-time.
namespace uam::secrets::keychain
{
	namespace
	{
		bool Unavailable(std::string* error)
		{
			if (error != nullptr) *error = "Saving secrets isn't available on Windows.";
			return false;
		}
	}

	bool Available()
	{
		return false;
	}

	bool Save(std::string_view, std::string_view, std::string* error)
	{
		return Unavailable(error);
	}

	std::optional<std::string> Load(std::string_view, std::string* error)
	{
		(void)Unavailable(error);
		return std::nullopt;
	}

	bool Forget(std::string_view, std::string* error)
	{
		return Unavailable(error);
	}
}

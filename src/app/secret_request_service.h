#pragma once

#include "common/secrets/secret_drop.h"
#include "common/state/app_state.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace uam
{
	/// Where a submitted secret goes: a read-once drop on the agent's own computer.
	struct SecretDeliveryTarget
	{
		std::optional<ExecutionHost> remote_host; // Empty: this computer.
		std::filesystem::path local_directory;
		std::string keychain_account;
	};

	/// The `secret_request` UAM tool: arguments, who may ask, the card the user sees and the read-once delivery.
	/// The value itself never enters chat state, the tool reply or a log; the agent receives only a path.
	class SecretRequestService
	{
	  public:
		static bool ValidArguments(const nlohmann::json& arguments);
		/// Non-empty when this caller may not ask for a secret at all (no card is shown).
		static std::string Refusal(const AppState& app, const UamControlCapability& capability);
		/// The card state kept on the pending approval: the arguments plus whether Plan mode needs a one-time Allow.
		static nlohmann::json PendingMetadata(const AppState& app, const UamControlCapability& capability, const nlohmann::json& arguments);
		/// The pending card as a chat's `pendingUserInput`, with the extra `secretRequest` details.
		static AcpPendingUserInputState Projection(const AppState& app, const UamControlCapability& capability, std::string request_id_json);
		static SecretDeliveryTarget Target(const AppState& app, const UamControlCapability& capability);
		/// Non-empty when Save or Use saved is asked where saving is not offered: SSH hosts and Windows.
		static std::string SaveRefusal(const SecretDeliveryTarget& target, bool save, bool use_saved);
		/// Blocking (SSH): run off the UI thread.
		static secrets::DropResult Deliver(const SecretDeliveryTarget& target, std::string_view value);
		static std::filesystem::path LocalDropDirectory(const std::filesystem::path& data_root);
		/// Saved-secret index: account names only, never values.
		static bool KeychainSaved(const std::filesystem::path& data_root, std::string_view account);
		static bool RecordKeychainAccount(const std::filesystem::path& data_root, std::string_view account, bool saved);
	};
} // namespace uam

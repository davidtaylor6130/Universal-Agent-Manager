#include "app/secret_request_service.h"

#include "app/chat_domain_service.h"
#include "common/config/approval_modes.h"
#include "common/config/execution_host_config.h"
#include "common/platform/platform_services.h"
#include "common/secrets/secret_keychain.h"
#include "common/utils/io_utils.h"
#include "remote/runner_client.h"

#include <algorithm>
#include <mutex>

namespace uam
{
	namespace
	{
		constexpr std::size_t kMaxLabelBytes = 80;
		constexpr std::size_t kMaxReasonBytes = 300;
		constexpr std::size_t kMaxPlaceholderBytes = 40;
		constexpr std::size_t kMaxUrlBytes = 512;

		/// Single-line, NUL-free, bounded display text.
		bool Line(const nlohmann::json& arguments, std::string_view key, std::size_t limit, bool required)
		{
			const auto found = arguments.find(key);
			if (found == arguments.end()) return !required;
			if (!found->is_string()) return false;
			const std::string& value = found->get_ref<const std::string&>();
			if (value.size() > limit || (required && value.find_first_not_of(" \t") == std::string::npos)) return false;
			return std::ranges::none_of(value, [](char ch) { return static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f; });
		}

		const AcpSessionState* CapabilitySession(const AppState& app, const UamControlCapability& capability)
		{
			for (const auto& session : app.acp_sessions)
				if (session != nullptr && session->uam_control_capability_id == capability.id) return session.get();
			return nullptr;
		}

		std::string ProviderName(const AppState& app, std::string_view provider_id)
		{
			for (const ProviderProfile& profile : app.provider_profiles)
				if (profile.id == provider_id && !profile.title.empty()) return profile.title;
			return std::string(provider_id);
		}

		std::filesystem::path IndexPath(const std::filesystem::path& data_root)
		{
			return data_root / "secrets" / "keychain-accounts.json";
		}

		// ponytail: one cached index for the app's data root; it changes only through RecordKeychainAccount.
		std::mutex g_index_mutex;
		std::filesystem::path g_index_root;
		std::vector<std::string> g_index;

		std::vector<std::string> ReadIndexFile(const std::filesystem::path& data_root)
		{
			std::string text;
			if (!io::TryReadTextFile(IndexPath(data_root), text)) return {};
			const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
			std::vector<std::string> accounts;
			if (parsed.is_array())
				for (const nlohmann::json& value : parsed)
					if (value.is_string()) accounts.push_back(value.get<std::string>());
			return accounts;
		}
	} // namespace

	bool SecretRequestService::ValidArguments(const nlohmann::json& arguments)
	{
		if (!arguments.is_object() || arguments.size() > 5) return false;
		for (const auto& [key, value] : arguments.items())
			if (key != "label" && key != "reason" && key != "placeholder" && key != "url" && key != "delivery") return false;
		if (!Line(arguments, "label", kMaxLabelBytes, true) || !Line(arguments, "reason", kMaxReasonBytes, true) ||
		    !Line(arguments, "placeholder", kMaxPlaceholderBytes, false) || !Line(arguments, "url", kMaxUrlBytes, false))
			return false;
		// ponytail: file delivery only; "ref" waits for a consumer that can use it (secret_exec).
		if (arguments.contains("delivery") && arguments["delivery"] != "file") return false;
		const std::string url = arguments.value("url", "");
		return url.empty() || (url.starts_with("https://") && url.size() > 8 && url.find(' ') == std::string::npos);
	}

	std::string SecretRequestService::Refusal(const AppState& app, const UamControlCapability& capability)
	{
		const ChatSession* chat = ChatDomainService().FindChatById(app, capability.session_chat_id);
		const ChatSession* root = ChatDomainService().FindChatById(app, capability.chat_id);
		if (chat == nullptr || root == nullptr) return "The chat that asked for this secret is unavailable.";
		if (const AcpSessionState* session = CapabilitySession(app, capability); session != nullptr && session->goal_review_turn)
			return "Goal review turns can't request secrets.";
		if (!chat->temporary_parent_chat_id.empty() || !root->temporary_parent_chat_id.empty())
			return "Side chats can't request secrets.";
		if (!root->automation_token_id.empty())
			return "Automation chats can't request secrets; no one is there to type it.";
		return {};
	}

	nlohmann::json SecretRequestService::PendingMetadata(const AppState& app, const UamControlCapability& capability, const nlohmann::json& arguments)
	{
		const ChatSession* root = ChatDomainService().FindChatById(app, capability.chat_id);
		const bool plan = root != nullptr && approval_modes::AppApprovalModeOrEmpty(root->approval_mode) == approval_modes::kPlanApprovalMode;
		return {{"label", arguments.at("label")}, {"reason", arguments.at("reason")},
		        {"placeholder", arguments.value("placeholder", "")}, {"url", arguments.value("url", "")},
		        {"planOptIn", plan}, {"delivering", false}};
	}

	AcpPendingUserInputState SecretRequestService::Projection(const AppState& app, const UamControlCapability& capability, std::string request_id_json)
	{
		const nlohmann::json& metadata = capability.pending_approval->secret_request;
		const std::string provider = ProviderName(app, capability.provider_id);
		const std::string label = metadata.value("label", "");
		AcpPendingUserInputState input;
		input.request_id_json = std::move(request_id_json);
		input.status = "pending";
		input.attention_kind = "question";
		AcpUserInputQuestionState question;
		question.id = "secretRequest";
		question.header = "Secret";
		// The phone and any client without the secret card read only this sentence: it cannot answer.
		question.question = provider + " asks for a secret: " + label + ". Answer it on the computer.";
		question.is_other = false;
		input.questions.push_back(std::move(question));
		const SecretDeliveryTarget target = Target(app, capability);
		nlohmann::json host = nullptr;
		if (target.remote_host.has_value())
			host = {{"name", target.remote_host->label.empty() ? target.remote_host->ssh_alias : target.remote_host->label},
			        {"platform", target.remote_host->platform}};
		const bool keychain = secrets::keychain::Available() && !target.remote_host.has_value();
		input.secret_request = {{"label", label}, {"reason", metadata.value("reason", "")}, {"localPlatform", std::string(secrets::LocalDrop().platform)},
		                        {"placeholder", metadata.value("placeholder", "")}, {"url", metadata.value("url", "")},
		                        {"provider", provider}, {"host", std::move(host)},
		                        {"planOptIn", metadata.value("planOptIn", false)},
		                        {"keychain", {{"available", keychain}, {"saved", keychain && KeychainSaved(app.data_root, target.keychain_account)}}}};
		return input;
	}

	SecretDeliveryTarget SecretRequestService::Target(const AppState& app, const UamControlCapability& capability)
	{
		SecretDeliveryTarget target;
		target.local_directory = LocalDropDirectory(app.data_root);
		// The secret goes where the asking agent runs: a subagent's own host, not its parent's.
		const ChatSession* chat = ChatDomainService().FindChatById(app, capability.session_chat_id);
		if (chat != nullptr && chat->execution_host_id != execution_hosts::kLocalHostId)
			if (const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat->execution_host_id))
				target.remote_host = *host;
		const ChatSession* root = ChatDomainService().FindChatById(app, capability.chat_id);
		const std::string workspace = root == nullptr ? std::string{} : root->workspace_directory;
		const std::string label = capability.pending_approval ? capability.pending_approval->secret_request.value("label", "") : std::string{};
		target.keychain_account = workspace + " | " + label;
		return target;
	}

	std::string SecretRequestService::SaveRefusal(const SecretDeliveryTarget& target, bool save, bool use_saved)
	{
		if (!save && !use_saved) return {};
		if (!secrets::keychain::Available()) return "Saving secrets isn't available on this computer; this secret is one-time.";
		if (target.remote_host.has_value()) return "Saved secrets are only for agents on this computer.";
		return {};
	}

	secrets::DropResult SecretRequestService::Deliver(const SecretDeliveryTarget& target, std::string_view value)
	{
		if (!target.remote_host.has_value())
		{
			// Drop creates only its own folder; its parent is made here on first use.
			std::error_code ignored;
			std::filesystem::create_directories(target.local_directory.parent_path(), ignored);
			return secrets::Drop(target.local_directory, value, secrets::LocalDrop().ttl);
		}
		secrets::DropResult result;
		if (!remote::CanAttemptRunnerConnection(*target.remote_host))
		{
			result.error = "The UAM helper on this host is not ready.";
			return result;
		}
		remote::RunnerClient client(PlatformServicesFactory::Instance().process_service, *target.remote_host);
		result.ok = client.DropSecret(value, result.path, &result.error);
		if (!result.ok && result.error.empty()) result.error = "The secret could not be delivered to the host.";
		return result;
	}

	std::filesystem::path SecretRequestService::LocalDropDirectory(const std::filesystem::path& data_root)
	{
		return data_root / "secrets" / "drops";
	}

	bool SecretRequestService::KeychainSaved(const std::filesystem::path& data_root, std::string_view account)
	{
		std::lock_guard lock(g_index_mutex);
		if (g_index_root != data_root)
		{
			g_index = ReadIndexFile(data_root);
			g_index_root = data_root;
		}
		return std::ranges::find(g_index, account) != g_index.end();
	}

	bool SecretRequestService::RecordKeychainAccount(const std::filesystem::path& data_root, std::string_view account, bool saved)
	{
		std::lock_guard lock(g_index_mutex);
		std::vector<std::string> accounts = ReadIndexFile(data_root);
		std::erase(accounts, std::string(account));
		if (saved) accounts.emplace_back(account);
		std::error_code ignored;
		std::filesystem::create_directories(IndexPath(data_root).parent_path(), ignored);
		const bool written = io::WriteTextFile(IndexPath(data_root), nlohmann::json(accounts).dump());
		g_index = std::move(accounts);
		g_index_root = data_root;
		return written;
	}
} // namespace uam

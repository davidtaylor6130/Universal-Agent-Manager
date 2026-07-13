#include "cef/cef_push.h"
#include "cef/state_serializer.h"

#include "app/chat_domain_service.h"

#include "common/config/settings_frontend_json.h"
#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/wrapper/cef_helpers.h"

#include "common/utils/base64.h"
#include "common/utils/nlohmann_json_utils.h"

#include <chrono>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace
{
	constexpr const char* kPushTypeStateUpdate = "stateUpdate";
	constexpr const char* kPushTypeStatePatch = "statePatch";
	constexpr const char* kPushTypeStreamToken = "streamToken";
	constexpr const char* kPushTypeStreamDone = "streamDone";
	constexpr const char* kPushTypeCliOutput = "cliOutput";
	constexpr long long kStatePatchSlowSerializationMs = 8;
	constexpr std::size_t kStatePatchLargeMessageBytes = 64 * 1024;
	// While a turn streams, the selected chat's messagesDigest changes on nearly
	// every 16ms poll tick; re-serializing and re-pushing the full message array
	// at that rate stalls both the serializer and the React message list. Live
	// text still arrives instantly via streamToken pushes, so the heavyweight
	// message-array patch only needs to keep up at a coarse interval.
	constexpr std::chrono::milliseconds kSelectedChatMessagesMinPushInterval{250};
	// The selected chat's summary carries the live turn timeline (turnEvents),
	// which grows on nearly every tick during a turn; 10 updates per second is
	// plenty for streamed text and tool-call progress. Summaries that introduce
	// a pending permission or user-input request bypass the throttle so
	// interaction prompts are never delayed.
	constexpr std::chrono::milliseconds kSelectedChatSummaryMinPushInterval{100};

	std::string g_last_pushed_state_fingerprint;
	std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_last_messages_push_time_by_chat_id;
	std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_last_summary_push_time_by_chat_id;
	bool g_messages_push_deferred = false;
	std::unordered_map<std::string, std::string> g_last_pushed_message_digests_by_chat_id;
	std::unordered_map<std::string, std::string> g_last_pushed_chat_summaries_by_chat_id;
	std::string g_last_pushed_folders_fingerprint;
	std::string g_last_pushed_providers_fingerprint;
	std::string g_last_pushed_settings_fingerprint;
	std::string g_last_pushed_memory_fingerprint;
	std::string g_last_pushed_shell_actions_fingerprint;
	std::string g_last_pushed_shell_action_notification;
	std::string g_last_pushed_selected_chat_id;

	/// Posts a window.uamPush(json) call to the browser's main frame.
	void PostPush(CefRefPtr<CefBrowser> browser, const std::string& json)
	{
		if (!browser)
			return;

		CefRefPtr<CefFrame> frame = browser->GetMainFrame();
		if (!frame)
			return;

		const std::string js = "window.uamPush && window.uamPush(" + json + ");";
		frame->ExecuteJavaScript(js, frame->GetURL(), 0);
	}

	nlohmann::json PushMessage(const char* type)
	{
		nlohmann::json message;
		message["type"] = type;
		return message;
	}

	nlohmann::json SerializeSettingsForPatch(const uam::AppState& app)
	{
		return uam::settings_frontend_json::SerializeLiveSettingsFields(app.settings, app.memory_last_status);
	}

	void ResetPatchBaselines(const uam::AppState& app)
	{
		const nlohmann::json fingerprint_state = uam::StateSerializer::SerializeFingerprint(app);
		g_last_pushed_folders_fingerprint = uam::nlohmann_json::ArrayFieldOrEmpty(fingerprint_state, "folders").dump();
		g_last_pushed_providers_fingerprint = uam::nlohmann_json::ArrayFieldOrEmpty(fingerprint_state, "providers").dump();
		g_last_pushed_settings_fingerprint = SerializeSettingsForPatch(app).dump();
		g_last_pushed_memory_fingerprint = uam::nlohmann_json::ObjectFieldOrEmpty(fingerprint_state, "memoryActivity").dump();
		g_last_pushed_shell_actions_fingerprint = uam::nlohmann_json::ArrayFieldOrEmpty(fingerprint_state, "shellActions").dump();
		g_last_pushed_shell_action_notification = uam::nlohmann_json::TrimmedStringValue(fingerprint_state, {"shellActionNotification"});
		g_last_pushed_selected_chat_id = ChatDomainService().SelectedChatId(app);
		g_last_pushed_message_digests_by_chat_id.clear();
		g_last_pushed_chat_summaries_by_chat_id.clear();
		g_last_messages_push_time_by_chat_id.clear();
		g_last_summary_push_time_by_chat_id.clear();
		g_messages_push_deferred = false;

		const nlohmann::json chats = uam::nlohmann_json::ArrayFieldOrEmpty(fingerprint_state, "chats");

		for (const nlohmann::json& chat : chats)
		{
			if (!chat.is_object())
			{
				continue;
			}

			const std::string chat_id = uam::nlohmann_json::TrimmedStringValue(chat, {"id"});
			if (chat_id.empty())
			{
				continue;
			}

			g_last_pushed_chat_summaries_by_chat_id[chat_id] = chat.dump();
			g_last_pushed_message_digests_by_chat_id[chat_id] = uam::nlohmann_json::TrimmedStringValue(chat, {"messagesDigest"});
		}
	}

	std::string BuildFullStateUpdateMessage(const uam::AppState& app)
	{
		nlohmann::json msg = PushMessage(kPushTypeStateUpdate);
		msg["data"] = uam::StateSerializer::Serialize(app);
		return msg.dump();
	}

	void MaybeLogLargePatch(const std::string& message, int changed_chat_count, std::chrono::steady_clock::duration elapsed)
	{
#ifndef NDEBUG
		const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
		if (elapsed_ms > kStatePatchSlowSerializationMs || message.size() > kStatePatchLargeMessageBytes)
		{
			std::cerr << "[UAM] statePatch serialization took " << elapsed_ms << "ms, bytes=" << message.size() << ", changedChats=" << changed_chat_count << "\n";
		}
#else
		(void)message;
		(void)changed_chat_count;
		(void)elapsed;
#endif
	}

	void AddChangedJsonField(nlohmann::json& patch_data, const char* field_name, const nlohmann::json& value, std::string& last_fingerprint)
	{
		const std::string fingerprint = value.dump();
		if (fingerprint == last_fingerprint)
		{
			return;
		}

		patch_data[field_name] = value;
		last_fingerprint = fingerprint;
	}

	struct ChatPatchDiff
	{
		nlohmann::json changed_chats = nlohmann::json::array();
		nlohmann::json messages_by_chat_id = nlohmann::json::object();
		nlohmann::json removed_chat_ids = nlohmann::json::array();
		std::unordered_map<std::string, std::string> next_chat_summaries;
		std::unordered_map<std::string, std::string> next_message_digests;

		bool HasChatListChange() const
		{
			return !changed_chats.empty() || !removed_chat_ids.empty();
		}
	};

	bool ChatSummaryHasPendingInteraction(const nlohmann::json& chat)
	{
		const auto acp_it = chat.find("acpSession");
		if (acp_it == chat.end() || !acp_it->is_object())
		{
			return false;
		}
		const auto permission_it = acp_it->find("pendingPermission");
		if (permission_it != acp_it->end() && !permission_it->is_null())
		{
			return true;
		}
		const auto user_input_it = acp_it->find("pendingUserInput");
		return user_input_it != acp_it->end() && !user_input_it->is_null();
	}

	nlohmann::json ChatOrderForPatch(const uam::AppState& app)
	{
		nlohmann::json chat_order = nlohmann::json::array();
		for (const ChatSession& chat : app.chats)
		{
			chat_order.push_back(chat.id);
		}
		return chat_order;
	}

	nlohmann::json SelectedChatMessagesForPatch(const uam::AppState& app, const std::string& chat_id)
	{
		const ChatSession* app_chat = ChatDomainService().FindChatById(app, chat_id);
		if (app_chat == nullptr || !app_chat->messages_loaded)
		{
			return nullptr;
		}
		return uam::nlohmann_json::ArrayFieldOrEmpty(uam::StateSerializer::SerializeSession(*app_chat), "messages");
	}

	ChatPatchDiff BuildChatPatchDiff(const uam::AppState& app, const nlohmann::json& fingerprint_state, const std::string& selected_chat_id)
	{
		ChatPatchDiff diff;
		g_messages_push_deferred = false;
		std::unordered_set<std::string> current_chat_ids;

		const nlohmann::json chats = uam::nlohmann_json::ArrayFieldOrEmpty(fingerprint_state, "chats");
		for (const nlohmann::json& chat : chats)
		{
			if (!chat.is_object())
			{
				continue;
			}

			const std::string chat_id = uam::nlohmann_json::TrimmedStringValue(chat, {"id"});
			if (chat_id.empty())
			{
				continue;
			}

			current_chat_ids.insert(chat_id);

			const std::string chat_fingerprint = chat.dump();
			const std::string message_digest = uam::nlohmann_json::TrimmedStringValue(chat, {"messagesDigest"});
			diff.next_chat_summaries[chat_id] = chat_fingerprint;
			diff.next_message_digests[chat_id] = message_digest;

			const auto previous_chat_it = g_last_pushed_chat_summaries_by_chat_id.find(chat_id);
			if (previous_chat_it == g_last_pushed_chat_summaries_by_chat_id.end() || previous_chat_it->second != chat_fingerprint)
			{
				const auto now = std::chrono::steady_clock::now();
				const auto last_summary_push_it = g_last_summary_push_time_by_chat_id.find(chat_id);
				const bool summary_throttled = chat_id == selected_chat_id &&
				                               !ChatSummaryHasPendingInteraction(chat) &&
				                               last_summary_push_it != g_last_summary_push_time_by_chat_id.end() &&
				                               now - last_summary_push_it->second < kSelectedChatSummaryMinPushInterval;
				if (summary_throttled)
				{
					// Keep the previous summary baseline so the deferred change is
					// retried on a later tick instead of silently dropped.
					if (previous_chat_it != g_last_pushed_chat_summaries_by_chat_id.end())
					{
						diff.next_chat_summaries[chat_id] = previous_chat_it->second;
					}
					else
					{
						diff.next_chat_summaries.erase(chat_id);
					}
					g_messages_push_deferred = true;
				}
				else
				{
					diff.changed_chats.push_back(chat);
					g_last_summary_push_time_by_chat_id[chat_id] = now;
				}
			}

			const auto previous_digest_it = g_last_pushed_message_digests_by_chat_id.find(chat_id);
			const bool selected_chat = chat_id == selected_chat_id;
			const bool messages_changed = previous_digest_it == g_last_pushed_message_digests_by_chat_id.end() || previous_digest_it->second != message_digest;
			if (selected_chat && messages_changed)
			{
				const auto now = std::chrono::steady_clock::now();
				const auto last_push_it = g_last_messages_push_time_by_chat_id.find(chat_id);
				const bool throttled = last_push_it != g_last_messages_push_time_by_chat_id.end() && now - last_push_it->second < kSelectedChatMessagesMinPushInterval;
				if (throttled)
				{
					// Keep the previous digest baseline so the deferred change is
					// retried on a later tick instead of silently dropped.
					if (previous_digest_it != g_last_pushed_message_digests_by_chat_id.end())
					{
						diff.next_message_digests[chat_id] = previous_digest_it->second;
					}
					else
					{
						diff.next_message_digests.erase(chat_id);
					}
					g_messages_push_deferred = true;
				}
				else
				{
					const nlohmann::json messages = SelectedChatMessagesForPatch(app, chat_id);
					if (!messages.is_null())
					{
						diff.messages_by_chat_id[chat_id] = messages;
						g_last_messages_push_time_by_chat_id[chat_id] = now;
					}
				}
			}
		}

		for (const auto& entry : g_last_pushed_chat_summaries_by_chat_id)
		{
			if (!current_chat_ids.contains(entry.first))
			{
				diff.removed_chat_ids.push_back(entry.first);
			}
		}

		return diff;
	}

	void ApplyChatPatchDiff(nlohmann::json& data, const uam::AppState& app, ChatPatchDiff diff)
	{
		const bool include_chat_order = diff.HasChatListChange();
		if (!diff.changed_chats.empty())
		{
			data["chats"] = std::move(diff.changed_chats);
		}
		if (!diff.messages_by_chat_id.empty())
		{
			data["messagesByChatId"] = std::move(diff.messages_by_chat_id);
		}
		if (!diff.removed_chat_ids.empty())
		{
			data["removedChatIds"] = std::move(diff.removed_chat_ids);
		}
		if (include_chat_order)
		{
			data["chatOrder"] = ChatOrderForPatch(app);
		}

		g_last_pushed_chat_summaries_by_chat_id = std::move(diff.next_chat_summaries);
		g_last_pushed_message_digests_by_chat_id = std::move(diff.next_message_digests);
	}

	std::string BuildStatePatchMessage(const uam::AppState& app, const nlohmann::json& fingerprint_state)
	{
		const auto started = std::chrono::steady_clock::now();
		const std::string selected_chat_id = ChatDomainService().SelectedChatId(app);
		nlohmann::json data = nlohmann::json::object();

		data["stateRevision"] = app.state_revision;

		const nlohmann::json folders = uam::nlohmann_json::ArrayFieldOrEmpty(fingerprint_state, "folders");
		AddChangedJsonField(data, "folders", folders, g_last_pushed_folders_fingerprint);

		const nlohmann::json providers = uam::nlohmann_json::ArrayFieldOrEmpty(fingerprint_state, "providers");
		AddChangedJsonField(data, "providers", providers, g_last_pushed_providers_fingerprint);

		const nlohmann::json settings = SerializeSettingsForPatch(app);
		AddChangedJsonField(data, "settings", settings, g_last_pushed_settings_fingerprint);

		const nlohmann::json memory = uam::nlohmann_json::ObjectFieldOrEmpty(fingerprint_state, "memoryActivity");
		AddChangedJsonField(data, "memoryActivity", memory, g_last_pushed_memory_fingerprint);

		const nlohmann::json shell_actions = uam::nlohmann_json::ArrayFieldOrEmpty(fingerprint_state, "shellActions");
		AddChangedJsonField(data, "shellActions", shell_actions, g_last_pushed_shell_actions_fingerprint);

		const std::string shell_notification = uam::nlohmann_json::TrimmedStringValue(fingerprint_state, {"shellActionNotification"});
		if (shell_notification != g_last_pushed_shell_action_notification)
		{
			data["shellActionNotification"] = shell_notification;
			g_last_pushed_shell_action_notification = shell_notification;
		}

		if (selected_chat_id != g_last_pushed_selected_chat_id)
		{
			data["selectedChatId"] = uam::nlohmann_json::StringOrNull(selected_chat_id);
			g_last_pushed_selected_chat_id = selected_chat_id;
		}

		ApplyChatPatchDiff(data, app, BuildChatPatchDiff(app, fingerprint_state, selected_chat_id));

		nlohmann::json msg = PushMessage(kPushTypeStatePatch);
		msg["data"] = std::move(data);
		const std::string message = msg.dump();
		MaybeLogLargePatch(message, static_cast<int>(uam::nlohmann_json::ArrayFieldOrEmpty(msg["data"], "chats").size()), std::chrono::steady_clock::now() - started);
		return message;
	}

	void StripVolatileCliDebugTelemetry(nlohmann::json& state)
	{
		const auto cli_debug_it = state.find("cliDebug");
		if (cli_debug_it == state.end() || !cli_debug_it->is_object())
		{
			return;
		}

		auto& cli_debug = *cli_debug_it;
		const auto terminals_it = cli_debug.find("terminals");
		if (terminals_it == cli_debug.end() || !terminals_it->is_array())
		{
			return;
		}

		for (auto& terminal : *terminals_it)
		{
			if (!terminal.is_object())
			{
				continue;
			}

			terminal.erase("lastUserInputAt");
			terminal.erase("lastAiOutputAt");
			terminal.erase("lastPolledAt");
		}
	}

	void StripVolatileAcpWaitTelemetry(nlohmann::json& state)
	{
		auto chats_it = state.find("chats");
		if (chats_it == state.end() || !chats_it->is_array())
		{
			return;
		}

		for (auto& chat : *chats_it)
		{
			if (!chat.is_object())
			{
				continue;
			}

			auto acp_it = chat.find("acpSession");
			if (acp_it != chat.end() && acp_it->is_object())
			{
				acp_it->erase("waitSeconds");
			}
		}
	}

	std::string BuildStateFingerprintFromState(const nlohmann::json& fingerprint_state)
	{
		nlohmann::json state = fingerprint_state;
		StripVolatileCliDebugTelemetry(state);
		StripVolatileAcpWaitTelemetry(state);
		return state.dump();
	}

	std::string BuildStateFingerprint(const uam::AppState& app)
	{
		return BuildStateFingerprintFromState(uam::StateSerializer::SerializeFingerprint(app));
	}

	void BumpStateRevision(uam::AppState& app)
	{
		++app.state_revision;
	}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public push functions
// ---------------------------------------------------------------------------

namespace uam
{

	std::string SettingsPatchForTests(const AppState& app)
	{
		return SerializeSettingsForPatch(app).dump();
	}

	bool HasDeferredStatePush()
	{
		return g_messages_push_deferred;
	}

	bool PushStateUpdateIfChanged(CefRefPtr<CefBrowser> browser, AppState& app)
	{
		const nlohmann::json fingerprint_state = uam::StateSerializer::SerializeFingerprint(app);
		const std::string fingerprint = BuildStateFingerprintFromState(fingerprint_state);
		// A deferred (throttled) selected-chat messages payload must still be
		// flushed even if no further state changes arrive after streaming ends.
		if (fingerprint == g_last_pushed_state_fingerprint && !g_messages_push_deferred)
		{
			return false;
		}

		BumpStateRevision(app);
		const std::string message = BuildStatePatchMessage(app, fingerprint_state);
		g_last_pushed_state_fingerprint = fingerprint;
		PostPush(browser, message);
		return true;
	}

	void PushStateUpdate(CefRefPtr<CefBrowser> browser, AppState& app)
	{
		BumpStateRevision(app);
		const std::string message = BuildFullStateUpdateMessage(app);
		g_last_pushed_state_fingerprint = BuildStateFingerprint(app);
		ResetPatchBaselines(app);
		PostPush(browser, message);
	}

	void PushStreamToken(CefRefPtr<CefBrowser> browser, const std::string& chat_id, const std::string& token)
	{
		nlohmann::json msg = PushMessage(kPushTypeStreamToken);
		msg["chatId"] = chat_id;
		msg["token"] = token;
		PostPush(browser, msg.dump());
	}

	void PushStreamDone(CefRefPtr<CefBrowser> browser, const std::string& chat_id)
	{
		nlohmann::json msg = PushMessage(kPushTypeStreamDone);
		msg["chatId"] = chat_id;
		PostPush(browser, msg.dump());
	}

	void PushCliOutput(CefRefPtr<CefBrowser> browser, const std::string& frontend_chat_id, const std::string& source_chat_id, const std::string& terminal_id, const std::string& raw_bytes)
	{
		nlohmann::json msg = PushMessage(kPushTypeCliOutput);
		msg["sessionId"] = frontend_chat_id;
		msg["sourceChatId"] = source_chat_id;
		msg["terminalId"] = terminal_id;
		msg["data"] = uam::base64::Encode(raw_bytes);
		PostPush(browser, msg.dump());
	}

} // namespace uam

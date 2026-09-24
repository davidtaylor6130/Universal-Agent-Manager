#include "cef/cef_push.h"
#include "cef/state_serializer.h"

#include "app/chat_domain_service.h"

#include "common/config/settings_frontend_json.h"
#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/wrapper/cef_helpers.h"

#include "common/utils/base64.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/nlohmann_json_utils.h"

#include <chrono>
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
	constexpr const char* kPushTypeDictation = "dictation";
	constexpr long long kStatePatchSlowSerializationMs = 8;
	constexpr std::size_t kStatePatchLargeMessageBytes = 64 * 1024;
	// The selected chat's summary carries the live turn timeline (turnEvents),
	// which grows on nearly every tick during a turn; 10 updates per second is
	// plenty for streamed text and tool-call progress. Summaries that introduce
	// a pending permission or user-input request bypass the throttle so
	// interaction prompts are never delayed.
	constexpr std::chrono::milliseconds kSelectedChatSummaryMinPushInterval{100};

	std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_last_summary_push_time_by_chat_id;
	bool g_state_push_deferred = false;
	std::unordered_map<std::string, nlohmann::json> g_last_pushed_chat_summaries_by_chat_id;
	std::string g_last_pushed_folders_fingerprint;
	std::string g_last_pushed_resource_collections_fingerprint;
	std::string g_last_pushed_providers_fingerprint;
	std::string g_last_pushed_provider_model_catalogs_fingerprint;
	std::string g_last_pushed_settings_fingerprint;
	std::string g_last_pushed_memory_fingerprint;
	std::string g_last_pushed_cli_version_manager_fingerprint;
	std::string g_last_pushed_shell_actions_fingerprint;
	std::string g_last_pushed_shell_action_notification;
	std::string g_last_pushed_status_line;
	std::string g_last_pushed_selected_chat_id;
	std::string g_last_pushed_chat_order_fingerprint;

	std::string DumpFrontendJson(const nlohmann::json& value)
	{
		return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
	}

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

	void StripVolatileAcpWaitTelemetry(nlohmann::json& state);
	nlohmann::json ChatOrderForPatch(const uam::AppState& app);

	void ResetPatchBaselines(const uam::AppState& app)
	{
		nlohmann::json fingerprint_state = uam::StateSerializer::SerializeFingerprint(app);
		StripVolatileAcpWaitTelemetry(fingerprint_state);
		g_last_pushed_folders_fingerprint = DumpFrontendJson(fingerprint_state.at("folders"));
		g_last_pushed_resource_collections_fingerprint = DumpFrontendJson(fingerprint_state.at("resourceCollections"));
		g_last_pushed_providers_fingerprint = DumpFrontendJson(fingerprint_state.at("providers"));
		g_last_pushed_provider_model_catalogs_fingerprint = DumpFrontendJson(fingerprint_state.at("providerModelCatalogs"));
		g_last_pushed_settings_fingerprint = DumpFrontendJson(SerializeSettingsForPatch(app));
		g_last_pushed_memory_fingerprint = DumpFrontendJson(fingerprint_state.at("memoryActivity"));
		g_last_pushed_cli_version_manager_fingerprint = DumpFrontendJson(fingerprint_state.at("cliVersionManager"));
		g_last_pushed_shell_actions_fingerprint = DumpFrontendJson(fingerprint_state.at("shellActions"));
		g_last_pushed_shell_action_notification = uam::nlohmann_json::TrimmedStringValue(fingerprint_state, {"shellActionNotification"});
		g_last_pushed_status_line = fingerprint_state.value("statusLine", std::string{});
		g_last_pushed_selected_chat_id = ChatDomainService().SelectedChatId(app);
		g_last_pushed_chat_order_fingerprint = DumpFrontendJson(ChatOrderForPatch(app));
		g_last_pushed_chat_summaries_by_chat_id.clear();
		g_last_summary_push_time_by_chat_id.clear();
		g_state_push_deferred = false;

		const nlohmann::json& chats = fingerprint_state.at("chats");

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

			g_last_pushed_chat_summaries_by_chat_id[chat_id] = chat;
		}
	}

	std::string BuildFullStateUpdateMessage(const uam::AppState& app)
	{
		nlohmann::json msg = PushMessage(kPushTypeStateUpdate);
		msg["data"] = uam::StateSerializer::Serialize(app);
		return DumpFrontendJson(msg);
	}

	void MaybeLogLargePatch(const std::string& message, int changed_chat_count, std::chrono::steady_clock::duration elapsed)
	{
#ifndef NDEBUG
		const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
		if (elapsed_ms > kStatePatchSlowSerializationMs || message.size() > kStatePatchLargeMessageBytes)
		{
			uam::diagnostics::Write("[UAM] statePatch serialization took " + std::to_string(elapsed_ms) + "ms, bytes=" + std::to_string(message.size()) + ", changedChats=" + std::to_string(changed_chat_count));
		}
#else
		(void)message;
		(void)changed_chat_count;
		(void)elapsed;
#endif
	}

	void AddChangedJsonField(nlohmann::json& patch_data, const char* field_name, const nlohmann::json& value, std::string& last_fingerprint)
	{
		const std::string fingerprint = DumpFrontendJson(value);
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
		nlohmann::json removed_chat_ids = nlohmann::json::array();
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

	ChatPatchDiff BuildChatPatchDiff(const uam::AppState& app, const nlohmann::json& fingerprint_state, const std::string& selected_chat_id)
	{
		ChatPatchDiff diff;
		g_state_push_deferred = false;
		std::unordered_set<std::string> current_chat_ids;

		const nlohmann::json& chats = fingerprint_state.at("chats");
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

			const auto previous_chat_it = g_last_pushed_chat_summaries_by_chat_id.find(chat_id);
			if (previous_chat_it == g_last_pushed_chat_summaries_by_chat_id.end() || previous_chat_it->second != chat)
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
					g_state_push_deferred = true;
				}
				else
				{
					diff.changed_chats.push_back(chat);
					g_last_summary_push_time_by_chat_id[chat_id] = now;
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

	void ApplyChatPatchDiff(nlohmann::json& data, ChatPatchDiff diff)
	{
		if (!diff.changed_chats.empty())
		{
			for (const nlohmann::json& chat : diff.changed_chats)
			{
				const std::string chat_id = uam::nlohmann_json::TrimmedStringValue(chat, {"id"});
				g_last_pushed_chat_summaries_by_chat_id[chat_id] = chat;
			}
			data["chats"] = std::move(diff.changed_chats);
		}
		if (!diff.removed_chat_ids.empty())
		{
			for (const nlohmann::json& chat_id : diff.removed_chat_ids)
			{
				g_last_pushed_chat_summaries_by_chat_id.erase(chat_id.get<std::string>());
			}
			data["removedChatIds"] = std::move(diff.removed_chat_ids);
		}
	}

	std::string BuildStatePatchMessage(const uam::AppState& app, const nlohmann::json& fingerprint_state, bool* has_payload = nullptr)
	{
		const auto started = std::chrono::steady_clock::now();
		const std::string selected_chat_id = ChatDomainService().SelectedChatId(app);
		nlohmann::json data = nlohmann::json::object();

		data["stateRevision"] = app.state_revision + 1;

		AddChangedJsonField(data, "folders", fingerprint_state.at("folders"), g_last_pushed_folders_fingerprint);

		AddChangedJsonField(data, "resourceCollections", fingerprint_state.at("resourceCollections"), g_last_pushed_resource_collections_fingerprint);

		AddChangedJsonField(data, "providers", fingerprint_state.at("providers"), g_last_pushed_providers_fingerprint);

		AddChangedJsonField(data, "providerModelCatalogs", fingerprint_state.at("providerModelCatalogs"), g_last_pushed_provider_model_catalogs_fingerprint);

		const nlohmann::json settings = SerializeSettingsForPatch(app);
		AddChangedJsonField(data, "settings", settings, g_last_pushed_settings_fingerprint);

		AddChangedJsonField(data, "memoryActivity", fingerprint_state.at("memoryActivity"), g_last_pushed_memory_fingerprint);

		AddChangedJsonField(data, "cliVersionManager", fingerprint_state.at("cliVersionManager"), g_last_pushed_cli_version_manager_fingerprint);

		AddChangedJsonField(data, "shellActions", fingerprint_state.at("shellActions"), g_last_pushed_shell_actions_fingerprint);

		const std::string shell_notification = uam::nlohmann_json::TrimmedStringValue(fingerprint_state, {"shellActionNotification"});
		if (shell_notification != g_last_pushed_shell_action_notification)
		{
			data["shellActionNotification"] = shell_notification;
			g_last_pushed_shell_action_notification = shell_notification;
		}

		const std::string status_line = fingerprint_state.value("statusLine", std::string{});
		if (status_line != g_last_pushed_status_line)
		{
			data["statusLine"] = status_line;
			g_last_pushed_status_line = status_line;
		}

		if (selected_chat_id != g_last_pushed_selected_chat_id)
		{
			data["selectedChatId"] = uam::nlohmann_json::StringOrNull(selected_chat_id);
			g_last_pushed_selected_chat_id = selected_chat_id;
		}

		ApplyChatPatchDiff(data, BuildChatPatchDiff(app, fingerprint_state, selected_chat_id));
		AddChangedJsonField(data, "chatOrder", ChatOrderForPatch(app), g_last_pushed_chat_order_fingerprint);
		if (has_payload != nullptr)
		{
			*has_payload = data.size() > 1;
		}

		nlohmann::json msg = PushMessage(kPushTypeStatePatch);
		msg["data"] = std::move(data);
		const std::string message = DumpFrontendJson(msg);
		MaybeLogLargePatch(message, static_cast<int>(uam::nlohmann_json::ArrayFieldOrEmpty(msg["data"], "chats").size()), std::chrono::steady_clock::now() - started);
		return message;
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
		return DumpFrontendJson(SerializeSettingsForPatch(app));
	}

	std::string StatePatchForTests(const AppState& before, const AppState& after)
	{
		ResetPatchBaselines(before);
		nlohmann::json fingerprint_state = StateSerializer::SerializeFingerprint(after);
		StripVolatileAcpWaitTelemetry(fingerprint_state);
		return BuildStatePatchMessage(after, fingerprint_state);
	}

	bool HasDeferredStatePush()
	{
		return g_state_push_deferred;
	}

	bool PushStateUpdateIfChanged(CefRefPtr<CefBrowser> browser, AppState& app)
	{
		const auto fingerprint_started = std::chrono::steady_clock::now();
		nlohmann::json fingerprint_state = uam::StateSerializer::SerializeFingerprint(app);
		const auto fingerprint_finished = std::chrono::steady_clock::now();
		static auto last_slow_fingerprint_report = std::chrono::steady_clock::time_point::min();
		const auto fingerprint_ms = std::chrono::duration_cast<std::chrono::milliseconds>(fingerprint_finished - fingerprint_started);
		if (fingerprint_ms >= std::chrono::milliseconds(50) &&
		    (last_slow_fingerprint_report == std::chrono::steady_clock::time_point::min() ||
		     fingerprint_finished - last_slow_fingerprint_report >= std::chrono::seconds(5)))
		{
			uam::diagnostics::Write("[performance] State fingerprint took " + std::to_string(fingerprint_ms.count()) +
			                        " ms across " + std::to_string(app.chats.size()) + " chats.");
			last_slow_fingerprint_report = fingerprint_finished;
		}
		StripVolatileAcpWaitTelemetry(fingerprint_state);
		bool has_payload = false;
		const std::string message = BuildStatePatchMessage(app, fingerprint_state, &has_payload);
		if (!has_payload)
		{
			return false;
		}
		BumpStateRevision(app);
		PostPush(browser, message);
		return true;
	}

	void PushStateUpdate(CefRefPtr<CefBrowser> browser, AppState& app)
	{
		BumpStateRevision(app);
		const std::string message = BuildFullStateUpdateMessage(app);
		ResetPatchBaselines(app);
		PostPush(browser, message);
	}

	void PushStreamToken(CefRefPtr<CefBrowser> browser, const std::string& chat_id, int message_index, const std::string& token)
	{
		nlohmann::json msg = PushMessage(kPushTypeStreamToken);
		msg["chatId"] = chat_id;
		msg["token"] = token;
		msg["messageIndex"] = message_index;
		PostPush(browser, DumpFrontendJson(msg));
	}

	void PushStreamDone(CefRefPtr<CefBrowser> browser, const std::string& chat_id)
	{
		nlohmann::json msg = PushMessage(kPushTypeStreamDone);
		msg["chatId"] = chat_id;
		PostPush(browser, DumpFrontendJson(msg));
	}

	void PushCliOutput(CefRefPtr<CefBrowser> browser, const std::string& frontend_chat_id, const std::string& source_chat_id, const std::string& terminal_id, const std::string& raw_bytes)
	{
		nlohmann::json msg = PushMessage(kPushTypeCliOutput);
		msg["sessionId"] = frontend_chat_id;
		msg["sourceChatId"] = source_chat_id;
		msg["terminalId"] = terminal_id;
		msg["data"] = uam::base64::Encode(raw_bytes);
		PostPush(browser, DumpFrontendJson(msg));
	}

	void PushDictationEvent(CefRefPtr<CefBrowser> browser, const DictationEvent& event)
	{
		nlohmann::json msg = PushMessage(kPushTypeDictation);
		msg["dictationId"] = event.dictation_id;
		switch (event.type)
		{
			case DictationEventType::Interim:
				msg["event"] = "interim";
				msg["text"] = event.text;
				break;
			case DictationEventType::Final:
				msg["event"] = "final";
				msg["text"] = event.text;
				break;
			case DictationEventType::Error:
				msg["event"] = "error";
				msg["message"] = event.text;
				break;
			case DictationEventType::End:
				msg["event"] = "end";
				break;
		}
		PostPush(browser, DumpFrontendJson(msg));
	}

} // namespace uam

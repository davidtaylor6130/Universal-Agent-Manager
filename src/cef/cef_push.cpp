#include "cef/cef_push.h"
#include "cef/state_serializer.h"

#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/wrapper/cef_helpers.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace
{
std::string g_last_pushed_state_fingerprint;
std::unordered_map<std::string, std::string> g_last_pushed_message_digests_by_chat_id;
std::unordered_map<std::string, std::string> g_last_pushed_chat_summaries_by_chat_id;
std::string g_last_pushed_folders_fingerprint;
std::string g_last_pushed_providers_fingerprint;
std::string g_last_pushed_settings_fingerprint;
std::string g_last_pushed_memory_fingerprint;
std::string g_last_pushed_selected_chat_id;

/// Escapes a raw string so it can be safely embedded inside a JS string literal.
std::string JsEscape(const std::string& s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (unsigned char c : s)
	{
		switch (c)
		{
		case '\\': out += "\\\\"; break;
		case '"':  out += "\\\""; break;
		case '\n': out += "\\n";  break;
		case '\r': out += "\\r";  break;
		case '\t': out += "\\t";  break;
		default:
			if (c < 0x20)
			{
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			}
			else
			{
				out += static_cast<char>(c);
			}
		}
	}
	return out;
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

std::string SelectedChatId(const uam::AppState& app)
{
	if (app.selected_chat_index >= 0 && app.selected_chat_index < static_cast<int>(app.chats.size()))
	{
		return app.chats[static_cast<std::size_t>(app.selected_chat_index)].id;
	}
	return "";
}

nlohmann::json SerializeSettingsForPatch(const uam::AppState& app)
{
	nlohmann::json settings;
	settings["activeProviderId"] = app.settings.active_provider_id;
	settings["theme"] = app.settings.ui_theme;
	settings["memoryEnabledDefault"] = app.settings.memory_enabled_default;
	settings["memoryIdleDelaySeconds"] = app.settings.memory_idle_delay_seconds;
	settings["memoryRecallBudgetBytes"] = app.settings.memory_recall_budget_bytes;
	settings["memoryLastStatus"] = app.memory_last_status;

	nlohmann::json bindings = nlohmann::json::object();
	for (const auto& entry : app.settings.memory_worker_bindings)
	{
		bindings[entry.first] = {
			{"workerProviderId", entry.second.worker_provider_id},
			{"workerModelId", entry.second.worker_model_id},
		};
	}
	settings["memoryWorkerBindings"] = std::move(bindings);
	return settings;
}

void ResetPatchBaselines(const uam::AppState& app)
{
	const nlohmann::json fingerprint_state = uam::StateSerializer::SerializeFingerprint(app);
	g_last_pushed_folders_fingerprint = fingerprint_state.value("folders", nlohmann::json::array()).dump();
	g_last_pushed_providers_fingerprint = fingerprint_state.value("providers", nlohmann::json::array()).dump();
	g_last_pushed_settings_fingerprint = SerializeSettingsForPatch(app).dump();
	g_last_pushed_memory_fingerprint = fingerprint_state.value("memoryActivity", nlohmann::json::object()).dump();
	g_last_pushed_selected_chat_id = SelectedChatId(app);
	g_last_pushed_message_digests_by_chat_id.clear();
	g_last_pushed_chat_summaries_by_chat_id.clear();

	const nlohmann::json chats = fingerprint_state.value("chats", nlohmann::json::array());
	if (!chats.is_array())
	{
		return;
	}

	for (const nlohmann::json& chat : chats)
	{
		if (!chat.is_object()) continue;
		const std::string chat_id = chat.value("id", "");
		if (chat_id.empty()) continue;
		g_last_pushed_chat_summaries_by_chat_id[chat_id] = chat.dump();
		g_last_pushed_message_digests_by_chat_id[chat_id] = chat.value("messagesDigest", "");
	}
}

std::string BuildFullStateUpdateMessage(const uam::AppState& app)
{
	nlohmann::json msg;
	msg["type"] = "stateUpdate";
	msg["data"] = uam::StateSerializer::Serialize(app);
	return msg.dump();
}

void MaybeLogLargePatch(const std::string& message, const int changed_chat_count, const std::chrono::steady_clock::duration elapsed)
{
#ifndef NDEBUG
	const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
	if (elapsed_ms > 8 || message.size() > 64 * 1024)
	{
		std::cerr << "[UAM] statePatch serialization took " << elapsed_ms << "ms, bytes=" << message.size() << ", changedChats=" << changed_chat_count << "\n";
	}
#else
	(void)message;
	(void)changed_chat_count;
	(void)elapsed;
#endif
}

std::string BuildStatePatchMessage(const uam::AppState& app)
{
	const auto started = std::chrono::steady_clock::now();
	const nlohmann::json fingerprint_state = uam::StateSerializer::SerializeFingerprint(app);
	const std::string selected_chat_id = SelectedChatId(app);
	nlohmann::json data = nlohmann::json::object();

	data["stateRevision"] = app.state_revision;

	const nlohmann::json folders = fingerprint_state.value("folders", nlohmann::json::array());
	const std::string folders_fingerprint = folders.dump();
	if (folders_fingerprint != g_last_pushed_folders_fingerprint)
	{
		data["folders"] = folders;
		g_last_pushed_folders_fingerprint = folders_fingerprint;
	}

	const nlohmann::json providers = fingerprint_state.value("providers", nlohmann::json::array());
	const std::string providers_fingerprint = providers.dump();
	if (providers_fingerprint != g_last_pushed_providers_fingerprint)
	{
		data["providers"] = providers;
		g_last_pushed_providers_fingerprint = providers_fingerprint;
	}

	const nlohmann::json settings = SerializeSettingsForPatch(app);
	const std::string settings_fingerprint = settings.dump();
	if (settings_fingerprint != g_last_pushed_settings_fingerprint)
	{
		data["settings"] = settings;
		g_last_pushed_settings_fingerprint = settings_fingerprint;
	}

	const nlohmann::json memory = fingerprint_state.value("memoryActivity", nlohmann::json::object());
	const std::string memory_fingerprint = memory.dump();
	if (memory_fingerprint != g_last_pushed_memory_fingerprint)
	{
		data["memoryActivity"] = memory;
		g_last_pushed_memory_fingerprint = memory_fingerprint;
	}

	if (selected_chat_id != g_last_pushed_selected_chat_id)
	{
		data["selectedChatId"] = selected_chat_id.empty() ? nlohmann::json(nullptr) : nlohmann::json(selected_chat_id);
		g_last_pushed_selected_chat_id = selected_chat_id;
	}

	nlohmann::json changed_chats = nlohmann::json::array();
	nlohmann::json messages_by_chat_id = nlohmann::json::object();
	std::unordered_map<std::string, std::string> next_chat_summaries;
	std::unordered_map<std::string, std::string> next_message_digests;
	std::unordered_set<std::string> current_chat_ids;

	const nlohmann::json chats = fingerprint_state.value("chats", nlohmann::json::array());
	if (chats.is_array())
	{
		for (const nlohmann::json& chat : chats)
		{
			if (!chat.is_object()) continue;
			const std::string chat_id = chat.value("id", "");
			if (chat_id.empty()) continue;
			current_chat_ids.insert(chat_id);

			const std::string chat_fingerprint = chat.dump();
			const std::string message_digest = chat.value("messagesDigest", "");
			next_chat_summaries[chat_id] = chat_fingerprint;
			next_message_digests[chat_id] = message_digest;

			const auto previous_chat_it = g_last_pushed_chat_summaries_by_chat_id.find(chat_id);
			if (previous_chat_it == g_last_pushed_chat_summaries_by_chat_id.end() || previous_chat_it->second != chat_fingerprint)
			{
				changed_chats.push_back(chat);
			}

			const auto previous_digest_it = g_last_pushed_message_digests_by_chat_id.find(chat_id);
			const bool selected_chat = chat_id == selected_chat_id;
			const bool messages_changed = previous_digest_it == g_last_pushed_message_digests_by_chat_id.end() || previous_digest_it->second != message_digest;
			if (selected_chat && messages_changed)
			{
				const auto app_chat_it = std::find_if(app.chats.begin(), app.chats.end(), [&](const ChatSession& candidate) {
					return candidate.id == chat_id;
				});
				if (app_chat_it != app.chats.end())
				{
					messages_by_chat_id[chat_id] = uam::StateSerializer::SerializeSession(*app_chat_it).value("messages", nlohmann::json::array());
				}
			}
		}
	}

	nlohmann::json removed_chat_ids = nlohmann::json::array();
	for (const auto& entry : g_last_pushed_chat_summaries_by_chat_id)
	{
		if (current_chat_ids.find(entry.first) == current_chat_ids.end())
		{
			removed_chat_ids.push_back(entry.first);
		}
	}

	if (!changed_chats.empty()) data["chats"] = std::move(changed_chats);
	if (!messages_by_chat_id.empty()) data["messagesByChatId"] = std::move(messages_by_chat_id);
	if (!removed_chat_ids.empty()) data["removedChatIds"] = std::move(removed_chat_ids);
	if (data.contains("chats") || data.contains("removedChatIds"))
	{
		nlohmann::json chat_order = nlohmann::json::array();
		for (const ChatSession& chat : app.chats)
		{
			chat_order.push_back(chat.id);
		}
		data["chatOrder"] = std::move(chat_order);
	}

	g_last_pushed_chat_summaries_by_chat_id = std::move(next_chat_summaries);
	g_last_pushed_message_digests_by_chat_id = std::move(next_message_digests);

	nlohmann::json msg;
	msg["type"] = "statePatch";
	msg["data"] = std::move(data);
	const std::string message = msg.dump();
	MaybeLogLargePatch(message, static_cast<int>(msg["data"].value("chats", nlohmann::json::array()).size()), std::chrono::steady_clock::now() - started);
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

std::string BuildStateFingerprint(const uam::AppState& app)
{
	nlohmann::json state = uam::StateSerializer::SerializeFingerprint(app);
	StripVolatileCliDebugTelemetry(state);
	StripVolatileAcpWaitTelemetry(state);
	return state.dump();
}

void BumpStateRevision(uam::AppState& app)
{
	++app.state_revision;
}

/// Simple base64 encoder for raw binary data from PTY reads.
static const char kBase64Chars[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const std::string& input)
{
	std::string out;
	out.reserve(((input.size() + 2) / 3) * 4);

	std::size_t i = 0;
	unsigned char buf[3];
	while (i + 3 <= input.size())
	{
		buf[0] = static_cast<unsigned char>(input[i]);
		buf[1] = static_cast<unsigned char>(input[i + 1]);
		buf[2] = static_cast<unsigned char>(input[i + 2]);
		out += kBase64Chars[(buf[0] >> 2) & 0x3F];
		out += kBase64Chars[((buf[0] & 0x03) << 4) | ((buf[1] >> 4) & 0x0F)];
		out += kBase64Chars[((buf[1] & 0x0F) << 2) | ((buf[2] >> 6) & 0x03)];
		out += kBase64Chars[buf[2] & 0x3F];
		i += 3;
	}

	std::size_t remaining = input.size() - i;
	if (remaining == 1)
	{
		buf[0] = static_cast<unsigned char>(input[i]);
		out += kBase64Chars[(buf[0] >> 2) & 0x3F];
		out += kBase64Chars[(buf[0] & 0x03) << 4];
		out += "==";
	}
	else if (remaining == 2)
	{
		buf[0] = static_cast<unsigned char>(input[i]);
		buf[1] = static_cast<unsigned char>(input[i + 1]);
		out += kBase64Chars[(buf[0] >> 2) & 0x3F];
		out += kBase64Chars[((buf[0] & 0x03) << 4) | ((buf[1] >> 4) & 0x0F)];
		out += kBase64Chars[(buf[1] & 0x0F) << 2];
		out += '=';
	}
	return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public push functions
// ---------------------------------------------------------------------------

namespace uam
{

bool PushStateUpdateIfChanged(CefRefPtr<CefBrowser> browser, const AppState& app)
{
	const std::string fingerprint = BuildStateFingerprint(app);
	if (fingerprint == g_last_pushed_state_fingerprint)
	{
		return false;
	}

	BumpStateRevision(const_cast<AppState&>(app));
	const std::string message = BuildStatePatchMessage(app);
	g_last_pushed_state_fingerprint = fingerprint;
	PostPush(browser, message);
	return true;
}

void PushStateUpdate(CefRefPtr<CefBrowser> browser, const AppState& app)
{
	BumpStateRevision(const_cast<AppState&>(app));
	const std::string message = BuildFullStateUpdateMessage(app);
	g_last_pushed_state_fingerprint = BuildStateFingerprint(app);
	ResetPatchBaselines(app);
	PostPush(browser, message);
}

void PushStreamToken(CefRefPtr<CefBrowser> browser,
                     const std::string&    chat_id,
                     const std::string&    token)
{
	nlohmann::json msg;
	msg["type"]   = "streamToken";
	msg["chatId"] = chat_id;
	msg["token"]  = token;
	PostPush(browser, msg.dump());
}

void PushStreamDone(CefRefPtr<CefBrowser> browser, const std::string& chat_id)
{
	nlohmann::json msg;
	msg["type"]   = "streamDone";
	msg["chatId"] = chat_id;
	PostPush(browser, msg.dump());
}

void PushCliOutput(CefRefPtr<CefBrowser> browser,
                   const std::string&    frontend_chat_id,
                   const std::string&    source_chat_id,
                   const std::string&    terminal_id,
                   const std::string&    raw_bytes)
{
	nlohmann::json msg;
	msg["type"] = "cliOutput";
	msg["sessionId"] = frontend_chat_id;
	msg["sourceChatId"] = source_chat_id;
	msg["terminalId"] = terminal_id;
	msg["data"] = Base64Encode(raw_bytes);
	PostPush(browser, msg.dump());
}

} // namespace uam

#include "cef/cef_push.h"
#include "cef/state_serializer.h"

#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/wrapper/cef_helpers.h"

#include <unordered_map>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace
{
std::string g_last_pushed_state_fingerprint;
std::unordered_map<std::string, std::string> g_last_pushed_message_digests_by_chat_id;

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

void PruneUnchangedChatMessages(nlohmann::json& state, const uam::AppState& app)
{
	const std::string selected_chat_id = SelectedChatId(app);
	auto chats_it = state.find("chats");
	if (chats_it == state.end() || !chats_it->is_array())
	{
		return;
	}

	std::unordered_map<std::string, std::string> next_digests;
	for (auto& chat : *chats_it)
	{
		if (!chat.is_object())
		{
			continue;
		}

		const std::string chat_id = chat.value("id", "");
		const std::string digest = chat.value("messagesDigest", "");
		if (chat_id.empty())
		{
			continue;
		}

		next_digests[chat_id] = digest;
		const auto previous_it = g_last_pushed_message_digests_by_chat_id.find(chat_id);
		const bool messages_changed = previous_it == g_last_pushed_message_digests_by_chat_id.end() || previous_it->second != digest;
		const bool selected_chat = chat_id == selected_chat_id;
		if (!messages_changed && !selected_chat)
		{
			chat.erase("messages");
		}
	}

	g_last_pushed_message_digests_by_chat_id = std::move(next_digests);
}

std::string BuildStateUpdateMessage(const uam::AppState& app, const bool incremental)
{
	nlohmann::json state = uam::StateSerializer::Serialize(app);
	if (incremental)
	{
		PruneUnchangedChatMessages(state, app);
	}

	nlohmann::json msg;
	msg["type"] = "stateUpdate";
	msg["data"] = state;
	return msg.dump();
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
	const std::string message = BuildStateUpdateMessage(app, true);
	g_last_pushed_state_fingerprint = fingerprint;
	PostPush(browser, message);
	return true;
}

void PushStateUpdate(CefRefPtr<CefBrowser> browser, const AppState& app)
{
	BumpStateRevision(const_cast<AppState&>(app));
	g_last_pushed_message_digests_by_chat_id.clear();
	const std::string message = BuildStateUpdateMessage(app, false);
	g_last_pushed_state_fingerprint = BuildStateFingerprint(app);
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

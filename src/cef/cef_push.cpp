#include "cef/cef_push.h"
#include "cef/state_serializer.h"

#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/wrapper/cef_helpers.h"

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace
{
std::string g_last_pushed_state_message;

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

std::string BuildStateUpdateMessage(const uam::AppState& app)
{
	const nlohmann::json state = uam::StateSerializer::Serialize(app);
	nlohmann::json msg;
	msg["type"] = "stateUpdate";
	msg["data"] = state;
	return msg.dump();
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
	const std::string message = BuildStateUpdateMessage(app);
	if (message == g_last_pushed_state_message)
	{
		return false;
	}

	g_last_pushed_state_message = message;
	PostPush(browser, message);
	return true;
}

void PushStateUpdate(CefRefPtr<CefBrowser> browser, const AppState& app)
{
	const std::string message = BuildStateUpdateMessage(app);
	g_last_pushed_state_message = message;
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

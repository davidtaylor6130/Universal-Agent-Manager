#pragma once

#include "cef/cef_includes.h"
#include "common/state/app_state.h"

#include <string>

namespace uam
{

/// <summary>
/// Serialises the full AppState and calls window.uamPush({type:"stateUpdate",...}) in the
/// browser's main frame.  Safe to call from any thread — marshals to the UI thread internally.
/// </summary>
bool PushStateUpdateIfChanged(CefRefPtr<CefBrowser> browser, const AppState& app);
void PushStateUpdate(CefRefPtr<CefBrowser> browser, const AppState& app);

/// <summary>
/// Delivers a single streaming token for the given chat session to the React frontend.
/// </summary>
void PushStreamToken(CefRefPtr<CefBrowser> browser,
                     const std::string&    chat_id,
                     const std::string&    token);

/// <summary>
/// Signals that the streaming response for chat_id has completed.
/// </summary>
void PushStreamDone(CefRefPtr<CefBrowser> browser, const std::string& chat_id);

/// <summary>
/// Forwards raw PTY bytes (base64-encoded) from a CLI terminal session to xterm.js.
/// </summary>
void PushCliOutput(CefRefPtr<CefBrowser> browser,
                   const std::string&    frontend_chat_id,
                   const std::string&    source_chat_id,
                   const std::string&    terminal_id,
                   const std::string&    raw_bytes);

} // namespace uam

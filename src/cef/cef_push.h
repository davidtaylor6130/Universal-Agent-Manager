#pragma once

#include "cef/cef_includes.h"
#include "cef/state_serializer.h"
#include "common/state/app_state.h"
#include "common/platform/platform_services.h"

#include <chrono>
#include <string>

namespace uam
{

	/// <summary>
	/// Serialises AppState summaries, with transcripts loaded separately, and calls window.uamPush({type:"stateUpdate",...}) in the
	/// browser's main frame.  Safe to call from any thread — marshals to the UI thread internally.
	/// </summary>
	bool PushStateUpdateIfChanged(CefRefPtr<CefBrowser> browser, AppState& app);
	void PushStateUpdate(CefRefPtr<CefBrowser> browser, AppState& app);

	/// <summary>
	/// True when a throttled chat summary is still waiting to be flushed by a
	/// later PushStateUpdateIfChanged call.
	/// </summary>
	bool HasDeferredStatePush();
	/// <summary>
	/// True when a deferred summary's throttle window has elapsed, so a tick with
	/// no other state change should rebuild the fingerprint and flush it.
	/// </summary>
	bool DeferredStatePushDue();
	/// <summary>
	/// Milliseconds (at least 1) until the pending deferred summary is due; the poll
	/// scheduler caps its next delay with this so slow idle ticks never hold it back.
	/// </summary>
	int MillisecondsUntilDeferredStatePushDue();
	/// <summary>Replaces the summary-throttle clock; nullptr restores steady_clock::now.</summary>
	void SetStatePushClockForTests(std::chrono::steady_clock::time_point (*clock)());
	inline std::string StateFingerprintForTests(const AppState& app)
	{
		return StateSerializer::SerializeFingerprint(app).dump();
	}
	std::string StateUpdateForTests(const AppState& app);
	std::string StatePatchForTests(const AppState& before, const AppState& after);
	std::string SettingsPatchForTests(const AppState& app);

	/// <summary>
	/// Delivers a streaming token with its native message position to the React frontend.
	/// </summary>
	void PushStreamToken(CefRefPtr<CefBrowser> browser, const std::string& chat_id, int message_index, const std::string& token);

	/// <summary>
	/// Signals that the streaming response for chat_id has completed.
	/// </summary>
	void PushStreamDone(CefRefPtr<CefBrowser> browser, const std::string& chat_id);

	/// <summary>
	/// Forwards raw PTY bytes (base64-encoded) from a CLI terminal session to xterm.js.
	/// </summary>
	void PushCliOutput(CefRefPtr<CefBrowser> browser, const std::string& frontend_chat_id, const std::string& source_chat_id, const std::string& terminal_id, double startup_time_s, const std::string& raw_bytes);

	/// <summary>
	/// Delivers one native speech-recognition event to the prompt composer.
	/// </summary>
	void PushDictationEvent(CefRefPtr<CefBrowser> browser, const DictationEvent& event);

} // namespace uam

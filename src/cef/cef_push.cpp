#include "cef/cef_push.h"
#include "common/secrets/secret_redactor.h"
#include "cef/state_serializer.h"

#include "app/chat_domain_service.h"

#include "common/config/custom_icon.h"
#include "common/config/settings_frontend_json.h"
#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/wrapper/cef_helpers.h"

#include "common/utils/base64.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
	constexpr const char* kPushTypeAttachmentProgress = "attachmentProgress";
	constexpr const char* kPushTypeCompanionConfirmations = "companionConfirmations";
	constexpr long long kStatePatchSlowSerializationMs = 50;
	constexpr std::size_t kStatePatchLargeMessageBytes = 64 * 1024;
	// Streaming chat summaries (selected or background) carry the live turn
	// timeline (turnEvents), which grows on nearly every tick during a turn;
	// 10 updates per chat per second is plenty for streamed text and tool-call
	// progress. Summaries that introduce a pending permission or user-input
	// request, or a new CLI launch, bypass the throttle so prompts are never delayed.
	constexpr std::chrono::milliseconds kChatSummaryMinPushInterval{100};

	std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_last_summary_push_time_by_chat_id;
	bool g_state_push_deferred = false;
	// Earliest moment a deferred summary leaves its throttle window; quiet ticks skip the fingerprint until then.
	std::chrono::steady_clock::time_point g_deferred_state_push_due_at{};
	// Throttle decisions read this clock so unit tests can step time deterministically.
	std::chrono::steady_clock::time_point (*g_state_push_clock)() = &std::chrono::steady_clock::now;
	// Poll ticks run every 16 ms while a turn streams; rebuilding the full fingerprint on each one is wasted work
	// because throttled summaries publish at most every 100 ms.
	std::chrono::steady_clock::time_point g_last_runtime_state_rebuild_at{};
	// Pending requests and CLI launch identities seen by the last poll rebuild; a change bypasses the window.
	std::string g_last_runtime_urgent_signature;
	std::size_t g_runtime_state_rebuild_count = 0;
	std::unordered_map<std::string, nlohmann::json> g_last_pushed_chat_summaries_by_chat_id;
	nlohmann::json g_last_pushed_folders_fingerprint;
	nlohmann::json g_last_pushed_resource_collections_fingerprint;
	nlohmann::json g_last_pushed_providers_fingerprint;
	nlohmann::json g_last_pushed_provider_model_catalogs_fingerprint;
	nlohmann::json g_last_pushed_startup_native_imports_fingerprint;
	nlohmann::json g_last_pushed_settings_fingerprint;
	nlohmann::json g_last_pushed_memory_fingerprint;
	nlohmann::json g_last_pushed_cli_version_manager_fingerprint;
	nlohmann::json g_last_pushed_shell_actions_fingerprint;
	nlohmann::json g_last_pushed_compute_pool_usage_fingerprint;
	nlohmann::json g_last_pushed_pr_watches_fingerprint;
	std::string g_last_pushed_shell_action_notification;
	std::string g_last_pushed_status_line;
	std::string g_last_pushed_selected_chat_id;
	nlohmann::json g_last_pushed_chat_order_fingerprint;
	// Digest of each chat's compact summary at its last push; a poll tick builds JSON only for chats whose digest moved.
	std::unordered_map<std::string, std::uint64_t> g_last_pushed_chat_digests_by_chat_id;
	// Pushes reuse values by revision stamp (see StateSerializer::StampedCacheMode). A push that starts at least
	// this long after the last verifying one recomputes them instead, so a write that bypassed a stamp reaches the
	// frontend with a [desync] log line instead of never: the poll loop wakes when the interval ends, and the
	// reconciling push can then wait out one 100 ms rebuild window, so about 3.1 s after the push that missed it.
	constexpr std::chrono::seconds kStateReconciliationInterval{3};
	std::chrono::steady_clock::time_point g_last_state_reconciliation_at{};
	// A push since then trusted a stamp; the poll loop runs one more push when the interval ends even if nothing reports a change.
	bool g_state_reconciliation_pending = false;
	// Tests: after every push, recompute everything the stamps vouch for and count differences.
	bool g_verify_state_caches_after_every_push = false;

	/// <summary>A clock that moved backwards (stepped test clocks) counts as elapsed, so verification is never postponed by it.</summary>
	bool ReconciliationIntervalElapsed(std::chrono::steady_clock::time_point now)
	{
		return now < g_last_state_reconciliation_at || now - g_last_state_reconciliation_at >= kStateReconciliationInterval;
	}

	/// <summary>Shows in the log whether non-const reads are defeating the transcript stamps, at most once a minute.</summary>
	void ReportRestampedTranscripts()
	{
		static std::size_t reported = 0;
		static auto last_report = std::chrono::steady_clock::time_point::min();
		const std::size_t transcripts = uam::StateSerializer::RestampedUnchangedTranscriptCount();
		const std::size_t tool_calls = uam::StateSerializer::RestampedUnchangedToolCallCount();
		const std::size_t total = transcripts + tool_calls;
		const auto now = std::chrono::steady_clock::now();
		if (total == reported || (last_report != std::chrono::steady_clock::time_point::min() && now - last_report < std::chrono::seconds(60))) return;
		uam::diagnostics::Write("[performance] State pushes recomputed " + std::to_string(total - reported) +
		    " stamped value(s) whose content had not changed since the last report: a non-const read restamped them. Totals: loaded transcripts rehashed " +
		    std::to_string(transcripts) + ", live tool call lists rebuilt " + std::to_string(tool_calls) + ".");
		reported = total;
		last_report = now;
	}

	/// <summary>Chooses whether this push verifies or trusts stamped values, and records what it did.</summary>
	class ReconciledPush
	{
	  public:
		ReconciledPush()
		    : m_now(g_state_push_clock()), m_verify(ReconciliationIntervalElapsed(m_now)),
		      m_mode(m_verify ? uam::StateSerializer::StampedCacheMode::Verify : uam::StateSerializer::StampedCacheMode::Trust),
		      m_trusted_before(uam::StateSerializer::TrustedCacheReadCount())
		{
		}
		~ReconciledPush()
		{
			if (m_verify)
			{
				g_last_state_reconciliation_at = m_now;
				g_state_reconciliation_pending = false;
				uam::StateSerializer::FlushCacheDivergenceReport();
			}
			else if (uam::StateSerializer::TrustedCacheReadCount() != m_trusted_before) g_state_reconciliation_pending = true;
			ReportRestampedTranscripts();
		}

	  private:
		std::chrono::steady_clock::time_point m_now;
		bool m_verify;
		uam::StateSerializer::ScopedStampedCacheMode m_mode;
		std::size_t m_trusted_before;
	};

	void VerifyStateCachesForTests(const uam::AppState& app)
	{
		if (!g_verify_state_caches_after_every_push) return;
		const uam::StateSerializer::ScopedStampedCacheMode verification(uam::StateSerializer::StampedCacheMode::Verify);
		const uam::StateSerializer::StablePass stable_pass;
		uam::StateSerializer::CaptureCatalogInputs(app);
		(void)uam::StateSerializer::SerializeFingerprint(app, true, false);
		(void)uam::StateSerializer::SerializeChatFingerprintDigests(app);
	}

	// Tests observe every built statePatch message; nullptr in production.
	void (*g_state_patch_observer)(const std::string& message) = nullptr;

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

		// Last stop before the page: any registered secret still in the payload is replaced.
		std::string scrubbed;
		if (uam::secrets::Active())
		{
			scrubbed = json;
			// A raw match can sit inside a number or eat an escape; then scrub the string values and serialize again.
			if (uam::secrets::ScrubInPlace(scrubbed) && !nlohmann::json::accept(scrubbed))
			{
				nlohmann::json value = nlohmann::json::parse(json, nullptr, false);
				(void)uam::secrets::ScrubJsonStrings(value);
				scrubbed = value.is_discarded() ? std::string("null") : DumpFrontendJson(value);
			}
		}
		const std::string js = "window.uamPush && window.uamPush(" + (scrubbed.empty() ? json : scrubbed) + ");";
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
		nlohmann::json settings = uam::settings_frontend_json::SerializeLiveSettingsFields(app.settings, app.memory_last_status);
		settings["computerUseRevocationPending"] = !app.computer_use_revocation_pending_chat_ids.empty();
		return settings;
	}

	void StripVolatileAcpWaitTelemetry(nlohmann::json& state);
	nlohmann::json ChatOrderForPatch(const uam::AppState& app);

	void ResetPatchBaselines(const uam::AppState& app, nlohmann::json fingerprint_state)
	{
		StripVolatileAcpWaitTelemetry(fingerprint_state);
		g_last_pushed_folders_fingerprint = fingerprint_state.at("folders");
		g_last_pushed_resource_collections_fingerprint = fingerprint_state.at("resourceCollections");
		g_last_pushed_providers_fingerprint = fingerprint_state.at("providers");
		g_last_pushed_provider_model_catalogs_fingerprint = fingerprint_state.at("providerModelCatalogs");
		// The serializer may omit the slice only relative to a baseline it saw compared; this one came from elsewhere.
		uam::StateSerializer::ForgetCatalogMemo();
		g_last_pushed_startup_native_imports_fingerprint = fingerprint_state.at("startupNativeImports");
		g_last_pushed_settings_fingerprint = SerializeSettingsForPatch(app);
		g_last_pushed_memory_fingerprint = fingerprint_state.at("memoryActivity");
		g_last_pushed_cli_version_manager_fingerprint = fingerprint_state.at("cliVersionManager");
		g_last_pushed_shell_actions_fingerprint = fingerprint_state.at("shellActions");
		g_last_pushed_compute_pool_usage_fingerprint = fingerprint_state.at("computePoolUsage");
		g_last_pushed_pr_watches_fingerprint = fingerprint_state.at("prWatches");
		g_last_pushed_shell_action_notification = uam::nlohmann_json::TrimmedStringValue(fingerprint_state, {"shellActionNotification"});
		g_last_pushed_status_line = fingerprint_state.value("statusLine", std::string{});
		g_last_pushed_selected_chat_id = ChatDomainService().SelectedChatId(app);
		g_last_pushed_chat_order_fingerprint = ChatOrderForPatch(app);
		g_last_pushed_chat_summaries_by_chat_id.clear();
		g_last_summary_push_time_by_chat_id.clear();
		g_last_runtime_state_rebuild_at = {};
		g_last_runtime_urgent_signature.clear();
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

		// The pushed state covers every compact summary field, so its digests are the next tick's baseline.
		g_last_pushed_chat_digests_by_chat_id.clear();
		for (const uam::StateSerializer::ChatSummaryDigest& entry : uam::StateSerializer::SerializeChatFingerprintDigests(app))
		{
			const std::string chat_id = uam::strings::Trim(entry.chat->id);
			if (!chat_id.empty()) g_last_pushed_chat_digests_by_chat_id[chat_id] = entry.digest;
		}
	}

	nlohmann::json BuildFullStateUpdateMessage(const uam::AppState& app)
	{
		nlohmann::json msg = PushMessage(kPushTypeStateUpdate);
		msg["data"] = uam::StateSerializer::Serialize(app, true);
		return msg;
	}

	/// <summary>Reports slow patches and large patches in every build, each at most once per five seconds.</summary>
	void MaybeLogLargePatch(const std::string& message, int changed_chat_count, std::chrono::steady_clock::duration elapsed)
	{
		const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
		const auto now = std::chrono::steady_clock::now();
		const auto due = [now](std::chrono::steady_clock::time_point& last_report)
		{
			if (last_report != std::chrono::steady_clock::time_point::min() && now - last_report < std::chrono::seconds(5)) return false;
			last_report = now;
			return true;
		};
		const std::string detail = "bytes=" + std::to_string(message.size()) + ", changedChats=" + std::to_string(changed_chat_count);
		// Separate limits: a large but fast patch must not hide a slow one, nor be reported as slow.
		static auto last_slow_report = std::chrono::steady_clock::time_point::min();
		static auto last_large_report = std::chrono::steady_clock::time_point::min();
		// A patch that is both slow and large falls through to the large line while the slow line is rate-limited.
		if (elapsed_ms > kStatePatchSlowSerializationMs && due(last_slow_report))
			uam::diagnostics::Write("[performance] statePatch serialization took " + std::to_string(elapsed_ms) + " ms, " + detail);
		else if (message.size() > kStatePatchLargeMessageBytes && due(last_large_report))
			uam::diagnostics::Write("[performance] statePatch is large: " + detail + ", built in " + std::to_string(elapsed_ms) + " ms");
	}

	// Structural comparison: dumping these slices (folder icons carry PNG data URLs) cost milliseconds per tick.
	// Unlike dumped text, 1 and 1.0 compare equal and a NaN never does; these slices carry neither in practice.
	void AddChangedJsonField(nlohmann::json& patch_data, const char* field_name, const nlohmann::json& value, nlohmann::json& last_value)
	{
		if (value == last_value)
		{
			return;
		}

		patch_data[field_name] = value;
		last_value = value;
	}

	struct ChatPatchDiff
	{
		nlohmann::json changed_chats = nlohmann::json::array();
		std::vector<std::uint64_t> changed_digests;
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

	/// <summary>Publish launch ownership before output, independently of transcript throttling.</summary>
	bool CliLaunchIdentityChanged(const nlohmann::json& previous, const nlohmann::json& current)
	{
		const nlohmann::json::const_iterator before = previous.find("cliTerminal");
		const nlohmann::json::const_iterator after = current.find("cliTerminal");
		if (before == previous.end() || after == current.end())
			return before != previous.end() || after != current.end();
		if (!before->is_object() || !after->is_object()) return *before != *after;
		return before->value("terminalId", nlohmann::json()) != after->value("terminalId", nlohmann::json()) ||
		    before->value("startupTime", nlohmann::json()) != after->value("startupTime", nlohmann::json());
	}

	ChatPatchDiff BuildChatPatchDiff(const uam::AppState& app)
	{
		ChatPatchDiff diff;
		g_state_push_deferred = false;
		std::unordered_set<std::string> current_chat_ids;

		// Digests come from the same field code as the summary JSON, so only chats whose digest moved are built.
		std::vector<uam::StateSerializer::ChatSummaryDigest> changed;
		std::vector<const ChatSession*> unbuilt;
		for (uam::StateSerializer::ChatSummaryDigest& entry : uam::StateSerializer::SerializeChatFingerprintDigests(app))
		{
			const std::string chat_id = uam::strings::Trim(entry.chat->id);
			if (chat_id.empty()) continue;
			current_chat_ids.insert(chat_id);
			const auto previous_digest = g_last_pushed_chat_digests_by_chat_id.find(chat_id);
			if (previous_digest != g_last_pushed_chat_digests_by_chat_id.end() && previous_digest->second == entry.digest) continue;
			if (entry.summary.is_null()) unbuilt.push_back(entry.chat);
			changed.push_back(std::move(entry));
		}
		std::vector<nlohmann::json> built = uam::StateSerializer::SerializeChatFingerprints(app, unbuilt);
		nlohmann::json changed_state = {{"chats", nlohmann::json::array()}};
		for (std::size_t index = 0, next_built = 0; index < changed.size(); ++index)
			changed_state["chats"].push_back(changed[index].summary.is_null() ? std::move(built[next_built++]) : std::move(changed[index].summary));
		StripVolatileAcpWaitTelemetry(changed_state);
		std::size_t changed_index = 0;
		for (nlohmann::json& chat : changed_state.at("chats"))
		{
			const std::uint64_t digest = changed[changed_index++].digest;
			const std::string chat_id = uam::nlohmann_json::TrimmedStringValue(chat, {"id"});
			const auto previous_chat_it = g_last_pushed_chat_summaries_by_chat_id.find(chat_id);
			if (previous_chat_it != g_last_pushed_chat_summaries_by_chat_id.end() && previous_chat_it->second == chat)
			{
				// Same JSON under a new digest (for example 1 and 1.0): nothing to publish, so only the digest moves.
				g_last_pushed_chat_digests_by_chat_id[chat_id] = digest;
				continue;
			}
			{
				const auto now = g_state_push_clock();
				const auto last_summary_push_it = g_last_summary_push_time_by_chat_id.find(chat_id);
				const bool launch_changed = previous_chat_it == g_last_pushed_chat_summaries_by_chat_id.end() ||
				    CliLaunchIdentityChanged(previous_chat_it->second, chat);
				const bool summary_throttled = !launch_changed &&
				                               !ChatSummaryHasPendingInteraction(chat) &&
				                               last_summary_push_it != g_last_summary_push_time_by_chat_id.end() &&
				                               now - last_summary_push_it->second < kChatSummaryMinPushInterval;
				if (summary_throttled)
				{
					// Keep the previous summary baseline so the deferred change is
					// retried on a later tick instead of silently dropped.
					const auto due = last_summary_push_it->second + kChatSummaryMinPushInterval;
					if (!g_state_push_deferred || due < g_deferred_state_push_due_at) g_deferred_state_push_due_at = due;
					g_state_push_deferred = true;
				}
				else
				{
					diff.changed_chats.push_back(chat);
					diff.changed_digests.push_back(digest);
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
			for (std::size_t index = 0; index < diff.changed_chats.size(); ++index)
			{
				const nlohmann::json& chat = diff.changed_chats[index];
				const std::string chat_id = uam::nlohmann_json::TrimmedStringValue(chat, {"id"});
				g_last_pushed_chat_summaries_by_chat_id[chat_id] = chat;
				g_last_pushed_chat_digests_by_chat_id[chat_id] = diff.changed_digests[index];
			}
			data["chats"] = std::move(diff.changed_chats);
		}
		if (!diff.removed_chat_ids.empty())
		{
			for (const nlohmann::json& chat_id : diff.removed_chat_ids)
			{
				g_last_pushed_chat_summaries_by_chat_id.erase(chat_id.get<std::string>());
				g_last_pushed_chat_digests_by_chat_id.erase(chat_id.get<std::string>());
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

		// Absent when the serializer vouches that the catalogue inputs are unchanged since the slice was last compared here.
		if (const nlohmann::json::const_iterator catalogs = fingerprint_state.find("providerModelCatalogs"); catalogs != fingerprint_state.end())
			AddChangedJsonField(data, "providerModelCatalogs", *catalogs, g_last_pushed_provider_model_catalogs_fingerprint);
		AddChangedJsonField(data, "startupNativeImports", fingerprint_state.at("startupNativeImports"), g_last_pushed_startup_native_imports_fingerprint);

		const nlohmann::json settings = SerializeSettingsForPatch(app);
		AddChangedJsonField(data, "settings", settings, g_last_pushed_settings_fingerprint);

		AddChangedJsonField(data, "memoryActivity", fingerprint_state.at("memoryActivity"), g_last_pushed_memory_fingerprint);

		AddChangedJsonField(data, "cliVersionManager", fingerprint_state.at("cliVersionManager"), g_last_pushed_cli_version_manager_fingerprint);

		AddChangedJsonField(data, "shellActions", fingerprint_state.at("shellActions"), g_last_pushed_shell_actions_fingerprint);
		AddChangedJsonField(data, "computePoolUsage", fingerprint_state.at("computePoolUsage"), g_last_pushed_compute_pool_usage_fingerprint);
		AddChangedJsonField(data, "prWatches", fingerprint_state.at("prWatches"), g_last_pushed_pr_watches_fingerprint);

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

		ApplyChatPatchDiff(data, BuildChatPatchDiff(app));
		if (nlohmann::json::iterator chats = data.find("chats"); chats != data.end())
		{
			for (nlohmann::json& chat : *chats)
			{
				const ChatSession* source = ChatDomainService().FindChatById(app, chat.value("id", ""));
				if (source == nullptr) continue;
				const nlohmann::json::const_iterator acp = chat.find("acpSession");
				if (acp != chat.end() && acp->contains("catalogDigest"))
				{
					// Hydration rebuilds the session summary only; keep the scan's managed subagent count.
					const nlohmann::json managed_subagent_count = chat.value("managedSubagentCount", nlohmann::json(0));
					chat = uam::StateSerializer::SerializeChatSummary(app, *source);
					chat["managedSubagentCount"] = managed_subagent_count;
				}
				// The frontend replaces the whole chat, so a patched PNG icon needs its data URL as in stateUpdate.
				uam::icons::AddFrontendAsset(chat, app.data_root, source->custom_icon);
			}
		}
		AddChangedJsonField(data, "chatOrder", ChatOrderForPatch(app), g_last_pushed_chat_order_fingerprint);
		if (has_payload != nullptr)
		{
			*has_payload = data.size() > 1;
		}

		nlohmann::json msg = PushMessage(kPushTypeStatePatch);
		msg["data"] = std::move(data);
		const std::string message = DumpFrontendJson(msg);
		MaybeLogLargePatch(message, static_cast<int>(uam::nlohmann_json::ArrayFieldOrEmpty(msg["data"], "chats").size()), std::chrono::steady_clock::now() - started);
		if (g_state_patch_observer != nullptr) g_state_patch_observer(message);
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

	std::string StateUpdateForTests(const AppState& app)
	{
		return DumpFrontendJson(BuildFullStateUpdateMessage(app));
	}

	std::string SettingsPatchForTests(const AppState& app)
	{
		return DumpFrontendJson(SerializeSettingsForPatch(app));
	}

	std::string StatePatchForTests(const AppState& before, const AppState& after)
	{
		ResetPatchBaselines(before, StateSerializer::SerializeFingerprint(before, true));
		nlohmann::json fingerprint_state = StateSerializer::SerializeFingerprint(after, true, false);
		StripVolatileAcpWaitTelemetry(fingerprint_state);
		return BuildStatePatchMessage(after, fingerprint_state);
	}

	void SetStatePatchObserverForTests(void (*observer)(const std::string& message))
	{
		g_state_patch_observer = observer;
	}

	bool HasDeferredStatePush()
	{
		return g_state_push_deferred;
	}

	int MillisecondsUntilDeferredStatePushDue()
	{
		const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(g_deferred_state_push_due_at - g_state_push_clock()).count();
		return remaining < 1 ? 1 : static_cast<int>(remaining);
	}

	int CapPollDelayForDeferredStatePush(int delay_ms)
	{
		if (HasDeferredStatePush()) delay_ms = std::min(delay_ms, MillisecondsUntilDeferredStatePushDue());
		// The reconciliation bound holds only if the poll loop wakes for it instead of on its idle interval.
		if (g_state_reconciliation_pending)
		{
			const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(g_last_state_reconciliation_at + kStateReconciliationInterval - g_state_push_clock()).count();
			delay_ms = std::min(delay_ms, remaining < 1 ? 1 : static_cast<int>(std::min<long long>(remaining, kStateReconciliationInterval / std::chrono::milliseconds(1))));
		}
		return delay_ms;
	}

	void SetStatePushClockForTests(std::chrono::steady_clock::time_point (*clock)())
	{
		g_state_push_clock = clock != nullptr ? clock : &std::chrono::steady_clock::now;
	}

	bool DeferredStatePushDue()
	{
		return g_state_push_deferred && g_state_push_clock() >= g_deferred_state_push_due_at;
	}

	bool StateReconciliationDue()
	{
		return g_state_reconciliation_pending && ReconciliationIntervalElapsed(g_state_push_clock());
	}

	void SetStateCacheVerificationAfterEveryPushForTests(bool verify)
	{
		g_verify_state_caches_after_every_push = verify;
	}

	static bool PushStateUpdateIfChangedOnce(CefRefPtr<CefBrowser> browser, AppState& app);

	bool PushStateUpdateIfChanged(CefRefPtr<CefBrowser> browser, AppState& app)
	{
		const bool pushed = PushStateUpdateIfChangedOnce(browser, app);
		VerifyStateCachesForTests(app);
		return pushed;
	}

	static bool PushStateUpdateIfChangedOnce(CefRefPtr<CefBrowser> browser, AppState& app)
	{
		const ReconciledPush reconciled_push;
		// The slices, the digest scan and the summaries built for changed chats read the same unchanged state.
		const uam::StateSerializer::StablePass stable_pass;
		uam::StateSerializer::CaptureCatalogInputs(app);
		const auto fingerprint_started = std::chrono::steady_clock::now();
		nlohmann::json fingerprint_state = uam::StateSerializer::SerializeFingerprint(app, true, false);
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

	/// <summary>Cheap key of the summary classes that bypass the 100 ms window inside BuildChatPatchDiff.</summary>
	std::string RuntimeUrgentSignature(const AppState& app)
	{
		std::string signature;
		for (const auto& session : app.acp_sessions)
		{
			if (session == nullptr) continue;
			signature += session->pending_permission.request_id_json;
			signature += '|';
			signature += session->pending_user_input.request_id_json;
			signature += ';';
		}
		for (const UamControlCapability& capability : app.uam_control_capabilities)
		{
			if (capability.pending_approval.has_value()) signature += "a" + capability.pending_approval->request_id + ';';
		}
		for (const auto& terminal : app.cli_terminals)
		{
			if (terminal == nullptr) continue;
			signature += terminal->terminal_id;
			signature += '@';
			signature += std::to_string(terminal->startup_time_s);
			signature += ';';
		}
		return signature;
	}

	bool PushRuntimeStateUpdateIfChanged(CefRefPtr<CefBrowser> browser, AppState& app, bool urgent)
	{
		const auto now = g_state_push_clock();
		std::string urgent_signature = RuntimeUrgentSignature(app);
		if (urgent_signature != g_last_runtime_urgent_signature) urgent = true;
		if (!urgent && g_last_runtime_state_rebuild_at != std::chrono::steady_clock::time_point{} &&
		    now - g_last_runtime_state_rebuild_at < kChatSummaryMinPushInterval)
		{
			// Defer instead of dropping: the poll scheduler wakes when the window ends.
			// A past-due deadline cannot publish before the window ends, so it moves out too; left in the past it makes the scheduler poll every 1 ms.
			const auto due = g_last_runtime_state_rebuild_at + kChatSummaryMinPushInterval;
			if (!g_state_push_deferred || due < g_deferred_state_push_due_at || g_deferred_state_push_due_at <= now) g_deferred_state_push_due_at = due;
			g_state_push_deferred = true;
			return false;
		}
		g_last_runtime_state_rebuild_at = now;
		g_last_runtime_urgent_signature = std::move(urgent_signature);
		++g_runtime_state_rebuild_count;
		return PushStateUpdateIfChanged(browser, app);
	}

	std::size_t RuntimeStateRebuildCountForTests() { return g_runtime_state_rebuild_count; }

	void PushStateUpdate(CefRefPtr<CefBrowser> browser, AppState& app)
	{
		BumpStateRevision(app);
		{
			const ReconciledPush reconciled_push;
			// The full state and the digest baselines taken from it read the same unchanged state.
			const uam::StateSerializer::StablePass stable_pass;
			uam::StateSerializer::CaptureCatalogInputs(app);
			nlohmann::json message = BuildFullStateUpdateMessage(app);
			const std::string serialized_message = DumpFrontendJson(message);
			// Reuse this push snapshot; strip baseline telemetry only after serializing its wire payload.
			ResetPatchBaselines(app, std::move(message["data"]));
			PostPush(browser, serialized_message);
		}
		VerifyStateCachesForTests(app);
	}

	namespace
	{
		/// Streamed tokens can split a secret; each stream holds back a possible secret prefix until it resolves.
		struct SecretStream
		{
			uam::secrets::StreamFilter filter;
			int message_index = 0;
		};
		std::mutex g_secret_streams_mutex;
		std::unordered_map<std::string, SecretStream> g_secret_streams;

		void PostStreamToken(CefRefPtr<CefBrowser> browser, const std::string& chat_id, int message_index, const std::string& token)
		{
			nlohmann::json msg = PushMessage(kPushTypeStreamToken);
			msg["chatId"] = chat_id;
			msg["token"] = token;
			msg["messageIndex"] = message_index;
			PostPush(browser, DumpFrontendJson(msg));
		}
	}

	void PushStreamToken(CefRefPtr<CefBrowser> browser, const std::string& chat_id, int message_index, const std::string& token)
	{
		if (!uam::secrets::Active())
		{
			PostStreamToken(browser, chat_id, message_index, token);
			return;
		}
		std::string safe;
		std::string previous;
		int previous_index = 0;
		{
			std::lock_guard lock(g_secret_streams_mutex);
			SecretStream& stream = g_secret_streams[chat_id];
			if (stream.message_index != message_index)
			{
				previous = stream.filter.Flush();
				previous_index = stream.message_index;
			}
			stream.message_index = message_index;
			safe = stream.filter.Push(token);
		}
		if (!previous.empty()) PostStreamToken(browser, chat_id, previous_index, previous);
		if (!safe.empty()) PostStreamToken(browser, chat_id, message_index, safe);
	}

	void PushStreamDone(CefRefPtr<CefBrowser> browser, const std::string& chat_id)
	{
		std::optional<SecretStream> held;
		{
			std::lock_guard lock(g_secret_streams_mutex);
			if (const auto found = g_secret_streams.find(chat_id); found != g_secret_streams.end())
			{
				held = std::move(found->second);
				g_secret_streams.erase(found);
			}
		}
		if (held.has_value())
			if (const std::string rest = held->filter.Flush(); !rest.empty()) PostStreamToken(browser, chat_id, held->message_index, rest);
		nlohmann::json msg = PushMessage(kPushTypeStreamDone);
		msg["chatId"] = chat_id;
		PostPush(browser, DumpFrontendJson(msg));
	}

	void PushCliOutput(CefRefPtr<CefBrowser> browser, const std::string& frontend_chat_id, const std::string& source_chat_id, const std::string& terminal_id, double startup_time_s, const std::string& raw_bytes)
	{
		nlohmann::json msg = PushMessage(kPushTypeCliOutput);
		msg["sessionId"] = frontend_chat_id;
		msg["sourceChatId"] = source_chat_id;
		msg["terminalId"] = terminal_id;
		msg["startupTime"] = startup_time_s;
		msg["data"] = uam::base64::Encode(raw_bytes);
		PostPush(browser, DumpFrontendJson(msg));
	}

	void PushAttachmentProgress(CefRefPtr<CefBrowser> browser, const std::string& chat_id, const std::string& attachment_id, const std::uintmax_t copied_bytes, const std::uintmax_t total_bytes)
	{
		nlohmann::json msg = PushMessage(kPushTypeAttachmentProgress);
		msg["chatId"] = chat_id;
		msg["attachmentId"] = attachment_id;
		msg["copiedBytes"] = copied_bytes;
		msg["totalBytes"] = total_bytes;
		PostPush(browser, DumpFrontendJson(msg));
	}

	void PushCompanionConfirmations(CefRefPtr<CefBrowser> browser, const nlohmann::json& items)
	{
		nlohmann::json msg = PushMessage(kPushTypeCompanionConfirmations);
		msg["items"] = items;
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

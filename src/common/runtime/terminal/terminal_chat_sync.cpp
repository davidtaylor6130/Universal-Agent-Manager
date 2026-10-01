#include "common/runtime/terminal/terminal_chat_sync.h"

#include "app/chat_domain_service.h"
#include "app/native_session_link_service.h"
#include "app/provider_resolution_service.h"
#include "common/paths/workspace_root.h"
#include "common/runtime/app_time.h"
#include "common/runtime/terminal_polling.h"
#include "common/utils/time_utils.h"
#include "app/runtime_orchestration_internal.h"
#include "app/runtime_orchestration_services.h"
#include "common/chat/chat_repository.h"
#include "common/provider/provider_runtime.h"
#include "common/config/execution_host_config.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/terminal/terminal_debug_diagnostics.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_native_identity.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/platform/platform_services.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/runtime/terminal/terminal_lifecycle_states.h"
#include "common/state/app_state.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace uam
{
namespace
{
	constexpr std::string_view kNativeRefreshFailurePrefix = "Native chat refresh failed: ";

	void UpdateNativeChatRefreshError(AppState& app, std::string_view target_id, std::string_view error)
	{
		if (!error.empty())
		{
			app.native_chat_refresh_error_target_id = target_id;
			app.native_chat_refresh_error_status = std::string(kNativeRefreshFailurePrefix) + std::string(error);
			app.status_line = app.native_chat_refresh_error_status;
		}
		else if (app.native_chat_refresh_error_target_id == target_id)
		{
			if (app.status_line == app.native_chat_refresh_error_status) app.status_line.clear();
			app.native_chat_refresh_error_target_id.clear();
			app.native_chat_refresh_error_status.clear();
		}
	}
}

bool HasCompetingUnboundCliSession(const AppState& app, const CliTerminalState& terminal)
{
	const ChatSession* chat = FindChatForCliTerminal(app, terminal);
	if (chat == nullptr || !uam::paths::IsControllerLocalWorkspace(*chat)) return false;
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, *chat);
	const std::filesystem::path workspace = uam::paths::ResolveControllerWorkspaceRootPath(app, *chat);
	for (const std::unique_ptr<CliTerminalState>& other : app.cli_terminals)
	{
		if (other == nullptr || other.get() == &terminal || !other->running || !CliTerminalAttachedSessionId(*other).empty()) continue;
		const ChatSession* other_chat = FindChatForCliTerminal(app, *other);
		if (other_chat == nullptr || other_chat->id == chat->id || !uam::paths::IsControllerLocalWorkspace(*other_chat)) continue;
		const ProviderProfile& other_provider = ProviderResolutionService().ProviderForChatOrDefault(app, *other_chat);
		if (other_provider.id != provider.id) continue;
		const std::filesystem::path other_workspace = uam::paths::ResolveControllerWorkspaceRootPath(app, *other_chat);
		std::error_code path_error;
		if (other_workspace != workspace && !std::filesystem::equivalent(workspace, other_workspace, path_error)) continue;
		if (ProviderRuntimeRegistry::Resolve(other_provider).ResolveInteractiveResumeId(app, *other_chat).empty()) return true;
	}
	return false;
}

bool DiscoverCliTerminalNativeSession(AppState& app, CliTerminalState& terminal)
{
	ChatSession* chat = FindChatForCliTerminal(app, terminal);
	if (chat == nullptr) return false;
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, *chat);
	std::string identity = CliTerminalAttachedSessionId(terminal);
	if (identity.empty()) identity = ProviderRuntimeRegistry::Resolve(provider).ResolveInteractiveResumeId(app, *chat);
	terminal.native_session_discovery_ambiguous = false;
	if (identity.empty() && (terminal.native_identity_requires_owned_reply || HasCompetingUnboundCliSession(app, terminal)))
	{
		terminal.native_session_discovery_ambiguous = true;
		return false;
	}
	if (identity.empty() && chat->execution_host_id == uam::execution_hosts::kLocalHostId)
	{
		if (ProviderResolutionService().ChatUsesNativeOverlayHistory(app, *chat))
		{
			const std::filesystem::path directory = ChatHistorySyncService().ResolveNativeHistoryChatsDirForChat(app, *chat);
			TryAttachNativeSessionFromHistory(app, terminal, ChatHistorySyncService().LoadNativeSessionChats(directory, provider));
			identity = CliTerminalAttachedSessionId(terminal);
		}
		else identity = ProviderRuntimeRegistry::Resolve(provider).DiscoverInteractiveSessionId(
		    terminal.session_ids_before, uam::paths::ResolveControllerWorkspaceRootPath(app, *chat), &terminal.native_session_discovery_ambiguous);
	}
	if (identity.empty()) return false;
	terminal.attached_session_id = identity;
	if (chat->native_session_id == identity) return false;
	chat->native_session_id = identity;
	chat->updated_at = uam::time::TimestampNow();
	terminal.attached_session_id = identity;
	app.resolved_native_sessions_by_chat_id[chat->id] = identity;
	if (!ProviderRuntime::SaveHistory(provider, app.data_root, *chat))
	{
		app.pending_chat_save_at_by_chat_id[chat->id] = GetAppTimeSeconds() + 1.0;
		LogCliDiagnosticEvent(app, "native_session_link", "save_failed", &terminal, "Could not save the native session link; retrying.");
	}
	return true;
}

/// <summary>Native slash commands are separated from Enter so Codex cannot classify them as a paste.</summary>
bool PollCliNativeIdentityQuery(AppState& app, CliTerminalState& terminal, std::string_view provider_id, std::string_view output, double now_s)
{
	if (terminal.native_identity_query_phase == 0 && terminal.native_identity_requires_owned_reply &&
	    terminal.native_identity_command.empty() && terminal.last_user_input_time_s == 0.0 &&
	    now_s - terminal.startup_time_s <= 30.0)
	{
		const ChatSession* chat = FindChatForCliTerminal(app, terminal);
		if (chat != nullptr)
		{
			const std::string key = CliProviderVersionStateKey(provider_id, chat->execution_host_id);
			const std::unordered_map<std::string, CliProviderVersionState>::const_iterator version = app.runtime_cli_versions_by_provider_id.find(key);
			if (version != app.runtime_cli_versions_by_provider_id.end() && version->second.checked)
			{
				terminal.native_identity_command = NativeSessionStatusCommand(provider_id, version->second.installed_version);
				if (!terminal.native_identity_command.empty()) terminal.native_identity_query_phase = 1;
			}
		}
	}
	if (terminal.native_identity_query_phase == 0) return false;
	IPlatformTerminalRuntime& runtime = PlatformServicesFactory::Instance().terminal_runtime;
	const std::function<void(bool)> finish = [&](bool clear_composer)
	{
		if (clear_composer) (void)runtime.WriteToCliTerminal(terminal, "\x15", 1);
		terminal.native_identity_query_phase = 0;
		terminal.native_identity_output.clear();
		if (terminal.running && !terminal.native_identity_deferred_input.empty())
		{
			const std::string deferred = std::exchange(terminal.native_identity_deferred_input, {});
			if (WriteToCliTerminal(terminal, deferred.data(), deferred.size()) && deferred.find_first_of("\r\n") != std::string::npos)
				MarkCliTerminalTurnBusy(terminal);
		}
		terminal.native_identity_deferred_input.clear();
	};
	if (!terminal.running || !terminal.attached_session_id.empty()) { finish(false); return false; }
	if (terminal.native_identity_output.size() + output.size() > 65536)
	{
		finish(terminal.native_identity_query_phase == 2);
		return false;
	}
	terminal.native_identity_output.append(output);
	if (terminal.native_identity_query_phase == 1)
	{
		if (terminal.last_user_input_time_s > 0.0 || now_s - terminal.startup_time_s > 30.0)
		{
			finish(false);
			return false;
		}
		const std::string text = StripTerminalControlSequencesForLifecycle(terminal.native_identity_output);
		const bool codex_ready = provider_id == provider_ids::kCodexCli &&
		    text.find("OpenAI Codex") != std::string::npos && text.find("(v") != std::string::npos &&
		    terminal.native_identity_output.find("\x1b]0;") != std::string::npos &&
		    text.rfind("Ask Codex to do anything") != std::string::npos &&
		    (text.rfind("Folder access") == std::string::npos || text.rfind("Folder access") < text.rfind("Ask Codex to do anything"));
		const bool gemini_ready = provider_id == provider_ids::kGeminiCli &&
		    text.rfind("Type your message or @path/to/file") != std::string::npos;
		if ((!codex_ready && !gemini_ready) || terminal.turn_state != CliTerminalTurnState::Idle) return false;
		if (!runtime.WriteToCliTerminal(terminal, terminal.native_identity_command.data(), terminal.native_identity_command.size()))
		{
			finish(false);
			return false;
		}
		terminal.native_identity_query_phase = 2;
		terminal.native_identity_query_time_s = now_s;
		terminal.native_identity_output.clear();
		return false;
	}
	if (now_s - terminal.native_identity_query_time_s > 10.0)
	{
		LogCliDiagnosticEvent(app, "native_session_link", "owned_status_timeout", &terminal);
		finish(terminal.native_identity_query_phase == 2);
		return false;
	}
	if (terminal.native_identity_query_phase == 2)
	{
		if (now_s - terminal.native_identity_query_time_s < 0.75) return false;
		if (!runtime.WriteToCliTerminal(terminal, "\r", 1)) { finish(true); return false; }
		terminal.native_identity_query_phase = 3;
		terminal.native_identity_output.clear();
		return false;
	}
	const std::string identity = NativeStatusSessionId(provider_id, terminal.native_identity_output);
	if (identity.empty()) return false;
	terminal.attached_session_id = identity;
	const bool changed = DiscoverCliTerminalNativeSession(app, terminal);
	finish(false);
	return changed;
}

bool ChatSyncIdsMatch(std::string_view lhs, std::string_view rhs)
{
	return uam::strings::TrimmedEqualsNonEmpty(lhs, rhs);
}

std::string NormalizeChatSyncTargetId(std::string_view chat_id)
{
	return uam::strings::Trim(chat_id);
}

bool ChatHasActiveAcpSession(const AppState& app, std::string_view chat_id)
{
	const std::string target_id = NormalizeChatSyncTargetId(chat_id);
	if (target_id.empty())
	{
		return false;
	}

	return std::ranges::any_of(app.acp_sessions, [&target_id](const auto& session) { return session != nullptr && ChatSyncIdsMatch(session->chat_id, target_id) && session->running && AcpSessionHasActiveTurn(*session); });
}

bool CliTerminalHasActiveTurn(const CliTerminalState& terminal)
{
	// Unknown activity must still block history rewrites and provider replacement.
	return terminal.native_identity_query_phase == 2 || terminal.native_identity_query_phase == 3 || terminal.native_session_discovery_ambiguous || terminal.lifecycle_state == CliTerminalLifecycleState::Unknown ||
	       uam::CliTerminalLifecycleStateIsProcessing(terminal.lifecycle_state) || terminal.turn_state == uam::CliTerminalTurnState::Busy;
}

bool ChatHasBusyCliTerminal(const AppState& app, std::string_view chat_id)
{
	const std::string target_id = NormalizeChatSyncTargetId(chat_id);
	if (target_id.empty())
	{
		return false;
	}

	const ChatSession* target_chat = ChatDomainService().FindChatById(app, target_id);
	if (target_chat == nullptr)
	{
		uam::CliTerminalState target_terminal;
		target_terminal.attached_session_id = target_id;
		target_chat = FindChatForCliTerminal(app, target_terminal);
	}

	if (target_chat != nullptr)
	{
		if (const CliTerminalState* terminal = FindCliTerminalForChat(app, *target_chat); terminal != nullptr)
		{
			return terminal->running && CliTerminalHasActiveTurn(*terminal);
		}
	}

	return std::ranges::any_of(app.cli_terminals, [&target_id](const auto& terminal) { return terminal != nullptr && terminal->running && uam::CliTerminalMatchesChatId(*terminal, target_id) && CliTerminalHasActiveTurn(*terminal); });
}

bool ChatHasRunningRuntime(const AppState& app, std::string_view chat_id)
{
	const std::string target_id = NormalizeChatSyncTargetId(chat_id);
	if (target_id.empty())
	{
		return false;
	}

	if (ChatHasActiveAcpSession(app, target_id))
	{
		return true;
	}

	return ChatHasBusyCliTerminal(app, target_id);
}

bool ChatHasActiveCliTerminal(const AppState& app, std::string_view chat_id)
{
	const std::string target_id = NormalizeChatSyncTargetId(chat_id);
	if (target_id.empty())
	{
		return false;
	}

	const ChatSession* target_chat = ChatDomainService().FindChatById(app, target_id);
	if (target_chat == nullptr)
	{
		uam::CliTerminalState target_terminal;
		target_terminal.attached_session_id = target_id;
		target_chat = FindChatForCliTerminal(app, target_terminal);
	}

	if (target_chat != nullptr)
	{
		if (const CliTerminalState* terminal = FindCliTerminalForChat(app, *target_chat); terminal != nullptr)
		{
			return terminal->running;
		}
	}

	return std::ranges::any_of(app.cli_terminals, [&target_id](const auto& terminal) { return terminal != nullptr && terminal->running && uam::CliTerminalMatchesChatId(*terminal, target_id); });
}

bool HasAnyActiveCliTerminal(const AppState& app)
{
	return std::ranges::any_of(app.cli_terminals, [](const auto& terminal) { return terminal != nullptr && terminal->running; });
}

void MarkChatUnseen(AppState& app, std::string_view chat_id)
{
	const std::string normalized_chat_id = NormalizeChatSyncTargetId(chat_id);
	if (normalized_chat_id.empty())
	{
		return;
	}

	if (ChatDomainService().SelectedChatId(app) == normalized_chat_id)
	{
		return;
	}

	app.chats_with_unseen_updates.emplace(normalized_chat_id);
}

void MarkSelectedChatSeen(AppState& app)
{
	const std::string selected_chat_id = ChatDomainService().SelectedChatId(app);
	if (!selected_chat_id.empty())
	{
		app.chats_with_unseen_updates.erase(selected_chat_id);
	}
}

bool ChatExists(const AppState& app, std::string_view chat_id)
{
	const std::string normalized_chat_id = NormalizeChatSyncTargetId(chat_id);
	return !normalized_chat_id.empty() && ChatDomainService().FindChatById(app, normalized_chat_id) != nullptr;
}

bool NativeChatMatchesPreferredSyncId(const ChatSession& chat, std::string_view preferred_chat_id)
{
	const std::string preferred_id = NormalizeChatSyncTargetId(preferred_chat_id);
	if (preferred_id.empty())
	{
		return false;
	}

	const NativeSessionLinkService native_session_links;
	return ChatSyncIdsMatch(chat.id, preferred_id) || ChatSyncIdsMatch(native_session_links.RealNativeSessionId(chat), preferred_id);
}

void RemoveMissingChatIds(const AppState& app, std::unordered_set<std::string>& chat_ids)
{
	std::unordered_set<std::string> normalized_existing_ids;
	normalized_existing_ids.reserve(chat_ids.size());
	for (const std::string& chat_id : chat_ids)
	{
		const std::string normalized_chat_id = NormalizeChatSyncTargetId(chat_id);
		if (ChatExists(app, normalized_chat_id))
		{
			normalized_existing_ids.emplace(normalized_chat_id);
		}
	}

	chat_ids = std::move(normalized_existing_ids);
}

bool FinalizeChatSyncSelection(uam::AppState& app, std::string_view selected_before, std::string_view preferred_chat_id, bool preserve_selection)
{
	const std::string selected_before_id = NormalizeChatSyncTargetId(selected_before);
	const std::string preferred_id = NormalizeChatSyncTargetId(preferred_chat_id);
	const std::string previous_selected = uam::strings::NonEmptyOrFallback(selected_before_id, preferred_id);

	if (preserve_selection && ChatExists(app, selected_before_id))
	{
		ChatDomainService().SelectChatById(app, selected_before_id);
	}
	else if (ChatExists(app, preferred_id))
	{
		ChatDomainService().SelectChatById(app, preferred_id);
	}
	else if (ChatExists(app, previous_selected))
	{
		ChatDomainService().SelectChatById(app, previous_selected);
	}
	else if (!app.chats.empty())
	{
		ChatDomainService().SelectChatById(app, app.chats.front().id);
	}
	else
	{
		ChatDomainService().SetSelectedChatIndexOrNearest(app, -1);
	}

	RemoveMissingChatIds(app, app.chats_with_unseen_updates);
	RemoveMissingChatIds(app, app.collapsed_branch_chat_ids);
	RemoveMissingChatIds(app, app.filtered_chat_ids);

	const std::string selected_now_id = ChatDomainService().SelectedChatId(app);

	if (selected_now_id != selected_before_id)
	{
		app.composer_text.clear();
	}

	std::string warning;
	ChatSession* selected_chat = ChatDomainService().SelectedChat(app);
	if (selected_chat != nullptr && !ChatRepository::HydrateChatMessages(app.data_root, *selected_chat, &warning))
	{
		UpdateNativeChatRefreshError(app, preferred_id, "could not load chat history. " + warning);
		LogCliDiagnosticEvent(app, "sync_native_history", "chat_load_failed", nullptr, app.status_line);
		return false;
	}
	MarkSelectedChatSeen(app);
	return true;
}

bool SyncChatsFromLoadedNative(AppState& app, std::vector<ChatSession> native_chats, std::string_view preferred_chat_id, bool preserve_selection)
{
	const std::string selected_before = ChatDomainService().SelectedChatId(app);
	const std::string preferred_id = NormalizeChatSyncTargetId(preferred_chat_id);
	(void)ChatHistorySyncService().OverlayLocalHistory(app, native_chats, true);

	// Only save the specifically requested chat (e.g. the one just discovered)
	for (ChatSession& chat : native_chats)
	{
		if (NativeChatMatchesPreferredSyncId(chat, preferred_id))
		{
			if (!ChatRepository::SaveChat(app.data_root, chat))
			{
				UpdateNativeChatRefreshError(app, preferred_id, "could not save chat history.");
				LogCliDiagnosticEvent(app, "sync_native_history", "chat_save_failed", nullptr, app.status_line);
				return false;
			}
			break;
		}
	}

	std::vector<ChatSession> chats = ChatRepository::LoadLocalChatSummaries(app.data_root);
	for (ChatSession& chat : chats)
	{
		if (!NativeChatMatchesPreferredSyncId(chat, preferred_id)) continue;
		std::string warning;
		if (!ChatRepository::HydrateChatMessages(app.data_root, chat, &warning))
		{
			UpdateNativeChatRefreshError(app, preferred_id, "could not load chat history. " + warning);
			LogCliDiagnosticEvent(app, "sync_native_history", "chat_load_failed", nullptr, app.status_line);
			return false;
		}
	}
	uam::runtime_orch_impl::ReplaceAppChatsWithNormalized(app, std::move(chats));
	if (!FinalizeChatSyncSelection(app, selected_before, preferred_id, preserve_selection)) return false;
	UpdateNativeChatRefreshError(app, preferred_id, {});
	return true;
}

bool SyncChatsFromNative(AppState& app, std::string_view preferred_chat_id, bool preserve_selection)
{
	const std::string selected_before = ChatDomainService().SelectedChatId(app);
	const std::string preferred_id = NormalizeChatSyncTargetId(preferred_chat_id);

	// Import from native to local before reloading the sidebar.
	// We only import the target chat to avoid re-importing chats the user manually deleted.
	const ChatHistorySyncService::ImportResult result = ChatHistorySyncService().ImportAllNativeChatsToLocal(app, false, preferred_id);
	if (!result.success)
	{
		UpdateNativeChatRefreshError(app, preferred_id, result.errors.empty() ? "could not import chat history." : result.errors.front());
		LogCliDiagnosticEvent(app, "sync_native_history", "import_failed", nullptr, app.status_line);
		return false;
	}

	ChatHistorySyncService().LoadSidebarChats(app);
	if (!FinalizeChatSyncSelection(app, selected_before, preferred_id, preserve_selection)) return false;
	UpdateNativeChatRefreshError(app, preferred_id, {});
	return true;
}

} // namespace uam

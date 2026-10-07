#pragma once

#include "common/provider/provider_profile.h"
#include "common/state/app_state.h"

#include <string>
#include <string_view>
#include <vector>

namespace uam
{

inline constexpr std::string_view kCliTerminalIdPrefix = "term-";

std::string CliTerminalIdForChat(std::string_view chat_id);
bool ProviderSupportsInteractiveTerminal(const ProviderProfile& provider);
std::string ProviderInteractiveTerminalUnavailableReason(const ProviderProfile& provider);
std::string ResolveProviderInteractiveResumeId(const AppState& app, const ChatSession& chat, const ProviderProfile& provider);
bool PrepareAcpSessionForCliTerminalLaunch(AppState& app, ChatSession& chat, std::string* error_out = nullptr);
/// <summary>Snapshots recent saved conversation text and compaction summaries within a 32 KiB history budget; current prompts are separate.</summary>
std::string BuildProviderHandoffContext(const ChatSession& chat);
std::vector<std::string> BuildProviderInteractiveArgv(const AppState& app, const ChatSession& chat);
/// <summary>Stages prior conversation as provider-native context without synthetic user turns.</summary>
bool PrepareCliProviderHandoff(AppState& app, ChatSession& chat, const ExecutionHost& host,
    std::vector<std::string>& argv, std::vector<std::pair<std::string, std::string>>& environment,
    std::string& launch_channel, std::string& error, std::stop_token stop_token = {});
/// <summary>Returns pending until isolated preparation finishes, validating the current launch before use.</summary>
bool PrepareCliProviderHandoffAsync(AppState& app, CliTerminalState& terminal, ChatSession& chat, const ExecutionHost& host,
    std::vector<std::string>& argv, std::vector<std::pair<std::string, std::string>>& environment,
    std::string& launch_channel, std::string& error);
/// <summary>Removes only owned context files after runtime shutdown; false keeps deletion retryable.</summary>
bool RemoveCliProviderHandoff(AppState& app, const ChatSession& chat, std::string& error, std::stop_token stop_token = {});
/// <summary>Durably stages cleanup independently of chat deletion and retries it outside the UI thread.</summary>
bool StageCliProviderContextCleanup(AppState& app, const ChatSession& chat);
void RetryCliProviderContextCleanup(AppState& app, double now_s);
void RepairCliTerminalIdentityForChat(AppState& app, CliTerminalState& terminal, const ChatSession& chat, const ProviderProfile& provider);
CliTerminalState& EnsureCliTerminalForChat(AppState& app, const ChatSession& chat);
void MarkSelectedCliTerminalForLaunch(AppState& app);

} // namespace uam

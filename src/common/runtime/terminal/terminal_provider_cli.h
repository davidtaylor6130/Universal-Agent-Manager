#pragma once

#include "common/provider/provider_profile.h"
#include "common/state/app_state.h"

#include <string>
#include <string_view>
#include <span>
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
std::string BuildProviderHandoffContext(const ChatSession& chat, std::stop_token stop_token = {}, std::string* error = nullptr);
std::string BuildProviderHandoffContext(std::span<const Message> messages, std::stop_token stop_token = {}, std::string* error = nullptr, std::size_t budget = 32768);
/// <summary>Holds saved context when quarantined history cannot prove its exact safe projection.</summary>
bool ValidateProviderHandoffContext(const ChatSession& chat, std::size_t history_count, std::string* error = nullptr, std::stop_token stop_token = {});
/// <summary>Builds the native terminal argv, including a read ceiling and a custom agent's native skill restriction.</summary>
std::vector<std::string> BuildProviderInteractiveArgv(const AppState& app, const ChatSession& chat);
/// <summary>Refuses CLI View when the provider cannot start in an enforced Plan for a Plan or read-only chat.</summary>
std::string ProviderInteractiveCeilingError(const AppState& app, const ChatSession& chat);
/// <summary>The custom UAM agent CLI View must carry, captured for one launch; empty for Build, Plan or no selection.</summary>
struct CliViewAgent
{
	std::string id;
	std::string instructions;
};
/// <summary>Captures the chat's managed or selected custom agent; false with an error when it cannot be captured.</summary>
bool ResolveCliViewAgent(const AppState& app, const ChatSession& chat, CliViewAgent& agent, std::string& error);
/// <summary>Refuses CLI View when the chat's custom agent cannot be captured or its provider cannot carry the
/// instructions; empty when they can be carried, Central Setup already carries them, or no custom agent applies.</summary>
std::string ProviderInteractiveAgentError(const AppState& app, const ChatSession& chat, const ExecutionHost& host);
/// <summary>Stages prior conversation, and a custom agent's instructions when Central Setup does not carry them,
/// as provider-native context without synthetic user turns.</summary>
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
bool StageCliProviderContextCleanup(AppState& app, const ChatSession& chat, const std::vector<ChatSession>& survivors);
void RetryCliProviderContextCleanup(AppState& app, double now_s);
void RepairCliTerminalIdentityForChat(AppState& app, CliTerminalState& terminal, const ChatSession& chat, const ProviderProfile& provider);
CliTerminalState& EnsureCliTerminalForChat(AppState& app, const ChatSession& chat);
void MarkSelectedCliTerminalForLaunch(AppState& app);

} // namespace uam

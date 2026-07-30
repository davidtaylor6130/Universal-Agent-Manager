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
bool ProviderUsesCodexCli(const ProviderProfile& provider);
bool ProviderSupportsInteractiveTerminal(const ProviderProfile& provider);
std::string ProviderInteractiveTerminalUnavailableReason(const ProviderProfile& provider);
std::string ResolveProviderInteractiveResumeId(const AppState& app, const ChatSession& chat, const ProviderProfile& provider);
bool PrepareAcpSessionForCliTerminalLaunch(AppState& app, ChatSession& chat, std::string* error_out = nullptr);
bool EnsureCopilotInteractiveSessionIdForLaunch(AppState& app, ChatSession& chat, const ProviderProfile& provider, std::string* error_out);
std::vector<std::string> BuildProviderInteractiveArgv(const AppState& app, const ChatSession& chat);
void RepairCliTerminalIdentityForChat(AppState& app, CliTerminalState& terminal, const ChatSession& chat, const ProviderProfile& provider);
CliTerminalState& EnsureCliTerminalForChat(AppState& app, const ChatSession& chat);
void MarkSelectedCliTerminalForLaunch(AppState& app);

} // namespace uam

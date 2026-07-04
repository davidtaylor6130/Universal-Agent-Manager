#pragma once

#include "common/state/app_state.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace uam
{

inline constexpr std::size_t kCliDiagnosticFieldMaxBytes = 4096;

const char* CliTurnStateLabel(const CliTerminalState& terminal);
const char* CliLifecycleStateLabel(const CliTerminalState& terminal);
const char* CliBoolLabel(bool value);
std::string CliSelectedChatId(const AppState& app);
const ChatSession* FindChatForCliDiagnostics(const AppState& app, const CliTerminalState& terminal);
std::string CliProviderIdForDiagnostics(const AppState& app, const CliTerminalState& terminal);
std::string CliNativeSessionIdForDiagnostics(const AppState& app, const CliTerminalState& terminal);
std::string CliAttachedSessionIdForDiagnostics(const AppState& app, const CliTerminalState& terminal);
std::string CliProcessHandleLabel(const CliTerminalState& terminal);
std::string CliDiagnosticQuotedField(std::string_view value);
void LogCliDiagnosticEvent(const AppState& app, std::string_view event_name, std::string_view reason, const CliTerminalState* terminal = nullptr, std::string_view note = "", long long bytes = -1);

} // namespace uam

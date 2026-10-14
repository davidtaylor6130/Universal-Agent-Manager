#pragma once

#include <string>

namespace uam { struct AppState; }

/// <summary>Controls the linked test terminal runtime without creating OS processes or handles.</summary>
class ComputerUseTerminalRuntimeFixture
{
public:
	ComputerUseTerminalRuntimeFixture();
	~ComputerUseTerminalRuntimeFixture();
	void AddTerminal(uam::AppState& app, const std::string& chat_id);
	int StopCount(const std::string& chat_id) const;
	void ConfirmExit(const std::string& chat_id);
};

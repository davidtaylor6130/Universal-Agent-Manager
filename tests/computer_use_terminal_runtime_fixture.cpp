#include "computer_use_terminal_runtime_fixture.h"
#include "common/platform/platform_services.h"
#include "common/state/app_state.h"

#include <map>
#include <set>

namespace
{
	class HandlerTerminalRuntime final : public IPlatformTerminalRuntime
	{
	public:
		mutable std::map<std::string, int> stop_counts;
		std::set<std::string> confirmed_exits;
		bool IsAvailable() const override { return true; }
		bool StartCliTerminalProcess(uam::CliTerminalState&, const std::filesystem::path&, const std::vector<std::string>&, std::string* error, const std::vector<std::pair<std::string, std::string>>&) const override
		{
			if (error != nullptr) *error = "The handler fixture cannot start terminal processes.";
			return false;
		}
		void CloseCliTerminalHandles(uam::CliTerminalState&) const override {}
		bool WriteToCliTerminal(uam::CliTerminalState&, const char*, std::size_t) const override { return false; }
		void StopCliTerminalProcess(uam::CliTerminalState& terminal, bool) const override { ++stop_counts[terminal.frontend_chat_id]; }
		void ResizeCliTerminal(uam::CliTerminalState&) const override {}
		std::ptrdiff_t ReadCliTerminalOutput(uam::CliTerminalState&, char*, std::size_t) const override { return -2; }
		bool HasReadableTerminalOutputHandle(const uam::CliTerminalState&) const override { return false; }
		bool PollCliTerminalProcessExited(uam::CliTerminalState& terminal) const override { return confirmed_exits.contains(terminal.frontend_chat_id); }
		bool SupportsAsyncNativeGeminiHistoryRefresh() const override
		{
#if defined(__APPLE__)
			return true;
#else
			return false;
#endif
		}
	};

	HandlerTerminalRuntime& Runtime()
	{
		static HandlerTerminalRuntime runtime;
		return runtime;
	}
}

ComputerUseTerminalRuntimeFixture::ComputerUseTerminalRuntimeFixture()
{
	Runtime().stop_counts.clear();
	Runtime().confirmed_exits.clear();
}

ComputerUseTerminalRuntimeFixture::~ComputerUseTerminalRuntimeFixture()
{
	Runtime().stop_counts.clear();
	Runtime().confirmed_exits.clear();
}

void ComputerUseTerminalRuntimeFixture::AddTerminal(uam::AppState& app, const std::string& chat_id)
{
	std::unique_ptr<uam::CliTerminalState> terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = chat_id;
	terminal->attached_chat_id = chat_id;
	terminal->terminal_id = chat_id;
	terminal->running = true;
	app.cli_terminals.push_back(std::move(terminal));
}

int ComputerUseTerminalRuntimeFixture::StopCount(const std::string& chat_id) const
{
	const std::map<std::string, int>::const_iterator found = Runtime().stop_counts.find(chat_id);
	return found == Runtime().stop_counts.end() ? 0 : found->second;
}

void ComputerUseTerminalRuntimeFixture::ConfirmExit(const std::string& chat_id)
{
	Runtime().confirmed_exits.insert(chat_id);
}

#if defined(__APPLE__)
namespace uam::platform_macos_impl
{
	IPlatformTerminalRuntime& GetMacTerminalRuntime() { return Runtime(); }
}
#elif defined(_WIN32)
namespace uam::platform_windows_impl
{
	IPlatformTerminalRuntime& GetWindowsTerminalRuntime() { return Runtime(); }
}
#endif

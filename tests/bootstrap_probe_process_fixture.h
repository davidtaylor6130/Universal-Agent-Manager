#pragma once

#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"
#include "remote/runner_bootstrap_internal.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace uam_test
{
	// No OS process implementation is linked by the standalone bootstrap tests.
	class BootstrapProbeProcessFixture final : public IPlatformProcessService
	{
	  public:
		uam::remote::detail::BootstrapProbeStatus outcome = uam::remote::detail::BootstrapProbeStatus::Succeeded;
		std::stop_source* cancel_on_read = nullptr;
		std::string output = "Linux\nx86_64\n";
		std::string diagnostic = "private-key=/private/fixture/key; secret-token";
		mutable std::size_t starts = 0;
		mutable std::size_t stops = 0;
		mutable std::size_t closes = 0;
		mutable std::size_t stdout_cursor = 0;
		mutable std::size_t stderr_cursor = 0;

		bool StartStdioProcess(uam::platform::StdioProcessPlatformFields&, const std::filesystem::path&,
		    const std::vector<std::string>&, std::string* error, const std::vector<std::pair<std::string, std::string>>&) const override
		{
			++starts;
			if (outcome == uam::remote::detail::BootstrapProbeStatus::LaunchFailed)
			{
				if (error != nullptr) *error = diagnostic;
				return false;
			}
			return true;
		}

		std::ptrdiff_t ReadStdioProcessStdout(uam::platform::StdioProcessPlatformFields&, char* buffer,
		    std::size_t size, std::string* error) const override
		{
			if (cancel_on_read != nullptr)
			{
				cancel_on_read->request_stop();
				return 0;
			}
			if (outcome == uam::remote::detail::BootstrapProbeStatus::ReadFailed)
			{
				if (error != nullptr) *error = diagnostic;
				return -1;
			}
			if (outcome == uam::remote::detail::BootstrapProbeStatus::OutputLimitExceeded)
			{
				std::memset(buffer, 'x', size);
				return static_cast<std::ptrdiff_t>(size);
			}
			return Read(output, stdout_cursor, buffer, size);
		}

		std::ptrdiff_t ReadStdioProcessStderr(uam::platform::StdioProcessPlatformFields&, char* buffer,
		    std::size_t size, std::string*) const override
		{
			return Read(diagnostic, stderr_cursor, buffer, size);
		}

		bool PollStdioProcessExited(uam::platform::StdioProcessPlatformFields&, int* exit_code) const override
		{
			if (cancel_on_read != nullptr) return false;
			if (exit_code != nullptr)
				*exit_code = outcome == uam::remote::detail::BootstrapProbeStatus::TransportFailed ? 255 :
				    outcome == uam::remote::detail::BootstrapProbeStatus::CommandFailed ? 127 : 0;
			return true;
		}

		void StopStdioProcess(uam::platform::StdioProcessPlatformFields&, bool) const override
		{
			++stops;
		}
		void CloseStdioProcessHandles(uam::platform::StdioProcessPlatformFields&) const override
		{
			++closes;
		}

		bool SupportsDetachedProcesses() const override
		{
			throw std::runtime_error("Unexpected non-probe call");
		}
		bool PopulateLocalTime(std::time_t, std::tm*) const override
		{
			throw std::runtime_error("Unexpected non-probe call");
		}
		std::string BuildShellCommandWithWorkingDirectory(const std::filesystem::path&, const std::string&) const override
		{
			throw std::runtime_error("Unexpected non-probe call");
		}
		bool CaptureCommandOutput(const std::string&, std::string*, int*, std::string*) const override
		{
			throw std::runtime_error("Unexpected non-probe call");
		}
		int NormalizeCapturedCommandExitCode(int) const override
		{
			throw std::runtime_error("Unexpected non-probe call");
		}
		ProcessExecutionResult ExecuteCommand(const std::string&, int, std::stop_token) const override
		{
			throw std::runtime_error("Unexpected non-probe call");
		}
		bool StartStdioProcessWithInput(uam::platform::StdioProcessPlatformFields&, const std::filesystem::path&, const std::vector<std::string>&, std::string_view, std::string*, const std::vector<std::pair<std::string, std::string>>&) const override
		{
			throw std::runtime_error("Unexpected non-probe call");
		}
		bool WriteToStdioProcess(uam::platform::StdioProcessPlatformFields&, const char*, std::size_t, std::string*) const override
		{
			throw std::runtime_error("Unexpected non-probe call");
		}
		void CloseStdioProcessInput(uam::platform::StdioProcessPlatformFields&) const override
		{
			throw std::runtime_error("Unexpected non-probe call");
		}
		void TerminateStdioProcess(uam::platform::StdioProcessPlatformFields&, bool) const override
		{
			throw std::runtime_error("Unexpected non-probe call");
		}
		std::filesystem::path ResolveCurrentExecutablePath() const override
		{
			throw std::runtime_error("Unexpected non-probe call");
		}
		std::unique_ptr<uam::platform::DataRootLock> TryAcquireDataRootLock(const std::filesystem::path&, std::string*) const override
		{
			throw std::runtime_error("Unexpected non-probe call");
		}
		uintmax_t NativeGeminiSessionMaxFileBytes() const override
		{
			throw std::runtime_error("Unexpected non-probe call");
		}
		std::size_t NativeGeminiSessionMaxMessages() const override
		{
			throw std::runtime_error("Unexpected non-probe call");
		}
		std::string GenerateUuid() const override
		{
			throw std::runtime_error("Unexpected non-probe call");
		}
		bool LaunchShellAt(const std::filesystem::path&, std::string*) const override
		{
			throw std::runtime_error("Unexpected non-probe call");
		}
		void SetKeepSystemAwake(bool) const override
		{
			throw std::runtime_error("Unexpected non-probe call");
		}
		bool EmbeddedBrowserUsesMockKeychain() const override
		{
			throw std::runtime_error("Unexpected non-probe call");
		}

	  private:
		static std::ptrdiff_t Read(const std::string& value, std::size_t& cursor, char* buffer, std::size_t size)
		{
			const auto count = std::min(size, value.size() - cursor);
			std::memcpy(buffer, value.data() + cursor, count);
			cursor += count;
			return static_cast<std::ptrdiff_t>(count);
		}
	};
}

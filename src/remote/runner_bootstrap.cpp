#include "remote/runner_bootstrap.h"

#include "common/config/execution_host_config.h"
#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"
#include "common/utils/shell_escape.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace uam::remote
{
	namespace
	{
		bool IsToken(std::string_view value, std::size_t maximum)
		{
			return !value.empty() && value.size() <= maximum &&
			       std::ranges::all_of(value, [](unsigned char character)
			       { return std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.'; });
		}

		bool IsSha256(std::string_view value)
		{
			return value.size() == 64 && std::ranges::all_of(value, [](unsigned char character)
			{ return std::isxdigit(character) != 0 && !std::isupper(character); });
		}

		std::vector<std::string> SshCommand(const std::string& alias, std::string command)
		{
			return {"ssh", "-T", "-o", "BatchMode=yes", "-o", "ClearAllForwardings=yes",
			        "-o", "ConnectTimeout=10", alias, std::move(command)};
		}

		bool RunStep(const BootstrapStep& step, std::string& output, std::string& diagnostic,
		             std::string& error,
		             std::stop_token stop_token)
		{
			auto& service = PlatformServicesFactory::Instance().process_service;
			uam::platform::StdioProcessPlatformFields process;
			if (!service.StartStdioProcess(process, std::filesystem::current_path(), step.argv,
			                               &error))
				return false;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
			std::array<char, 16 * 1024> buffer{};
			int exit_code = -1;
			for (;;)
			{
				for (const bool standard_error : {false, true})
				{
					for (;;)
					{
						std::string read_error;
						const std::ptrdiff_t read = standard_error
						    ? service.ReadStdioProcessStderr(process, buffer.data(), buffer.size(),
						                                     &read_error)
						    : service.ReadStdioProcessStdout(process, buffer.data(), buffer.size(),
						                                     &read_error);
						if (read <= 0) break;
						std::string& destination = standard_error ? diagnostic : output;
						if (destination.size() + static_cast<std::size_t>(read) > 1024 * 1024)
						{
							error = "Remote setup output exceeded 1 MiB.";
							service.StopStdioProcess(process, true);
							return false;
						}
						destination.append(buffer.data(), static_cast<std::size_t>(read));
					}
				}
				if (service.PollStdioProcessExited(process, &exit_code)) break;
				if (stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline)
				{
					error = stop_token.stop_requested() ? "Remote setup was canceled."
					                                    : "Remote setup timed out.";
					service.StopStdioProcess(process, true);
					return false;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			service.CloseStdioProcessHandles(process);
			if (exit_code != 0)
			{
				error = "Remote setup step failed: " + step.label;
				const std::string detail = uam::strings::Trim(
				    diagnostic.empty() ? output : diagnostic);
				if (!detail.empty()) error += " — " + detail;
				return false;
			}
			if (!step.expected_output.empty() &&
			    uam::strings::Trim(output) != step.expected_output)
			{
				error = "Remote runner version verification failed.";
				return false;
			}
			return true;
		}
	}

	bool BuildBootstrapPlan(const std::string& ssh_alias,
	                        const std::filesystem::path& local_runner,
	                        const std::string& version,
	                        const std::string& sha256,
	                        const std::string& nonce,
	                        BootstrapPlan& plan,
	                        std::string* error_out)
	{
		plan = {};
		const auto fail = [error_out](std::string error)
		{
			if (error_out != nullptr) *error_out = std::move(error);
			return false;
		};
		if (!uam::execution_hosts::IsSafeSshAlias(ssh_alias))
			return fail("Use one exact alias from ~/.ssh/config.");
		if (!IsToken(version, 64)) return fail("Runner version is invalid.");
		if (!IsSha256(sha256)) return fail("Runner checksum is invalid.");
		if (!IsToken(nonce, 64)) return fail("Runner install nonce is invalid.");
		std::error_code status_error;
		if (!std::filesystem::is_regular_file(local_runner, status_error) || status_error)
			return fail("The packaged runner artifact is missing.");

		const std::string relative_version_directory = ".local/share/uam/runner/" + version;
		const std::string remote_version_directory = "~/" + relative_version_directory;
		const std::string temporary = remote_version_directory + "/uam-runner.tmp-" + nonce;
		const std::string installed = remote_version_directory + "/uam-runner";
		const std::string verify =
		    "set -eu; file=" + temporary + "; "
		    "if command -v shasum >/dev/null 2>&1; then printf '%s  %s\\n' " + sha256 +
		    " \"$file\" | shasum -a 256 -c -; "
		    "elif command -v sha256sum >/dev/null 2>&1; then printf '%s  %s\\n' " + sha256 +
		    " \"$file\" | sha256sum -c -; else echo 'No SHA-256 verifier is available.' >&2; exit 69; fi; "
		    "chmod 700 \"$file\"; mv -f \"$file\" " + installed + "; "
		    "ln -sfn " + version + " ~/.local/share/uam/runner/current; "
		    "~/.local/share/uam/runner/current/uam-runner stop --socket ~/.local/share/uam/runner/uam.sock; "
		    "i=0; while [ -S ~/.local/share/uam/runner/uam.sock ] && [ \"$i\" -lt 50 ]; do i=$((i+1)); sleep 0.1; done; "
		    "~/.local/share/uam/runner/current/uam-runner start --socket ~/.local/share/uam/runner/uam.sock";

		plan.ssh_alias = ssh_alias;
		plan.version = version;
		plan.install_directory = remote_version_directory;
		plan.steps = {
		    {"Check remote platform", SshCommand(ssh_alias, "uname -s && uname -m"), ""},
		    {"Create private runner directory",
		     SshCommand(ssh_alias, "umask 077; mkdir -p " + remote_version_directory), ""},
		    {"Copy runner",
		     {"scp", "-q", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
		      local_runner.string(), ssh_alias + ":" + relative_version_directory +
		                                 "/uam-runner.tmp-" + nonce},
		     ""},
		    {"Verify and activate runner", SshCommand(ssh_alias, verify), ""},
		    {"Verify runner version",
		     SshCommand(ssh_alias,
		                "~/.local/share/uam/runner/current/uam-runner --version"),
		     version},
		};
		return true;
	}

	std::string BootstrapPlanPreview(const BootstrapPlan& plan)
	{
		std::ostringstream preview;
		preview << "Install UAM runner " << plan.version << " on SSH alias " << plan.ssh_alias
		        << " at " << plan.install_directory << "\n";
		for (std::size_t index = 0; index < plan.steps.size(); ++index)
			preview << index + 1 << ". " << plan.steps[index].label << ": "
			        << uam::shell::JoinEscapedArgs(plan.steps[index].argv) << '\n';
		return preview.str();
	}

	BootstrapResult ExecuteBootstrapPlan(const BootstrapPlan& plan, std::stop_token stop_token)
	{
		BootstrapResult result;
		if (plan.steps.size() != 5 || !uam::execution_hosts::IsSafeSshAlias(plan.ssh_alias))
		{
			result.error = "Remote setup plan is invalid.";
			return result;
		}
		for (std::size_t index = 0; index < plan.steps.size(); ++index)
		{
			std::string output;
			std::string diagnostic;
			if (!RunStep(plan.steps[index], output, diagnostic, result.error, stop_token))
				return result;
			if (index == 0)
			{
				std::istringstream values(output);
				std::getline(values, result.platform);
				std::getline(values, result.architecture);
				result.platform = uam::strings::Trim(result.platform);
				result.architecture = uam::strings::Trim(result.architecture);
				if (result.platform.empty() || result.architecture.empty())
				{
					result.error = "Remote platform detection returned an invalid response.";
					return result;
				}
#if defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
				constexpr std::string_view local_architecture = "arm64";
#elif defined(__x86_64__)
				constexpr std::string_view local_architecture = "x86_64";
#else
				constexpr std::string_view local_architecture = "unknown";
#endif
				if (result.platform != "Darwin" || result.architecture != local_architecture)
				{
					result.error = "This build can install its runner only on "
					               "macOS/" + std::string(local_architecture) +
					               "; the SSH host reported " + result.platform + "/" +
					               result.architecture + ".";
					return result;
				}
#else
				result.error = "SSH runner installation is not yet available from this platform.";
				return result;
#endif
			}
		}
		result.ok = true;
		return result;
	}
}

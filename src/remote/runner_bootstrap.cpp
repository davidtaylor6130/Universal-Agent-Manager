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
						if (read == -1)
						{
							error = read_error.empty() ? "Remote setup output could not be read."
							                           : std::move(read_error);
							service.StopStdioProcess(process, true);
							return false;
						}
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

	bool BuildBootstrapPlan(const std::string& ssh_alias, const std::string& version,
	                        const std::string& nonce,
	                        std::vector<RunnerArtifact> artifacts,
	                        BootstrapPlan& plan, std::string* error_out)
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
		if (!IsToken(nonce, 64)) return fail("Runner install nonce is invalid.");
		if (artifacts.empty()) return fail("No packaged remote runner artifacts are available.");
		for (const RunnerArtifact& artifact : artifacts)
		{
			std::error_code status_error;
			if ((artifact.platform != "linux" && artifact.platform != "windows") ||
			    (artifact.architecture != "arm64" && artifact.architecture != "x86_64") ||
			    !IsSha256(artifact.sha256) ||
			    !std::filesystem::is_regular_file(artifact.path, status_error) || status_error)
				return fail("A packaged remote runner artifact is invalid.");
		}

		plan.ssh_alias = ssh_alias;
		plan.version = version;
		plan.install_directory = "the selected host's private UAM runner directory";
		plan.nonce = nonce;
		plan.artifacts = std::move(artifacts);
		plan.steps = {
		    {"Check remote platform", SshCommand(ssh_alias, "uname -s && uname -m"), ""},
		    {"Fallback Windows platform check",
		     SshCommand(ssh_alias,
		         "powershell.exe -NoLogo -NoProfile -NonInteractive -Command \"'Windows'; $env:PROCESSOR_ARCHITECTURE\""),
		     ""},
		};
		return true;
	}

	std::string BootstrapPlanPreview(const BootstrapPlan& plan)
	{
		std::ostringstream preview;
		preview << "Install UAM runner " << plan.version << " on SSH alias " << plan.ssh_alias
		        << " at " << plan.install_directory << "\n";
		preview << "1. Detect Linux or Windows and CPU architecture over SSH.\n"
		        << "2. Select the matching bundled helper; unsupported targets stop before copying.\n"
		        << "3. Copy to a private versioned user directory.\n"
		        << "4. Verify SHA-256, activate, restart, and verify the exact version.\n";
		return preview.str();
	}

	BootstrapResult ExecuteBootstrapPlan(const BootstrapPlan& plan, std::stop_token stop_token)
	{
		BootstrapResult result;
		if (plan.steps.size() != 2 || plan.artifacts.empty() ||
		    !uam::execution_hosts::IsSafeSshAlias(plan.ssh_alias))
		{
			result.error = "Remote setup plan is invalid.";
			return result;
		}
		std::string output;
		std::string diagnostic;
		std::string probe_error;
		bool unix_probe = RunStep(plan.steps[0], output, diagnostic, probe_error, stop_token);
		if (!unix_probe)
		{
			output.clear();
			diagnostic.clear();
			if (!RunStep(plan.steps[1], output, diagnostic, result.error, stop_token))
			{
				result.error = "Remote host is not a supported Ubuntu/Linux or Windows OpenSSH host.";
				return result;
			}
		}
		std::istringstream values(output);
		std::getline(values, result.platform);
		std::getline(values, result.architecture);
		result.platform = uam::strings::Trim(result.platform);
		result.architecture = uam::strings::Trim(result.architecture);
		if (result.platform == "Linux") result.platform = "linux";
		else if (result.platform == "Windows") result.platform = "windows";
		else
		{
			result.error = "Remote host is not Ubuntu/Linux or Windows.";
			return result;
		}
		std::ranges::transform(result.architecture, result.architecture.begin(),
		                       [](unsigned char character)
		                       { return static_cast<char>(std::tolower(character)); });
		if (result.architecture == "aarch64" || result.architecture == "arm64")
			result.architecture = "arm64";
		else if (result.architecture == "x86_64" || result.architecture == "amd64")
			result.architecture = "x86_64";
		else
		{
			result.error = "Remote CPU architecture is unsupported: " + result.architecture + ".";
			return result;
		}
		const auto artifact = std::ranges::find_if(plan.artifacts, [&](const RunnerArtifact& value)
		{
			return value.platform == result.platform &&
			       value.architecture == result.architecture;
		});
		if (artifact == plan.artifacts.end())
		{
			result.error = "This UAM build does not contain a runner for " + result.platform +
			               "/" + result.architecture + ".";
			return result;
		}

		std::vector<BootstrapStep> install_steps;
		if (result.platform == "linux")
		{
			const std::string relative = ".local/share/uam/runner/" + plan.version;
			const std::string directory = "~/" + relative;
			const std::string temporary = directory + "/uam-runner.tmp-" + plan.nonce;
			const std::string installed = directory + "/uam-runner";
			const std::string verify =
			    "set -eu; file=" + temporary + "; trap 'rm -f \"$file\"' EXIT; "
			    "printf '%s  %s\\n' " + artifact->sha256 +
			    " \"$file\" | sha256sum -c -; chmod 700 \"$file\"; "
			    "\"$file\" stop --socket ~/.local/share/uam/runner/uam.sock; "
			    "mv -f \"$file\" " + installed + "; " + installed +
			    " start --socket ~/.local/share/uam/runner/uam.sock";
			install_steps = {
			    {"Create private runner directory", SshCommand(plan.ssh_alias,
			        "umask 077; mkdir -p " + directory), ""},
			    {"Copy runner", {"scp", "-q", "-o", "BatchMode=yes", "-o",
			        "ConnectTimeout=10", artifact->path.string(), plan.ssh_alias + ":" +
			        relative + "/uam-runner.tmp-" + plan.nonce}, ""},
			    {"Verify and activate runner", SshCommand(plan.ssh_alias, verify), ""},
			    {"Verify runner version", SshCommand(plan.ssh_alias, installed + " --version"),
			        plan.version},
			};
		}
		else
		{
			const std::string relative = ".uam/runner/" + plan.version;
			const std::string temporary = relative + "/uam-runner.tmp-" + plan.nonce + ".exe";
			const std::string installed = ".uam/runner/" + plan.version + "/uam-runner.exe";
			const std::string verify =
			    "powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \""
			    "$file=Join-Path $HOME '" + temporary + "'; "
			    "try { if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne '" +
			    artifact->sha256 + "') { throw 'Runner checksum mismatch.' }; "
			    "& $file stop; $installed=Join-Path $HOME '.uam/runner/" + plan.version +
			    "/uam-runner.exe'; $moved=$false; for ($i=0; $i -lt 50 -and -not $moved; $i++) { "
			    "try { Move-Item -LiteralPath $file -Destination $installed -Force -ErrorAction Stop; $moved=$true } "
			    "catch { Start-Sleep -Milliseconds 100 } }; if (-not $moved) { throw 'Runner service did not release its executable.' }; "
			    "& $installed start; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } } "
			    "finally { Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue }\"";
			install_steps = {
			    {"Create private runner directory", SshCommand(plan.ssh_alias,
			        "powershell.exe -NoLogo -NoProfile -NonInteractive -Command \"New-Item -ItemType Directory -Force -Path (Join-Path $HOME '" + relative + "') | Out-Null\""), ""},
			    {"Copy runner", {"scp", "-q", "-o", "BatchMode=yes", "-o",
			        "ConnectTimeout=10", artifact->path.string(), plan.ssh_alias + ":" + temporary}, ""},
			    {"Verify and activate runner", SshCommand(plan.ssh_alias, verify), ""},
			    {"Verify runner version", SshCommand(plan.ssh_alias,
			        "powershell.exe -NoLogo -NoProfile -NonInteractive -Command \"& (Join-Path $HOME '" + installed + "') --version\""),
			        plan.version},
			};
		}
		for (const BootstrapStep& step : install_steps)
		{
			output.clear();
			diagnostic.clear();
			if (!RunStep(step, output, diagnostic, result.error, stop_token)) return result;
		}
		result.ok = true;
		return result;
	}
}

#include "remote/runner_bootstrap.h"

#include "common/config/execution_host_config.h"
#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"
#include "common/utils/base64.h"
#include "common/utils/shell_escape.h"
#include "common/utils/string_utils.h"
#include "remote/runner_protocol.h"

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

		std::string PowerShellCommand(std::string_view script)
		{
			std::string utf16_le;
			utf16_le.reserve(script.size() * 2);
			for (const char character : script)
			{
				utf16_le.push_back(character);
				utf16_le.push_back('\0');
			}
			return "powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass "
			       "-EncodedCommand " + uam::base64::Encode(utf16_le);
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
				error = "Remote setup step failed (exit " + std::to_string(exit_code) + "): " +
				        step.label;
				std::string detail = uam::strings::Trim(output);
				const std::string stderr_detail = uam::strings::Trim(diagnostic);
				if (!stderr_detail.empty())
				{
					if (!detail.empty()) detail += " — ";
					detail += stderr_detail;
				}
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
	                        BootstrapPlan& plan, std::string* error_out,
	                        const std::string& runner_directory)
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
		if (!uam::execution_hosts::IsSafeRunnerDirectory(runner_directory))
			return fail("The helper folder must be a safe relative path under the remote user's home directory.");
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
		plan.runner_directory = runner_directory;
		std::ranges::replace(plan.runner_directory, '\\', '/');
		plan.install_directory = plan.runner_directory.empty()
		    ? "the recommended private UAM folder under the remote user's home directory"
		    : "the remote user's home directory / " + plan.runner_directory;
		plan.nonce = nonce;
		plan.artifacts = std::move(artifacts);
		plan.steps = {
		    {"Check remote platform", SshCommand(ssh_alias, "uname -s && uname -m"), ""},
		    {"Fallback Windows platform check",
		     SshCommand(ssh_alias, PowerShellCommand("'Windows'; $env:PROCESSOR_ARCHITECTURE")),
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
		        << "4. Verify SHA-256, activate, restart, and verify version and protocol compatibility.\n";
		return preview.str();
	}

	BootstrapResult ExecuteBootstrapPlan(const BootstrapPlan& plan, std::stop_token stop_token)
	{
		BootstrapResult result;
		if (plan.steps.size() != 2 || plan.artifacts.empty() ||
		    !uam::execution_hosts::IsSafeSshAlias(plan.ssh_alias) ||
		    !uam::execution_hosts::IsSafeRunnerDirectory(plan.runner_directory))
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
			const std::string root = uam::execution_hosts::RunnerDirectory(
			    result.platform, plan.runner_directory);
			const std::string relative = root + "/" + plan.version;
			const std::string directory = "~/" + relative;
			const std::string temporary = directory + "/uam-runner.tmp-" + plan.nonce;
			const std::string installed = directory + "/uam-runner";
			const std::string backup = directory + "/uam-runner.rollback-" + plan.nonce;
			const bool can_restore_previous = plan.previous_platform == result.platform &&
			    IsToken(plan.previous_version, 64) &&
			    uam::execution_hosts::IsSafeRunnerDirectory(plan.previous_runner_directory) &&
			    plan.previous_protocol_version > 0;
			const std::string previous_root = can_restore_previous
			    ? uam::execution_hosts::RunnerDirectory(result.platform,
			                                              plan.previous_runner_directory)
			    : std::string();
			const std::string previous = can_restore_previous
			    ? "~/" + previous_root + "/" + plan.previous_version + "/uam-runner"
			    : std::string();
			const std::string stop_previous = can_restore_previous
			    ? "\"" + previous + "\" stop --socket ~/" + previous_root + "/uam.sock || true; "
			    : "\"$file\" stop --socket ~/" + root + "/uam.sock || true; ";
			const std::string restore_previous = can_restore_previous
			    ? "\"" + previous + "\" start --socket ~/" + previous_root +
			          "/uam.sock && test \"$(\"" + previous + "\" --version)\" = " +
			          plan.previous_version + " && test \"$(\"" + previous +
			          "\" --protocol-version)\" = " +
			          std::to_string(plan.previous_protocol_version) + "; "
			    : std::string();
			const std::string verify =
			    "set -eu; file=" + temporary + "; installed=" + installed +
			    "; backup=" + backup + "; trap 'rm -f \"$file\"' EXIT; "
			    "printf '%s  %s\\n' " + artifact->sha256 +
			    " \"$file\" | sha256sum -c -; chmod 700 \"$file\"; "
			    "had_backup=0; if test -f \"$installed\"; then cp -p \"$installed\" \"$backup\"; had_backup=1; fi; "
			    + stop_previous +
			    "if mv -f \"$file\" \"$installed\" && \"$installed\" start --socket ~/" + root +
			    "/uam.sock && test \"$(\"$installed\" --version)\" = " + plan.version +
			    " && test \"$(\"$installed\" --protocol-version)\" = " +
			    std::to_string(kRunnerProtocolVersion) +
			    "; then :; else status=$?; \"$installed\" stop --socket ~/" + root +
			    "/uam.sock || true; if test \"$had_backup\" = 1; then "
			    "mv -f \"$backup\" \"$installed\"; else rm -f \"$installed\"; fi; " +
			    restore_previous + "exit \"$status\"; fi";
			install_steps = {
			    {"Create private runner directory", SshCommand(plan.ssh_alias,
			        "umask 077; mkdir -p " + directory), ""},
			    {"Copy runner", {"scp", "-q", "-o", "BatchMode=yes", "-o",
			        "ConnectTimeout=10", artifact->path.string(), plan.ssh_alias + ":" +
			        relative + "/uam-runner.tmp-" + plan.nonce}, ""},
			    {"Verify and activate runner", SshCommand(plan.ssh_alias, verify), ""},
			    {"Verify runner version", SshCommand(plan.ssh_alias, installed + " --version"),
			        plan.version},
			    {"Verify runner protocol", SshCommand(plan.ssh_alias,
			        installed + " --protocol-version"),
			        std::to_string(kRunnerProtocolVersion)},
			};
		}
		else
		{
			const std::string root = uam::execution_hosts::RunnerDirectory(
			    result.platform, plan.runner_directory);
			const std::string relative = root + "/" + plan.version;
			const std::string temporary = relative + "/uam-runner.tmp-" + plan.nonce + ".exe";
			const std::string installed = relative + "/uam-runner.exe";
			const std::string backup = relative + "/uam-runner.rollback-" + plan.nonce + ".exe";
			const bool can_restore_previous = plan.previous_platform == result.platform &&
			    IsToken(plan.previous_version, 64) &&
			    uam::execution_hosts::IsSafeRunnerDirectory(plan.previous_runner_directory) &&
			    plan.previous_protocol_version > 0;
			const std::string previous_relative = can_restore_previous
			    ? uam::execution_hosts::RunnerDirectory(result.platform,
			                                              plan.previous_runner_directory) +
			          "/" + plan.previous_version + "/uam-runner.exe"
			    : std::string();
			const std::string previous_setup = can_restore_previous
			    ? "$previous=Join-Path $HOME '" + previous_relative + "'; "
			    : std::string();
			const std::string stop_previous = can_restore_previous
			    ? "& $previous stop | Out-Null; "
			    : "& $file stop | Out-Null; ";
			const std::string restore_previous = can_restore_previous
			    ? "& $previous start | Out-Null; if ($LASTEXITCODE -ne 0) { throw 'Previous runner service could not restart.' }; "
			      "if ((& $previous --version) -ne '" + plan.previous_version +
			          "') { throw 'Previous runner version verification failed.' }; "
			      "if ((& $previous --protocol-version) -ne '" +
			          std::to_string(plan.previous_protocol_version) +
			          "') { throw 'Previous runner protocol verification failed.' }; "
			    : std::string();
			const std::string verify = PowerShellCommand(
			    "$file=Join-Path $HOME '" + temporary + "'; "
			    "$installed=Join-Path $HOME '" + installed + "'; $backup=Join-Path $HOME '" + backup + "'; " +
			    previous_setup + "$hadBackup=$false; "
			    "try { if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne '" +
			    artifact->sha256 + "') { throw 'Runner checksum mismatch.' }; "
			    "if (Test-Path -LiteralPath $installed) { Copy-Item -LiteralPath $installed -Destination $backup -Force; $hadBackup=$true }; "
			    + stop_previous + "$moved=$false; for ($i=0; $i -lt 50 -and -not $moved; $i++) { "
			    "try { Move-Item -LiteralPath $file -Destination $installed -Force -ErrorAction Stop; $moved=$true } "
			    "catch { Start-Sleep -Milliseconds 100 } }; if (-not $moved) { throw 'Runner service did not release its executable.' }; "
			    "& $installed start; if ($LASTEXITCODE -ne 0) { throw 'Runner service could not start.' }; "
			    "if ((& $installed --version) -ne '" + plan.version + "') { throw 'Runner version verification failed.' }; "
			    "if ((& $installed --protocol-version) -ne '" + std::to_string(kRunnerProtocolVersion) + "') { throw 'Runner protocol verification failed.' }; "
			    "} catch { $failed=$_; try { & $installed stop | Out-Null } catch {}; "
			    "if ($hadBackup -and (Test-Path -LiteralPath $backup)) { Move-Item -LiteralPath $backup -Destination $installed -Force } "
			    "elseif (Test-Path -LiteralPath $installed) { Remove-Item -LiteralPath $installed -Force }; " +
			    restore_previous + "throw $failed } "
			    "finally { if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force } }");
			install_steps = {
			    {"Create private runner directory", SshCommand(plan.ssh_alias,
			        PowerShellCommand("New-Item -ItemType Directory -Force -Path (Join-Path $HOME '" +
			                          relative + "') | Out-Null")), ""},
			    {"Copy runner", {"scp", "-q", "-o", "BatchMode=yes", "-o",
			        "ConnectTimeout=10", artifact->path.string(), plan.ssh_alias + ":" + temporary}, ""},
			    {"Verify and activate runner", SshCommand(plan.ssh_alias, verify), ""},
			    {"Verify runner version", SshCommand(plan.ssh_alias,
			        PowerShellCommand("& (Join-Path $HOME '" + installed + "') --version")),
			        plan.version},
			    {"Verify runner protocol", SshCommand(plan.ssh_alias,
			        PowerShellCommand("& (Join-Path $HOME '" + installed +
			                          "') --protocol-version")),
			        std::to_string(kRunnerProtocolVersion)},
			};
		}
		bool activated = false;
		for (const BootstrapStep& step : install_steps)
		{
			output.clear();
			diagnostic.clear();
			if (!RunStep(step, output, diagnostic, result.error, stop_token))
			{
				if (activated)
				{
					std::string rollback_error;
					if (!FinalizeBootstrapPlan(plan, result, false, &rollback_error, stop_token) &&
					    !rollback_error.empty())
						result.error += " Rollback failed: " + rollback_error;
				}
				return result;
			}
			if (step.label == "Verify and activate runner") activated = true;
		}
		result.ok = true;
		return result;
	}

	bool FinalizeBootstrapPlan(const BootstrapPlan& plan, const BootstrapResult& result,
	                           bool keep_new_runner, std::string* error_out,
	                           std::stop_token stop_token)
	{
		const auto fail = [error_out](std::string error)
		{
			if (error_out != nullptr) *error_out = std::move(error);
			return false;
		};
		if ((result.platform != "linux" && result.platform != "windows") ||
		    !IsToken(plan.version, 64) || !IsToken(plan.nonce, 64) ||
		    !uam::execution_hosts::IsSafeSshAlias(plan.ssh_alias) ||
		    !uam::execution_hosts::IsSafeRunnerDirectory(plan.runner_directory))
			return fail("Remote setup rollback plan is invalid.");

		const std::string root = uam::execution_hosts::RunnerDirectory(
		    result.platform, plan.runner_directory);
		const std::string relative = root + "/" + plan.version;
		const bool can_restore_previous = plan.previous_platform == result.platform &&
		    IsToken(plan.previous_version, 64) &&
		    uam::execution_hosts::IsSafeRunnerDirectory(plan.previous_runner_directory) &&
		    plan.previous_protocol_version > 0;
		std::string command;
		if (result.platform == "linux")
		{
			const std::string installed = "~/" + relative + "/uam-runner";
			const std::string backup = "~/" + relative + "/uam-runner.rollback-" + plan.nonce;
			if (keep_new_runner)
				command = "rm -f " + backup;
			else
			{
				command = "set -eu; installed=" + installed + "; backup=" + backup +
				    "; \"$installed\" stop --socket ~/" + root +
				    "/uam.sock || true; if test -f \"$backup\"; then mv -f \"$backup\" \"$installed\"; else rm -f \"$installed\"; fi; ";
				if (can_restore_previous)
				{
					const std::string previous_root = uam::execution_hosts::RunnerDirectory(
					    result.platform, plan.previous_runner_directory);
					const std::string previous = "~/" + previous_root + "/" +
					    plan.previous_version + "/uam-runner";
					command += "\"" + previous + "\" start --socket ~/" + previous_root +
					    "/uam.sock; test \"$(\"" + previous + "\" --version)\" = " +
					    plan.previous_version + "; test \"$(\"" + previous +
					    "\" --protocol-version)\" = " +
					    std::to_string(plan.previous_protocol_version);
				}
			}
		}
		else
		{
			const std::string installed = relative + "/uam-runner.exe";
			const std::string backup = relative + "/uam-runner.rollback-" + plan.nonce + ".exe";
			if (keep_new_runner)
				command = PowerShellCommand("$backup=Join-Path $HOME '" + backup +
				    "'; Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue");
			else
			{
				std::string script = "$installed=Join-Path $HOME '" + installed +
				    "'; $backup=Join-Path $HOME '" + backup +
				    "'; try { & $installed stop | Out-Null } catch {}; "
				    "if (Test-Path -LiteralPath $backup) { Move-Item -LiteralPath $backup -Destination $installed -Force } "
				    "elseif (Test-Path -LiteralPath $installed) { Remove-Item -LiteralPath $installed -Force }; ";
				if (can_restore_previous)
				{
					const std::string previous = uam::execution_hosts::RunnerDirectory(
					    result.platform, plan.previous_runner_directory) + "/" +
					    plan.previous_version + "/uam-runner.exe";
					script += "$previous=Join-Path $HOME '" + previous +
					    "'; & $previous start | Out-Null; if ($LASTEXITCODE -ne 0) { throw 'Previous runner service could not restart.' }; "
					    "if ((& $previous --version) -ne '" + plan.previous_version +
					    "') { throw 'Previous runner version verification failed.' }; "
					    "if ((& $previous --protocol-version) -ne '" +
					    std::to_string(plan.previous_protocol_version) +
					    "') { throw 'Previous runner protocol verification failed.' }";
				}
				command = PowerShellCommand(script);
			}
		}
		BootstrapStep step{"Finalize runner activation", SshCommand(plan.ssh_alias, command), ""};
		std::string output;
		std::string diagnostic;
		std::string error;
		if (!RunStep(step, output, diagnostic, error, stop_token)) return fail(std::move(error));
		return true;
	}
}

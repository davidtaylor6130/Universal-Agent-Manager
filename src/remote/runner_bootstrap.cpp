#include "remote/runner_bootstrap.h"

#include "common/config/execution_host_config.h"
#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"
#include "common/paths/path_utils.h"
#include "common/utils/base64.h"
#include "common/utils/shell_escape.h"
#include "common/utils/string_utils.h"
#include "remote/runner_protocol.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <memory>
#include <mutex>
#include <vector>

namespace uam::remote
{
	namespace
	{
		bool IsToken(std::string_view value, std::size_t maximum)
		{
			return !value.empty() && value != "." && value != ".." && value.size() <= maximum &&
			       std::ranges::all_of(value, [](unsigned char character)
			       { return std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.'; });
		}

		bool IsSha256(std::string_view value)
		{
			return value.size() == 64 && std::ranges::all_of(value, [](unsigned char character)
			{ return std::isxdigit(character) != 0 && !std::isupper(character); });
		}

		constexpr std::string_view kUnavailableWorkingDirectoryError =
		    "The remote setup working directory is unavailable.";

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

		/// <summary>Keep the Windows OpenSSH command below cmd.exe's limit without consuming the transaction input stream.</summary>
		std::string PowerShellUtf8Command(std::string_view script)
		{
			return "powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass "
			       "-Command \"& ([scriptblock]::Create([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('" +
			       uam::base64::Encode(script) + "'))))\"";
		}

		bool RunStep(const BootstrapStep& step, std::string& output, std::string& diagnostic,
		             std::string& error,
		             std::stop_token stop_token)
		{
			if (stop_token.stop_requested())
			{
				error = "Remote setup was canceled.";
				return false;
			}
			auto& service = PlatformServicesFactory::Instance().process_service;
			uam::platform::StdioProcessPlatformFields process;
			std::error_code current_path_error;
			const std::optional<std::filesystem::path> current_path =
			    uam::paths::CurrentPathNoThrow(&current_path_error);
			if (!current_path)
			{
				error = std::string(kUnavailableWorkingDirectoryError);
				return false;
			}
			if (!service.StartStdioProcess(process, *current_path, step.argv, &error))
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

	class BootstrapInstallLease
	{
	  public:
		IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
		uam::platform::StdioProcessPlatformFields process;
		std::mutex mutex;
		bool released = false;
		bool separate_commands = false;
		std::vector<std::string> release_argv;

		~BootstrapInstallLease()
		{
			if (separate_commands)
			{
				Release();
				return;
			}
			// Losing the UI callback closes the guard's input. Its target-side
			// finally/trap rolls back the unfinished transaction before unlocking.
			std::lock_guard<std::mutex> guard(mutex);
			if (!released)
			{
				service.CloseStdioProcessInput(process);
				service.CloseStdioProcessHandles(process);
			}
		}

		bool Held(std::string& error)
		{
			std::lock_guard<std::mutex> guard(mutex);
			int status = -1;
			if (released || service.PollStdioProcessExited(process, &status))
			{
				error = "The remote helper installation lock was lost. Retry setup after checking the host.";
				return false;
			}
			return true;
		}

		bool Run(const BootstrapStep& step, std::string& output, std::string& error, std::stop_token stop_token)
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (released || step.argv.empty()) { error = "The helper installation lock is unavailable."; return false; }
			// Windows OpenSSH buffers PowerShell input until EOF. Keep the OS lock
			// on its own connection and execute finite commands on separate connections.
			if (separate_commands)
			{
				std::string diagnostic;
				return RunStep(step, output, diagnostic, error, stop_token);
			}
			std::string command = step.argv.back();
			if (command.starts_with("powershell.exe "))
			{
				std::string utf16;
				if (!uam::base64::Decode(command.substr(command.find_last_of(' ') + 1), utf16)) return false;
				command.clear();
				for (std::size_t index = 0; index < utf16.size(); index += 2) command += utf16[index];
			}
			const std::string request = uam::base64::Encode(command) + "\n";
			if (!service.WriteToStdioProcess(process, request.data(), request.size(), &error)) return false;
			std::string received;
			std::string diagnostic;
			std::array<char, 4096> buffer{};
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
			for (;;)
			{
				for (const bool stderr_output : {false, true})
				{
					const std::ptrdiff_t count = stderr_output
					    ? service.ReadStdioProcessStderr(process, buffer.data(), buffer.size(), &error)
					    : service.ReadStdioProcessStdout(process, buffer.data(), buffer.size(), &error);
					if (count == -1) return false;
					if (count > 0) (stderr_output ? diagnostic : received).append(buffer.data(), static_cast<std::size_t>(count));
				}
				const std::size_t newline = received.find('\n');
				if (newline != std::string::npos)
				{
					const std::string line = uam::strings::Trim(received.substr(0, newline));
					const std::size_t status_end = line.find(':', 19);
					if (!line.starts_with("UAM_INSTALL_RESULT:") || status_end == std::string::npos ||
					    !uam::base64::Decode(line.substr(status_end + 1), output))
					{
						error = "The guarded helper transaction returned an invalid result.";
						return false;
					}
					if (line.substr(19, status_end - 19) != "0")
					{
						error = "Remote setup step failed: " + step.label + ". " + uam::strings::Trim(output);
						return false;
					}
					if (!step.expected_output.empty() && uam::strings::Trim(output) != step.expected_output)
					{
						error = "Remote runner version verification failed.";
						return false;
					}
					return true;
				}
				if (received.size() + diagnostic.size() > 2 * 1024 * 1024 || service.PollStdioProcessExited(process) ||
				    stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline)
				{
					error = stop_token.stop_requested() ? "Remote setup was canceled." :
					    diagnostic.empty() ? "The guarded helper transaction disconnected or timed out." : uam::strings::Trim(diagnostic);
					return false;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}

		void Release()
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (released) return;
			if (!release_argv.empty() && !service.PollStdioProcessExited(process))
			{
				std::string output;
				std::string diagnostic;
				std::string error;
				RunStep({"Release helper installation lock", release_argv, ""}, output, diagnostic, error, {});
			}
			service.CloseStdioProcessInput(process);
			// Give the target guard time to finish its rollback before closing SSH.
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!service.PollStdioProcessExited(process) && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			service.StopStdioProcess(process, true);
			released = true;
		}
	};

	static std::string BootstrapFinalizationCommand(const BootstrapPlan& plan, std::string_view platform, bool keep_new_runner, bool wrap_powershell = true)
	{
		const std::string root = uam::execution_hosts::RunnerDirectory(
		    platform, plan.runner_directory);
		const std::string relative = root + "/" + plan.version;
		std::string command;
		if (platform == "linux")
		{
			const std::string installed = "~/" + relative + "/uam-runner";
			const std::string backup = "~/" + relative + "/uam-runner.rollback-" + plan.nonce;
			const std::string marker = "~/" + relative + "/uam-runner.activation-" + plan.nonce;
			if (keep_new_runner)
				command = "rm -f " + backup + " " + marker;
			else
			{
				command = "set -eu; installed=" + installed + "; backup=" + backup +
				    "; marker=" + marker + "; if test -f \"$marker\"; then "
				    "\"$installed\" stop --socket ~/" + root + "/" +
				    RunnerEndpointName(plan.version) +
				    ".sock || true; if test -f \"$backup\"; then cp -p \"$backup\" \"$installed\"; else rm -f \"$installed\"; fi; "
				    "fi; rm -f \"$marker\"; rm -f \"$backup\"";
			}
		}
		else
		{
			const std::string installed = relative + "/uam-runner.exe";
			const std::string backup = relative + "/uam-runner.rollback-" + plan.nonce + ".exe";
			const std::string marker = relative + "/uam-runner.activation-" + plan.nonce;
			if (keep_new_runner)
				command = PowerShellCommand("$backup=Join-Path $HOME '" + backup +
				    "'; $marker=Join-Path $HOME '" + marker +
				    "'; Remove-Item -LiteralPath $backup,$marker -Force -ErrorAction SilentlyContinue");
			else
			{
				std::string script = "$installed=Join-Path $HOME '" + installed +
				    "'; $backup=Join-Path $HOME '" + backup +
				    "'; $marker=Join-Path $HOME '" + marker +
				    "'; if (Test-Path -LiteralPath $marker) { try { & $installed stop | Out-Null } catch {}; "
				    "if (Test-Path -LiteralPath $backup) { Copy-Item -LiteralPath $backup -Destination $installed -Force -ErrorAction Stop } "
				    "elseif (Test-Path -LiteralPath $installed) { Remove-Item -LiteralPath $installed -Force -ErrorAction Stop } }; "
				    "if (Test-Path -LiteralPath $marker) { Remove-Item -LiteralPath $marker -Force -ErrorAction Stop }; "
				    "Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue; ";
				command = wrap_powershell ? PowerShellCommand(script) : script;
			}
		}
		return command;
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

	std::vector<std::string> BuildBootstrapLockArgv(const BootstrapPlan& plan, std::string_view platform)
	{
		if (!uam::execution_hosts::IsSafeSshAlias(plan.ssh_alias) || !IsToken(plan.version, 64) || !IsToken(plan.nonce, 64) ||
		    !uam::execution_hosts::IsSafeRunnerDirectory(plan.runner_directory) || (platform != "linux" && platform != "windows")) return {};
		const std::string root = uam::execution_hosts::RunnerDirectory(platform, plan.runner_directory);
		const std::string rollback = BootstrapFinalizationCommand(plan, platform, false, false);
		std::string command;
		if (platform == "linux")
		{
			std::string quoted = "'";
			for (const char character : rollback) quoted += character == '\'' ? "'\\''" : std::string(1, character);
			quoted += "'";
			std::string quoted_trap = "'";
			for (const char character : "sh -c " + quoted) quoted_trap += character == '\'' ? "'\\''" : std::string(1, character);
			quoted_trap += "'";
			const std::string recover =
			    "journal=\"$root/install.transaction\"; if test -f \"$journal\"; then test ! -L \"$journal\"; "
			    "v=$(sed -n '1p' \"$journal\"); n=$(sed -n '2p' \"$journal\"); p=$(sed -n '3p' \"$journal\"); "
			    "case \"$v:$n\" in *[!a-zA-Z0-9_.:-]*|:*) echo 'Invalid helper transaction receipt.' >&2; exit 2;; esac; "
			    "case \"$v\" in .|..) exit 2;; esac; test -n \"$n\"; test \"${#v}\" -le 64; test \"${#n}\" -le 64; case \"$p\" in 2|3|4) :;; *) exit 2;; esac; "
			    "installed=\"$root/$v/uam-runner\"; backup=\"$root/$v/uam-runner.rollback-$n\"; marker=\"$root/$v/uam-runner.activation-$n\"; "
			    "if test -f \"$marker\"; then test ! -L \"$installed\"; test ! -L \"$backup\"; test ! -L \"$marker\"; "
			    "if test -f \"$installed\"; then \"$installed\" stop --socket \"$root/uam-$v-p$p.sock\" || true; fi; "
			    "if test -f \"$backup\"; then cp -p \"$backup\" \"$installed\"; \"$installed\" start --socket \"$root/uam-$v-p$p.sock\" >/dev/null; "
			    "else rm -f \"$installed\"; fi; rm -f \"$marker\" \"$backup\"; fi; rm -f \"$journal\"; fi; ";
			command = "set -eu; umask 077; root=\"$HOME/" + root + "\"; mkdir -p \"$root\"; "
			    "test ! -L \"$root/install.lock\"; exec 9>\"$root/install.lock\"; "
			    "if ! flock -n 9; then printf '%s\\n' 'The remote helper is busy with another installation.' >&2; exit 73; fi; " +
			    recover + "trap " + quoted_trap + " EXIT; printf '%s\\n' 'UAM_INSTALL_LOCK_READY:" + plan.nonce + "'; "
			    "while IFS= read -r line; do case \"$line\" in *[!a-zA-Z0-9+/=]*|'') exit 2;; esac; "
			    "command=$(printf '%s' \"$line\" | base64 -d); if output=$(sh -c \"$command\" 2>&1); then status=0; else status=$?; fi; "
			    "printf 'UAM_INSTALL_RESULT:%s:%s\\n' \"$status\" \"$(printf '%s' \"$output\" | base64 -w 0)\"; done";
		}
		else
		{
			const std::string recover =
			    "$journal=Join-Path $root 'install.transaction'; if (Test-Path -LiteralPath $journal) { "
			    "if ((Get-Item -LiteralPath $journal).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'The transaction receipt cannot be a link.' }; "
			    "$receipt=@(Get-Content -LiteralPath $journal); if ($receipt.Count -ne 3 -or $receipt[0] -cnotmatch '^[a-zA-Z0-9_.-]{1,64}$' -or "
			    "$receipt[0] -in @('.','..') -or $receipt[1] -cnotmatch '^[a-zA-Z0-9_.-]{1,64}$' -or $receipt[2] -notin @('2','3','4')) { throw 'Invalid helper transaction receipt.' }; "
			    "$directory=Join-Path $root $receipt[0]; $installed=Join-Path $directory 'uam-runner.exe'; "
			    "$backup=Join-Path $directory ('uam-runner.rollback-'+$receipt[1]+'.exe'); $marker=Join-Path $directory ('uam-runner.activation-'+$receipt[1]); "
			    "if (Test-Path -LiteralPath $marker) { foreach ($file in @($installed,$backup,$marker)) { if ((Test-Path -LiteralPath $file) -and "
			    "((Get-Item -LiteralPath $file).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Transaction files cannot be links.' } }; "
			    "if (Test-Path -LiteralPath $installed) { & $installed stop | Out-Null }; "
			    "if (Test-Path -LiteralPath $backup) { Copy-Item -LiteralPath $backup -Destination $installed -Force; & $installed start | Out-Null; "
			    "if ($LASTEXITCODE -ne 0) { throw 'The previous runner could not restart.' } } else { Remove-Item -LiteralPath $installed -Force -ErrorAction SilentlyContinue }; "
			    "Remove-Item -LiteralPath $marker,$backup -Force -ErrorAction SilentlyContinue }; Remove-Item -LiteralPath $journal -Force }; ";
			command = PowerShellUtf8Command("$ErrorActionPreference='Stop'; $root=Join-Path $HOME '" + root + "'; "
			    "New-Item -ItemType Directory -Path $root -Force | Out-Null; $path=Join-Path $root 'install.lock'; "
			    "if ((Test-Path -LiteralPath $path) -and ((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'The helper lock cannot be a link.' }; "
			    "try { $lock=[IO.File]::Open($path,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None) } "
			    "catch { throw 'The remote helper is busy with another installation.' }; "
			    "try { " + recover + "[Console]::WriteLine('UAM_INSTALL_LOCK_READY:" + plan.nonce + "'); "
			    "$release=Join-Path $root 'install.release-" + plan.nonce + "'; $deadline=[DateTime]::UtcNow.AddMinutes(10); "
			    "while (-not (Test-Path -LiteralPath $release) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 } } "
			    "finally { try { " + rollback + " } finally { $lock.Dispose(); "
			    "Remove-Item -LiteralPath (Join-Path $root 'install.release-" + plan.nonce + "') -Force -ErrorAction SilentlyContinue } }");
		}
		return SshCommand(plan.ssh_alias, std::move(command));
	}

	std::shared_ptr<BootstrapInstallLease> AcquireBootstrapInstallLease(const BootstrapPlan& plan, std::string_view platform,
	    std::string& error, std::stop_token stop_token)
	{
		const std::vector<std::string> argv = BuildBootstrapLockArgv(plan, platform);
		const std::optional<std::filesystem::path> cwd = uam::paths::CurrentPathNoThrow();
		if (argv.empty() || !cwd || stop_token.stop_requested())
		{
			error = stop_token.stop_requested() ? "Remote setup was canceled." : "The remote helper lock request is invalid.";
			return {};
		}
		const std::shared_ptr<BootstrapInstallLease> lease = std::make_shared<BootstrapInstallLease>();
		lease->separate_commands = platform == "windows";
		if (lease->separate_commands)
		{
			const std::string root = uam::execution_hosts::RunnerDirectory(platform, plan.runner_directory);
			lease->release_argv = SshCommand(plan.ssh_alias, PowerShellCommand(
			    "New-Item -ItemType File -Force -Path (Join-Path $HOME '" + root + "/install.release-" + plan.nonce + "') | Out-Null"));
		}
		if (!lease->service.StartStdioProcess(lease->process, *cwd, argv, &error)) return {};
		std::string output;
		std::string diagnostic;
		std::array<char, 4096> buffer{};
		const std::string ready = "UAM_INSTALL_LOCK_READY:" + plan.nonce + "\n";
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		for (;;)
		{
			bool read_failed = false;
			for (const bool stderr_output : {false, true})
			{
				const std::ptrdiff_t count = stderr_output
				    ? lease->service.ReadStdioProcessStderr(lease->process, buffer.data(), buffer.size(), &error)
				    : lease->service.ReadStdioProcessStdout(lease->process, buffer.data(), buffer.size(), &error);
				if (count == -1) read_failed = true;
				if (count > 0) (stderr_output ? diagnostic : output).append(buffer.data(), static_cast<std::size_t>(count));
			}
			if (output == ready || (platform == "windows" && output == ready.substr(0, ready.size() - 1) + "\r\n")) return lease;
			int status = -1;
			if (read_failed || output.size() + diagnostic.size() > 8192 || lease->service.PollStdioProcessExited(lease->process, &status) ||
			    stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline)
			{
				error = stop_token.stop_requested() ? "Remote setup was canceled." :
				    diagnostic.empty() ? "The remote helper installation lock could not be acquired." : uam::strings::Trim(diagnostic);
				lease->Release();
				return {};
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
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
			if (stop_token.stop_requested())
			{
				result.error = "Remote setup was canceled.";
				return result;
			}
			output.clear();
			diagnostic.clear();
			if (!RunStep(plan.steps[1], output, diagnostic, result.error, stop_token))
			{
				if (!stop_token.stop_requested() &&
				    result.error != kUnavailableWorkingDirectoryError)
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

		result.install_lease = AcquireBootstrapInstallLease(plan, result.platform, result.error, stop_token);
		if (!result.install_lease) return result;

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
			const std::string marker = directory + "/uam-runner.activation-" + plan.nonce;
			const std::string socket = "~/" + root + "/" +
			                           RunnerEndpointName(plan.version) + ".sock";
			const std::string socket_relative = root + "/" +
			                                    RunnerEndpointName(plan.version) + ".sock";
			const std::string validate_socket =
			    "set -eu; LC_ALL=C; export LC_ALL; socket=\"$HOME/" + socket_relative +
			    "\"; if test \"${#socket}\" -ge " +
			    std::to_string(uam::execution_hosts::kLinuxRunnerSocketPathCapacity) + "; then "
			    "printf '%s\\n' 'The configured helper folder makes the runner socket path too long.' >&2; "
			    "exit 2; fi";
			const std::string verify =
			    "set -eu; file=" + temporary + "; installed=" + installed +
			    "; backup=" + backup + "; marker=" + marker + "; trap 'rm -f \"$file\"' EXIT; "
			    "printf '%s  %s\\n' " + artifact->sha256 +
			    " \"$file\" | sha256sum -c -; chmod 700 \"$file\"; "
			    "if test -f \"$installed\"; then cp -p \"$installed\" \"$backup\"; fi; "
			    "\"$file\" stop --socket " + socket + "; printf '%s\\n' " + plan.version + " " + plan.nonce + " " +
			    std::to_string(kRunnerProtocolVersion) + " > ~/" + root + "/install.transaction; : > \"$marker\"; "
			    "if mv -f \"$file\" \"$installed\" && \"$installed\" start --socket " + socket +
			    " && test \"$(\"$installed\" --version)\" = " + plan.version +
			    " && test \"$(\"$installed\" --protocol-version)\" = " +
			    std::to_string(kRunnerProtocolVersion) +
			    "; then :; else status=$?; \"$installed\" stop --socket " + socket +
			    " || true; if test -f \"$backup\"; then "
			    "cp -p \"$backup\" \"$installed\"; else rm -f \"$installed\"; fi; "
			    "rm -f \"$marker\"; rm -f \"$backup\"; exit \"$status\"; fi";
			install_steps = {
			    {"Validate runner endpoint", SshCommand(plan.ssh_alias, validate_socket), ""},
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
			const std::string marker = relative + "/uam-runner.activation-" + plan.nonce;
			const std::string verify = PowerShellCommand(
			    "$file=Join-Path $HOME '" + temporary + "'; "
			    "$installed=Join-Path $HOME '" + installed + "'; $backup=Join-Path $HOME '" + backup + "'; " +
			    "$marker=Join-Path $HOME '" + marker + "'; "
			    "try { if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne '" +
			    artifact->sha256 + "') { throw 'Runner checksum mismatch.' }; "
			    "if (Test-Path -LiteralPath $installed) { Copy-Item -LiteralPath $installed -Destination $backup -Force -ErrorAction Stop }; "
			    "& $file stop | Out-Null; if ($LASTEXITCODE -ne 0) { throw 'Runner service is busy with active chats.' }; "
			    "[IO.File]::WriteAllLines((Join-Path $HOME '" + root + "/install.transaction'), @('" + plan.version + "','" + plan.nonce + "','" + std::to_string(kRunnerProtocolVersion) + "')); "
			    "New-Item -ItemType File -Path $marker -Force -ErrorAction Stop | Out-Null; "
			    "$moved=$false; for ($i=0; $i -lt 50 -and -not $moved; $i++) { "
			    "try { Move-Item -LiteralPath $file -Destination $installed -Force -ErrorAction Stop; $moved=$true } "
			    "catch { Start-Sleep -Milliseconds 100 } }; if (-not $moved) { throw 'Runner service did not release its executable.' }; "
			    "& $installed start; if ($LASTEXITCODE -ne 0) { throw 'Runner service could not start.' }; "
			    "if ((& $installed --version) -ne '" + plan.version + "') { throw 'Runner version verification failed.' }; "
			    "if ((& $installed --protocol-version) -ne '" + std::to_string(kRunnerProtocolVersion) + "') { throw 'Runner protocol verification failed.' }; "
			    "} catch { $failed=$_; if (Test-Path -LiteralPath $marker) { "
			    "try { & $installed stop | Out-Null } catch {}; "
			    "if (Test-Path -LiteralPath $backup) { Copy-Item -LiteralPath $backup -Destination $installed -Force -ErrorAction Stop } "
			    "elseif (Test-Path -LiteralPath $installed) { Remove-Item -LiteralPath $installed -Force -ErrorAction Stop }; "
			    "Remove-Item -LiteralPath $marker -Force -ErrorAction Stop; "
			    "Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue }; throw $failed } "
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
		bool activation_started = false;
		for (const BootstrapStep& step : install_steps)
		{
			if (!result.install_lease->Held(result.error)) return result;
			if (step.label == "Verify and activate runner") activation_started = true;
			output.clear();
			diagnostic.clear();
			if (!(step.label == "Copy runner" ? RunStep(step, output, diagnostic, result.error, stop_token) :
			      result.install_lease->Run(step, output, result.error, stop_token)))
			{
				if (activation_started)
				{
					std::string rollback_error;
					if (!FinalizeBootstrapPlan(plan, result, false, &rollback_error) &&
					    !rollback_error.empty())
						result.error += " Rollback failed: " + rollback_error;
				}
				return result;
			}
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

		if (stop_token.stop_requested()) return fail("Remote setup was canceled.");
		std::string lease_error;
		const std::shared_ptr<BootstrapInstallLease> lease = result.install_lease ? result.install_lease :
		    AcquireBootstrapInstallLease(plan, result.platform, lease_error, stop_token);
		if (!lease || !lease->Held(lease_error)) return fail(std::move(lease_error));
		const std::string command = BootstrapFinalizationCommand(plan, result.platform, keep_new_runner);
		BootstrapStep step{"Finalize runner activation", SshCommand(plan.ssh_alias, command), ""};
		std::string output;
		std::string diagnostic;
		std::string error;
		if (!lease->Run(step, output, error, stop_token)) return fail(std::move(error));
		lease->Release();
		return true;
	}
}

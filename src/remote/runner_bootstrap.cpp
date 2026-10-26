#include "remote/runner_bootstrap.h"
#include "common/utils/diagnostic_log.h"

#include "common/config/execution_host_config.h"
#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"
#include "common/paths/path_utils.h"
#include "common/utils/base64.h"
#include "common/utils/io_utils.h"
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
			// These generated scripts have no comments. Remove only whitespace at
			// statement/block boundaries, preserving quoted bytes and word separation.
			std::string compact;
			char quote = 0;
			for (std::size_t i = 0; i < script.size(); ++i)
			{
				const char c = script[i];
				if (quote)
				{
					compact += c;
					if ((c == '`' && quote == '"') || (c == '\'' && quote == '\'' && i + 1 < script.size() && script[i + 1] == '\''))
					{
						if (i + 1 < script.size()) compact += script[++i];
					}
					else if (c == quote) quote = 0;
				}
				else if (c == '\'' || c == '"') { quote = c; compact += c; }
				else if (std::isspace(static_cast<unsigned char>(c)))
				{
					while (i + 1 < script.size() && std::isspace(static_cast<unsigned char>(script[i + 1]))) ++i;
					if (!compact.empty() && i + 1 < script.size() && std::string_view("{};").find(compact.back()) == std::string_view::npos &&
					    std::string_view("{};").find(script[i + 1]) == std::string_view::npos) compact += ' ';
				}
				else compact += c;
			}
			return "powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass "
			       "-Command \"& ([scriptblock]::Create([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('" +
			       uam::base64::Encode(compact) + "'))))\"";
		}

		/// <summary>Replace mapped Windows images atomically; rollback always stages an unexecuted copy.</summary>
		std::string WindowsRunnerReplacementScript()
		{
			return "$ProgressPreference='SilentlyContinue'; "
			    "function Set-UamRunnerFile($src,$dst,$save) { $failure=$null; for ($i=0; $i -lt 50; $i++) { try { "
			    "if (Test-Path -LiteralPath $dst) { if ($save) { [IO.File]::Replace($src,$dst,$save) } "
			    "else { [IO.File]::Replace($src,$dst,[NullString]::Value) } } else { [IO.File]::Move($src,$dst) }; return "
			    "} catch { $failure=$_; Start-Sleep -Milliseconds 100 } }; throw $failure }; "
			    "function Restore-UamRunnerFile($old,$bin) { $restore=$old+'.restore'; "
			    "if ((Test-Path -LiteralPath $restore) -and ((Get-Item -LiteralPath $restore).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Unsafe restore link.' }; "
			    "Copy-Item -LiteralPath $old -Destination $restore -Force; Set-UamRunnerFile $restore $bin $null }; ";
		}

		std::string WindowsRunnerCommandScript()
		{
			return "$ErrorActionPreference='Stop'; "
			    "function Assert-UamPath($path,$directory=$false) { if (Test-Path -LiteralPath $path) { $item=Get-Item -LiteralPath $path -Force; "
			    "if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $item.PSIsContainer -ne $directory) { throw 'Unsafe helper transaction path.' } } }; "
			    "function Remove-UamOptional($paths) { foreach ($path in $paths) { if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force } } }; "
			    "function Get-UamTransactionPaths($d,$n) { \"$d/uam-runner.exe\"; \"$d/uam-runner.rollback-$n.exe\"; "
			    "\"$d/uam-runner.activation-$n\"; \"$d/uam-runner.metadata-$n\" }; "
			    "function Invoke-UamRunner($path,$argument,$failure) { $value=& $path $argument; if ($LASTEXITCODE -ne 0) { throw $failure }; $value }; ";
		}

		// Both guard recovery and finite commands hold this operation lock. Even if
		// the guard dies, its successor cannot recover while an old command mutates.
		std::string WindowsRunnerLeaseScript()
		{
			return "function Open-UamOperation($root,$wait=$false) { $p=Join-Path $root 'install.operation'; "
			    "if ((Test-Path -LiteralPath $p) -and ((Get-Item -LiteralPath $p -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Unsafe operation lock.' }; "
			    "do { try { return [IO.File]::Open($p,'OpenOrCreate','ReadWrite','None') } catch [IO.IOException] { "
			    "if (-not $wait) { throw }; Start-Sleep -Milliseconds 100 } } while ($true) }; ";
		}

		std::string WindowsRunnerOwnedScript(const std::string& root, const std::string& nonce, const std::string& script)
		{
			return "$ErrorActionPreference='Stop'; " + WindowsRunnerLeaseScript() + "$root=Join-Path $HOME '" + root + "'; $operation=Open-UamOperation $root; try { "
			    "$owner=[IO.File]::Open((Join-Path $root 'install.lock'),'Open','Read','ReadWrite'); try { "
			    "if ([IO.StreamReader]::new($owner).ReadToEnd() -cne '" + nonce + "') { throw 'Installation ownership was lost.' } "
			    "} finally { $owner.Dispose() }; " + script + " } finally { $operation.Dispose() }";
		}

		// Shared by activation, explicit finalization and the target-side guard. The
		// marker commits binary + metadata together; snapshots never attest a resident.
		std::string WindowsRunnerTransactionScript()
		{
			return WindowsRunnerReplacementScript() + WindowsRunnerCommandScript() +
			    "function Clear-UamMetadata($meta) { Assert-UamPath $meta $true; if (Test-Path -LiteralPath $meta) { "
			    "foreach ($name in @('uam-runner.manifest.json','uam-runner.sha256','ready')) { foreach ($suffix in @('','.new','.restore')) { "
			    "$path=Join-Path $meta ($name+$suffix); Assert-UamPath $path; Remove-UamOptional @($path) } }; [IO.Directory]::Delete($meta) } }; "
			    "function Restore-UamMetadata($bin,$meta) { Assert-UamPath $meta $true; $ready=Join-Path $meta 'ready'; Assert-UamPath $ready; "
			    "if (Test-Path -LiteralPath $ready) { foreach ($name in @('uam-runner.manifest.json','uam-runner.sha256')) { "
			    "$src=Join-Path $meta $name; $dst=Join-Path (Split-Path $bin) $name; Assert-UamPath $src; Assert-UamPath $dst; "
			    "if (Test-Path -LiteralPath $src) { Restore-UamRunnerFile $src $dst } else { Remove-UamOptional @($dst) } } } }; "
			    "function Complete-UamTransaction($old,$mark,$meta) { Assert-UamPath $old; Assert-UamPath $mark; "
			    "Remove-UamOptional @($mark); Remove-UamOptional @($old,($old+'.receipt')); Clear-UamMetadata $meta }; "
			    "function Undo-UamTransaction($bin,$old,$mark,$meta,$restart=$false) { "
			    "foreach ($path in @($bin,$old,$mark)) { Assert-UamPath $path }; if (Test-Path -LiteralPath $mark) { "
			    "if (Test-Path -LiteralPath $bin) { Invoke-UamRunner $bin stop 'Busy; rollback refused.' | Out-Null }; "
			    "if (Test-Path -LiteralPath $old) { Restore-UamRunnerFile $old $bin; Restore-UamMetadata $bin $meta; "
			    "if ($restart) { Invoke-UamRunner $bin start 'The prior runner could not restart.' | Out-Null } } "
			    "else { Remove-UamOptional @($bin); Restore-UamMetadata $bin $meta } }; Complete-UamTransaction $old $mark $meta }; ";
		}

		std::string WindowsRunnerIdentityScript()
		{
			return "function Test-UamRunner($path,$version,$protocol,$fingerprint) { "
			    "foreach ($pair in @(@('--version',$version),@('--protocol-version',$protocol),@('--source-fingerprint',$fingerprint))) { "
			    "$value=@(Invoke-UamRunner $path $pair[0] 'Runner identity query failed.'); "
			    "if ($value.Count -ne 1 -or $value[0] -cne $pair[1]) { throw ('Runner identity verification failed: '+$pair[0]) } } }; ";
		}

		std::string WindowsRunnerMetadataPreparationScript()
		{
			return "function New-UamMetadata($installed,$metadata,$fingerprint,$protocol,$checksum) { "
			    "New-Item -ItemType Directory -Path $metadata -ErrorAction Stop | Out-Null; "
			    "foreach ($name in @('uam-runner.manifest.json','uam-runner.sha256')) { $path=Join-Path (Split-Path $installed) $name; "
			    "Assert-UamPath $path; if (Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination (Join-Path $metadata $name) -ErrorAction Stop } }; "
			    "$manifest=@{sourceFingerprint=$fingerprint;protocolVersion=[int]$protocol} | ConvertTo-Json -Compress; "
			    "[IO.File]::WriteAllText((Join-Path $metadata 'uam-runner.manifest.json.new'),$manifest+\"`n\",[Text.Encoding]::ASCII); "
			    "[IO.File]::WriteAllText((Join-Path $metadata 'uam-runner.sha256.new'),$checksum+\"`n\",[Text.Encoding]::ASCII); "
			    "[IO.File]::WriteAllText((Join-Path $metadata 'ready'),'') }; ";
		}

		std::string WindowsRunnerMetadataPublicationScript()
		{
			return "function Publish-UamMetadata($installed,$metadata,$fingerprint,$protocol,$checksum) { Assert-UamPath $metadata $true; "
			    "foreach ($name in @('ready','uam-runner.manifest.json.new','uam-runner.sha256.new')) { Assert-UamPath (Join-Path $metadata $name) }; "
			    "if (-not (Test-Path -LiteralPath (Join-Path $metadata 'ready'))) { throw 'The helper metadata snapshot is unavailable.' }; "
			    "$manifest=[IO.File]::ReadAllText((Join-Path $metadata 'uam-runner.manifest.json.new')) | ConvertFrom-Json; "
			    "if ($manifest.sourceFingerprint -isnot [string] -or $manifest.sourceFingerprint -cne $fingerprint -or ($manifest.protocolVersion -isnot [int] -and $manifest.protocolVersion -isnot [long]) -or "
			    "$manifest.protocolVersion -ne [int]$protocol -or [IO.File]::ReadAllText((Join-Path $metadata 'uam-runner.sha256.new')) -cne ($checksum+\"`n\")) { throw 'Staged helper metadata mismatch.' }; "
			    "foreach ($name in @('uam-runner.manifest.json','uam-runner.sha256')) { "
			    "$target=Join-Path (Split-Path $installed) $name; Assert-UamPath $target; "
			    "Set-UamRunnerFile (Join-Path $metadata ($name+'.new')) $target $null } }; ";
		}

		// The ready receipt distinguishes an owned metadata snapshot from legacy binary-only
		// transactions. Restore absence as well as bytes; never synthesize old metadata.
		std::string LinuxRunnerMetadataScript()
		{
			return "uam_restore_runner() { test ! -L \"$backup.restore\"; rm -f \"$backup.restore\"; cp -p \"$backup\" \"$backup.restore\"; mv -f \"$backup.restore\" \"$installed\"; }; "
			    "uam_restore_metadata() { test ! -L \"$metadata\"; "
			    "if test -f \"$metadata/ready\"; then test ! -L \"$metadata/ready\"; "
			    "for name in uam-runner.manifest.json uam-runner.sha256; do target=\"${installed%/*}/$name\"; "
			    "test ! -L \"$target\"; test ! -L \"$metadata/$name\"; "
			    "if test -f \"$metadata/$name\"; then test ! -L \"$metadata/$name.restore\"; rm -f \"$metadata/$name.restore\"; cp -p \"$metadata/$name\" \"$metadata/$name.restore\"; "
			    "mv -f \"$metadata/$name.restore\" \"$target\"; else rm -f \"$target\"; fi; done; fi; }; ";
		}

		bool RunStep(const BootstrapStep& step, std::string& output, std::string& diagnostic,
		             std::string& error,
		             std::stop_token stop_token,
		             IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service)
		{
			if (stop_token.stop_requested())
			{
				error = "Remote setup was canceled.";
				return false;
			}
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
				// Bytes written before exit are already in the pipe, so one drain after observing exit keeps the final line.
				const bool exited = service.PollStdioProcessExited(process, &exit_code);
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
				if (exited) break;
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
		bool authorized = false;
		bool uncertain = false;
		std::uint64_t next_request = 1;
		std::string identity;
		std::string nonce;
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
			if (released || uncertain || service.PollStdioProcessExited(process, &status))
			{
				error = "The remote helper installation lock was lost. Retry setup after checking the host.";
				return false;
			}
			return true;
		}

		bool Run(const BootstrapStep& step, std::string& output, std::string& error, std::stop_token stop_token)
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (released || uncertain || !authorized || step.argv.empty()) { error = "The helper installation lock is unavailable."; return false; }
			// Windows OpenSSH buffers PowerShell input until EOF. Keep the OS lock
			// on its own connection and execute finite commands on separate connections.
			if (separate_commands)
			{
				std::string diagnostic;
				return RunStep(step, output, diagnostic, error, stop_token, service);
			}
			std::string command = step.argv.back();
			if (command.starts_with("powershell.exe "))
			{
				std::string utf16;
				if (!uam::base64::Decode(command.substr(command.find_last_of(' ') + 1), utf16)) return false;
				command.clear();
				for (std::size_t index = 0; index < utf16.size(); index += 2) command += utf16[index];
			}
			const std::string id = std::to_string(next_request++);
			const std::string request = id + ":" + uam::base64::Encode(command) + "\n";
			const auto invalidate = [&]() { uncertain = true; service.CloseStdioProcessInput(process); return false; };
			if (!service.WriteToStdioProcess(process, request.data(), request.size(), &error)) return invalidate();
			std::string received;
			std::string diagnostic;
			std::array<char, 4096> buffer{};
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
			for (;;)
			{
				// A guard that prints its result and exits at once must not lose that result.
				const bool exited = service.PollStdioProcessExited(process);
				bool read_any = false;
				for (const bool stderr_output : {false, true})
				{
					const std::ptrdiff_t count = stderr_output
					    ? service.ReadStdioProcessStderr(process, buffer.data(), buffer.size(), &error)
					    : service.ReadStdioProcessStdout(process, buffer.data(), buffer.size(), &error);
					if (count == -1) return invalidate();
					if (count <= 0) continue;
					read_any = true;
					(stderr_output ? diagnostic : received).append(buffer.data(), static_cast<std::size_t>(count));
				}
				const std::size_t newline = received.find('\n');
				if (newline != std::string::npos)
				{
					const std::string line = uam::strings::Trim(received.substr(0, newline));
					const std::string prefix = "UAM_INSTALL_RESULT:" + id + ":";
					const std::size_t status_end = line.find(':', prefix.size());
					if (!line.starts_with(prefix) || status_end == std::string::npos ||
					    !uam::base64::Decode(line.substr(status_end + 1), output) ||
					    received.size() != newline + 1)
					{
						error = "The guarded helper transaction returned an invalid or uncorrelated result.";
						return invalidate();
					}
					if (line.substr(prefix.size(), status_end - prefix.size()) != "0")
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
				if (received.size() + diagnostic.size() > 2 * 1024 * 1024 || (exited && !read_any) ||
				    stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline)
				{
					error = stop_token.stop_requested() ? "Remote setup was canceled." :
					    diagnostic.empty() ? "The guarded helper transaction disconnected or timed out." : uam::strings::Trim(diagnostic);
					return invalidate();
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
				RunStep({"Release helper installation lock", release_argv, ""}, output, diagnostic, error, {}, service);
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

	std::string WindowsHelperRetirementScript()
	{
		// Older helper roots keep their own Run value and resident service; activation leaves only the current one.
		// A helper with child processes (other than its console host) is busy even if it predates the stop refusal; the whole pass is bounded to stay inside the step deadline. The current root's Run value is
		// shared across its versions, so it is repointed rather than removed. Folders stay for explicit removal.
		// A same-root sibling reporting the current version shares the current pipe, so it is never sent stop.
		// ponytail: paths compare by full-path text; 8.3 or junction aliases of the current root are not recognised.
		return R"PS(try { $ErrorActionPreference='Stop'; $uamSelf=[IO.Path]::GetFullPath($uamCurrent); $uamSelfRoot=Split-Path (Split-Path $uamSelf); )PS"
		    R"PS($uamRoot=[IO.Path]::GetFullPath($uamHelperRoot).TrimEnd('\')+'\'; $uamTargets=@{}; )PS"
		    R"PS(function Get-UamServe($path) { @(Get-CimInstance Win32_Process -Filter "Name='uam-runner.exe'" | Where-Object { $_.ExecutablePath -and $_.CommandLine -match '\sserve\s*$' -and ([IO.Path]::GetFullPath($_.ExecutablePath) -ieq $path) }) }; )PS"
		    R"PS($uamRun=Get-Item -LiteralPath $uamRunKey -ErrorAction SilentlyContinue; )PS"
		    R"PS(if ($uamRun) { foreach ($name in $uamRun.GetValueNames()) { $value=[string]$uamRun.GetValue($name); if ($name -like 'uam-runner-*' -and $value -match '^"([^"]+\\uam-runner\.exe)" start$') { $path=[IO.Path]::GetFullPath($Matches[1]); if (-not $uamTargets.ContainsKey($path)) { $uamTargets[$path]=@() }; $uamTargets[$path]+=$name } } }; )PS"
		    R"PS(foreach ($process in @(Get-CimInstance Win32_Process -Filter "Name='uam-runner.exe'")) { if ($process.ExecutablePath -and $process.CommandLine -match '\sserve\s*$') { $path=[IO.Path]::GetFullPath($process.ExecutablePath); if (-not $uamTargets.ContainsKey($path)) { $uamTargets[$path]=@() } } }; )PS"
		    R"PS($uamClock=[Diagnostics.Stopwatch]::StartNew(); foreach ($path in @($uamTargets.Keys)) { if ($path -ieq $uamSelf -or -not $path.StartsWith($uamRoot,[StringComparison]::OrdinalIgnoreCase)) { continue }; )PS"
		    R"PS(if ($uamClock.Elapsed.TotalSeconds -gt 120) { 'UAM_HELPER_KEPT_TIMEOUT:'+$path; continue }; try { )PS"
		    R"PS(if (@(Get-UamServe $path | ForEach-Object { Get-CimInstance Win32_Process -Filter ('ParentProcessId='+$_.ProcessId+" AND Name<>'conhost.exe'") }).Count -gt 0) { 'UAM_HELPER_KEPT_CHILDREN:'+$path; continue }; $stopped=$true; $sameRoot=(Split-Path (Split-Path $path)) -ieq $uamSelfRoot; )PS"
		    R"PS(if ($sameRoot -and (Test-Path -LiteralPath $path) -and ((& $path --version) -eq (& $uamSelf --version))) { $shared=$true } else { $shared=$false }; )PS"
		    R"PS(if (-not $shared -and (Test-Path -LiteralPath $path)) { $stop=Start-Process -FilePath $path -ArgumentList 'stop' -WindowStyle Hidden -PassThru; $null=$stop.Handle; if (-not $stop.WaitForExit(20000)) { try { $stop.Kill() } catch {}; $stopped=$false } elseif ($stop.ExitCode -ne 0) { $stopped=$false } }; )PS"
		    R"PS(for ($i=0; -not $shared -and $stopped -and $i -lt 50 -and (Get-UamServe $path).Count -gt 0; $i++) { Start-Sleep -Milliseconds 100 }; if (-not $shared -and $stopped -and (Get-UamServe $path).Count -gt 0) { $stopped=$false }; )PS"
		    R"PS(if (-not $stopped) { 'UAM_HELPER_KEPT_BUSY:'+$path; continue }; )PS"
		    R"PS(foreach ($name in $uamTargets[$path]) { if ($sameRoot) { Set-ItemProperty -LiteralPath $uamRunKey -Name $name -Value ('"'+$uamSelf+'" start') } else { Remove-ItemProperty -LiteralPath $uamRunKey -Name $name } }; 'UAM_HELPER_RETIRED:'+$path )PS"
		    R"PS(} catch { 'UAM_HELPER_RETIREMENT_FAILED:'+$path+': '+$_.Exception.Message } } )PS"
		    R"PS(} catch { 'UAM_HELPER_RETIREMENT_FAILED:'+$_.Exception.Message }; $ErrorActionPreference='Continue'; )PS";
	}

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
			const std::string metadata = "~/" + relative + "/uam-runner.metadata-" + plan.nonce;
			if (keep_new_runner)
				// Removing the activation marker commits both the executable and its metadata.
				command = "set -eu; test -f " + marker + "; test ! -L " + marker + "; rm -f " + marker + "; printf '%s\\n' 'UAM_INSTALL_COMMITTED:" + plan.nonce + "'; rm -f " + backup + " " + backup + ".receipt; rm -rf " + metadata;
			else
			{
				command = "set -eu; " + LinuxRunnerMetadataScript() + "installed=" + installed + "; backup=" + backup +
				    "; marker=" + marker + "; metadata=" + metadata + "; if test -f \"$marker\"; then "
				    "if test -f \"$installed\"; then \"$installed\" stop --socket ~/" + root + "/" +
				    RunnerEndpointName(plan.version) +
				    ".sock 8>&-; fi; if test -f \"$backup\"; then uam_restore_runner; else rm -f \"$installed\"; fi; "
				    "uam_restore_metadata; fi; rm -f \"$marker\"; rm -f \"$backup\" \"$backup.receipt\"; rm -rf \"$metadata\"";
			}
		}
		else
		{
			const std::string script = "$directory=Join-Path $HOME '" + relative + "'; "
			    "$installed,$backup,$marker,$metadata=Get-UamTransactionPaths $directory '" + plan.nonce + "'; " +
			    (keep_new_runner ? "Assert-UamPath $marker; if (-not (Test-Path -LiteralPath $marker)) { throw 'Activation is not pending.' }; Remove-UamOptional @($marker); [Console]::WriteLine('UAM_INSTALL_COMMITTED:" + plan.nonce + "'); Complete-UamTransaction $backup $marker $metadata; "
			                       "$uamHelperRoot=$HOME; $uamRunKey='HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Run'; $uamCurrent=$installed; " + WindowsHelperRetirementScript() :
			                       "Undo-UamTransaction $installed $backup $marker $metadata");
			command = wrap_powershell ? PowerShellUtf8Command(WindowsRunnerOwnedScript(root, plan.nonce, WindowsRunnerTransactionScript() + script)) : script;
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
		for (RunnerArtifact& artifact : artifacts)
		{
			std::error_code status_error;
			if ((artifact.platform != "linux" && artifact.platform != "windows") ||
			    (artifact.architecture != "arm64" && artifact.architecture != "x86_64") ||
			    !IsSha256(artifact.sha256) ||
			    !std::filesystem::is_regular_file(artifact.path, status_error) || status_error)
				return fail("A packaged remote runner artifact is invalid.");
			std::string manifest_bytes;
			if (!uam::io::TryReadTextFile(artifact.path.parent_path() / "uam-runner.manifest.json", manifest_bytes, 4096))
				return fail("A packaged remote runner source manifest is missing or unreadable.");
			const nlohmann::json manifest = nlohmann::json::parse(manifest_bytes, nullptr, false);
			if (!manifest.is_object() || !manifest.contains("sourceFingerprint") ||
			    !manifest["sourceFingerprint"].is_string() ||
			    !IsSha256(manifest["sourceFingerprint"].get_ref<const std::string&>()) ||
			    !manifest.contains("protocolVersion") || !manifest["protocolVersion"].is_number_integer() ||
			    manifest["protocolVersion"] != kRunnerProtocolVersion)
				return fail("A packaged remote runner source manifest is invalid or incompatible.");
			artifact.source_fingerprint = manifest["sourceFingerprint"].get<std::string>();
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

	static std::string BootstrapRecoveryScript(std::string_view platform)
	{
		if (platform == "linux")
		{
			return
			    "journal=\"$root/install.transaction\"; if test -f \"$journal\"; then printf 'R\\n'; test ! -L \"$journal\"; "
			    "v=$(sed -n '1p' \"$journal\"); n=$(sed -n '2p' \"$journal\"); p=$(sed -n '3p' \"$journal\"); "
			    "case \"$v:$n\" in *[!a-zA-Z0-9_.:-]*|:*) echo 'Invalid helper transaction receipt.' >&2; exit 2;; esac; "
			    "case \"$v\" in .|..) exit 2;; esac; test -n \"$n\"; test \"${#v}\" -le 64; test \"${#n}\" -le 64; case \"$p\" in 2|3|4) :;; *) exit 2;; esac; "
			    "installed=\"$root/$v/uam-runner\"; backup=\"$root/$v/uam-runner.rollback-$n\"; marker=\"$root/$v/uam-runner.activation-$n\"; metadata=\"$root/$v/uam-runner.metadata-$n\"; "
			    "endpoint=\"$root/uam-$v-p$p.sock\"; if test \"$p\" = 2; then endpoint=\"$root/uam.sock\"; fi; "
			    "if test -f \"$marker\"; then test ! -L \"$installed\"; test ! -L \"$backup\"; test ! -L \"$marker\"; "
			    "if test -f \"$installed\"; then \"$installed\" stop --socket \"$endpoint\" 8>&-; fi; "
			    "if test -f \"$backup\"; then uam_restore_runner; uam_restore_metadata; \"$installed\" start --socket \"$endpoint\" >/dev/null 9>&- 8>&-; "
			    "else rm -f \"$installed\"; uam_restore_metadata; fi; rm -f \"$marker\" \"$backup\"; fi; rm -rf \"$metadata\"; rm -f \"$journal\"; fi; ";
		}
		else
		{
			return
			    "$journal=Join-Path $root 'install.transaction'; Assert-UamPath $journal; if (Test-Path -LiteralPath $journal) { [Console]::WriteLine('R'); "
			    "$receipt=@(Get-Content -LiteralPath $journal); if ($receipt.Count -ne 3 -or $receipt[0] -cnotmatch '^[a-zA-Z0-9_.-]{1,64}$' -or "
			    "$receipt[0] -in @('.','..') -or $receipt[1] -cnotmatch '^[a-zA-Z0-9_.-]{1,64}$' -or $receipt[2] -notin @('2','3','4')) { throw 'Invalid helper transaction receipt.' }; "
			    "$installed,$backup,$marker,$metadata=Get-UamTransactionPaths (Join-Path $root $receipt[0]) $receipt[1]; "
			    "Undo-UamTransaction $installed $backup $marker $metadata $true; Remove-UamOptional @($journal) }; ";
		}
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
			for (const char character : "sh -c " + quoted + " 9>&-") quoted_trap += character == '\'' ? "'\\''" : std::string(1, character);
			quoted_trap += "'";

			command = "set -eu; umask 077; " + LinuxRunnerMetadataScript() + "root=\"$HOME/" + root + "\"; mkdir -p \"$root\"; "
			    "test ! -L \"$root/install.lock\"; exec 9>\"$root/install.lock\"; "
			    "if ! flock -n 9; then echo UAM_INSTALL_BUSY; printf '%s\\n' 'The remote helper is busy with another installation.' >&2; exit 73; fi; " +
			    "test ! -L \"$root/install.operation\"; exec 8>\"$root/install.operation\"; "
			    "if ! flock -n 8; then echo UAM_INSTALL_BUSY; echo 'A helper operation is still active.' >&2; exit 73; fi; " +
			    "printf '%s\\n' 'UAM_INSTALL_LOCK_READY:" + plan.nonce + "'; "
			    "IFS= read -r authorization; test \"$authorization\" = 'UAM_INSTALL_AUTHORIZE:" + plan.nonce + "'; trap " + quoted_trap + " EXIT; "
			    "while IFS= read -r line; do id=${line%%:*}; line=${line#*:}; case \"$id\" in *[!0-9]*|'') exit 2;; esac; case \"$line\" in *[!a-zA-Z0-9+/=]*|'') exit 2;; esac; "
			    "command=$(printf '%s' \"$line\" | base64 -d); if output=$(sh -c \"$command\" 9>&- 2>&1); then status=0; else status=$?; fi; "
			    "printf 'UAM_INSTALL_RESULT:%s:%s:%s\\n' \"$id\" \"$status\" \"$(printf '%s' \"$output\" | base64 -w 0)\"; done";
		}
		else
		{

			command = PowerShellUtf8Command(WindowsRunnerTransactionScript() + WindowsRunnerLeaseScript() + "$root=Join-Path $HOME '" + root + "'; "
			    "[IO.Directory]::CreateDirectory($root)|Out-Null; $path=Join-Path $root 'install.lock'; Assert-UamPath $path; "
			    "try { $lock=[IO.File]::Open($path,'OpenOrCreate','ReadWrite','Read') } "
			    "catch { Write-Output 'UAM_INSTALL_BUSY'; exit 73 }; "
			    "try { $operation=Open-UamOperation $root $true; try { " +
			    "$lock.SetLength(0); $b=[Text.Encoding]::ASCII.GetBytes('" + plan.nonce + "'); $lock.Write($b,0,$b.Length); $lock.Flush() "
			    "} finally { $operation.Dispose() }; [Console]::WriteLine('UAM_INSTALL_LOCK_READY:" + plan.nonce + "'); "
			    "$release=Join-Path $root 'install.release-" + plan.nonce + "'; $deadline=[DateTime]::UtcNow.AddMinutes(10); "
			    "while (-not (Test-Path -LiteralPath $release) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 } } "
			    "finally { try { $operation=Open-UamOperation $root $true; try { $lock.SetLength(0); $lock.Flush(); "
			    "$authorization=Join-Path $root 'install.authorize-" + plan.nonce + "'; Assert-UamPath $authorization; if (Test-Path -LiteralPath $authorization) { " + rollback + "; Remove-UamOptional @($authorization) }; " +
			    " } finally { $operation.Dispose() } } finally { $lock.Dispose(); "
			    "if ($release -and (Test-Path -LiteralPath $release)) { Remove-Item -LiteralPath $release -Force -ErrorAction Stop } } }");
		}
		return SshCommand(plan.ssh_alias, std::move(command));
	}

	std::shared_ptr<BootstrapInstallLease> AcquireBootstrapInstallLease(const BootstrapPlan& plan, std::string_view platform,
	    std::string& error, std::stop_token stop_token, bool* recovery_required)
	{
		if (recovery_required) *recovery_required = false;
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
		if (recovery_required) *recovery_required = false;
		bool recovery_observed = false;
		std::string output;
		std::string diagnostic;
		std::array<char, 4096> buffer{};
		const std::string ready = "UAM_INSTALL_LOCK_READY:" + plan.nonce + "\n";
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		for (;;)
		{
			// Observe exit first so a busy or failed guard's final diagnostic is still read.
			int status = -1;
			const bool exited = lease->service.PollStdioProcessExited(lease->process, &status);
			bool read_failed = false;
			bool read_any = false;
			for (const bool stderr_output : {false, true})
			{
				const std::ptrdiff_t count = stderr_output
				    ? lease->service.ReadStdioProcessStderr(lease->process, buffer.data(), buffer.size(), &error)
				    : lease->service.ReadStdioProcessStdout(lease->process, buffer.data(), buffer.size(), &error);
				if (count == -1) read_failed = true;
				if (count <= 0) continue;
				read_any = true;
				(stderr_output ? diagnostic : output).append(buffer.data(), static_cast<std::size_t>(count));
			}
			if (output.starts_with("R\n")) { recovery_observed = true; output.erase(0, 2); }
			else if (output.starts_with("R\r\n")) { recovery_observed = true; output.erase(0, 3); }
			if (output == ready || (platform == "windows" && output == ready.substr(0, ready.size() - 1) + "\r\n"))
			{
				if (recovery_required) *recovery_required = recovery_observed;
				return lease;
			}
			if (read_failed || output.size() + diagnostic.size() > 8192 || (exited && !read_any) ||
			    stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline)
			{
				if (status == 73 && !recovery_observed && uam::strings::Trim(output) == "UAM_INSTALL_BUSY" && recovery_required) *recovery_required = false;
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

	BootstrapResult AcquireBootstrapPlan(const BootstrapPlan& plan, std::stop_token stop_token)
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
		if (!IsSha256(artifact->source_fingerprint))
		{
			result.error = "The packaged remote runner source fingerprint is invalid.";
			return result;
		}

		result.install_lease = AcquireBootstrapInstallLease(plan, result.platform, result.error, stop_token, &result.recovery_required);
		if (!result.install_lease) return result;
		result.install_lease->identity = BootstrapInstallIdentity(plan, result);
		result.install_lease->nonce = plan.nonce;
		result.ok = true;
		return result;
	}

	std::string BootstrapInstallIdentity(const BootstrapPlan& plan, const BootstrapResult& result)
	{
		const auto artifact = std::ranges::find_if(plan.artifacts, [&](const RunnerArtifact& value)
		{ return value.platform == result.platform && value.architecture == result.architecture; });
		if (artifact == plan.artifacts.end()) return {};
		return nlohmann::json::array({plan.ssh_alias, result.platform, result.architecture,
		    uam::execution_hosts::RunnerDirectory(result.platform, plan.runner_directory), plan.version,
		    kRunnerProtocolVersion, artifact->sha256, artifact->source_fingerprint}).dump();
	}

	bool RecoverBootstrapInstallation(const BootstrapPlan& plan, BootstrapResult& result,
	    std::string_view saved_nonce, std::string_view saved_identity, std::stop_token stop_token)
	{
		const auto lease = result.install_lease;
		if (!lease || saved_nonce != plan.nonce || saved_nonce != lease->nonce || saved_identity.empty() ||
		    saved_identity != lease->identity || saved_identity != BootstrapInstallIdentity(plan, result) || !lease->Held(result.error))
		{ result.error = "The durable helper installation fence is missing or changed."; return false; }
		std::string output, diagnostic;
		const std::string root = uam::execution_hosts::RunnerDirectory(result.platform, plan.runner_directory);
		// Only the acknowledged durable owner may arm target-side cleanup and recovery.
		if (lease->separate_commands)
		{
			const std::string authorize = PowerShellUtf8Command(WindowsRunnerOwnedScript(root, plan.nonce,
			    "$a=[IO.File]::Open((Join-Path $root 'install.authorize-" + plan.nonce + "'),'CreateNew','Write','None'); $a.Dispose()"));
			if (!RunStep({"Authorize helper recovery", SshCommand(plan.ssh_alias, authorize), ""}, output, diagnostic, result.error, stop_token)) return false;
		}
		else
		{
			const std::string authorize = "UAM_INSTALL_AUTHORIZE:" + plan.nonce + "\n";
			if (!lease->service.WriteToStdioProcess(lease->process, authorize.data(), authorize.size(), &result.error))
			{ lease->uncertain = true; lease->service.CloseStdioProcessInput(lease->process); return false; }
		}
		lease->authorized = true;
		result.recovery_required = true;
		std::string recovery = BootstrapRecoveryScript(result.platform);
		if (result.platform == "windows") recovery = PowerShellUtf8Command(WindowsRunnerOwnedScript(root, plan.nonce, WindowsRunnerTransactionScript() + recovery));
		else recovery = "set -eu; " + LinuxRunnerMetadataScript() + "root=\"$HOME/" + root + "\"; " + recovery;
		if (!lease->Run({"Recover helper installation", SshCommand(plan.ssh_alias, recovery), ""}, output, result.error, stop_token)) return false;
		result.recovery_required = !uam::strings::Trim(output).empty();
		return true;
	}

	BootstrapResult ExecuteBootstrapPlan(const BootstrapPlan& plan, BootstrapResult result,
	    std::string_view saved_nonce, std::string_view saved_identity, std::stop_token stop_token)
	{
		result.ok = false;
		if (!RecoverBootstrapInstallation(plan, result, saved_nonce, saved_identity, stop_token)) return result;
		std::string output, diagnostic;
		const auto artifact = std::ranges::find_if(plan.artifacts, [&](const RunnerArtifact& value)
		{ return value.platform == result.platform && value.architecture == result.architecture; });
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
			const std::string metadata = directory + "/uam-runner.metadata-" + plan.nonce;
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
			const std::string manifest = nlohmann::json{{"sourceFingerprint", artifact->source_fingerprint},
			    {"protocolVersion", kRunnerProtocolVersion}}.dump();
			const std::string verify =
			    "set -eu; umask 077; " + LinuxRunnerMetadataScript() + "file=" + temporary + "; installed=" + installed +
			    "; backup=" + backup + "; marker=" + marker + "; metadata=" + metadata + "; trap 'rm -f \"$file\"' EXIT; "
			    "printf '%s  %s\\n' " + artifact->sha256 +
			    " \"$file\" | sha256sum -c -; chmod 700 \"$file\"; "
			    "identity=$(\"$file\" --source-fingerprint 8>&-); test \"$identity\" = " + artifact->source_fingerprint + "; "
			    "identity=$(\"$file\" --version 8>&-); test \"$identity\" = " + plan.version + "; "
			    "identity=$(\"$file\" --protocol-version 8>&-); test \"$identity\" = " + std::to_string(kRunnerProtocolVersion) + "; "
			    "mkdir \"$metadata\"; for name in uam-runner.manifest.json uam-runner.sha256; do "
			    "target=\"${installed%/*}/$name\"; test ! -L \"$target\"; "
			    "if test -e \"$target\"; then test -f \"$target\"; cp -p \"$target\" \"$metadata/$name\"; fi; done; "
			    "printf '%s\\n' '" + manifest + "' > \"$metadata/uam-runner.manifest.json.new\"; "
			    "printf '%s\\n' " + artifact->sha256 + " > \"$metadata/uam-runner.sha256.new\"; : > \"$metadata/ready\"; "
			    "if test -f \"$installed\"; then cp -p \"$installed\" \"$backup\"; fi; "
			    "\"$file\" stop --socket " + socket + " 8>&-; test ! -L \"$backup.receipt\"; printf '%s\\n' " + plan.version + " " + plan.nonce + " " +
			    std::to_string(kRunnerProtocolVersion) + " > \"$backup.receipt\"; mv -f \"$backup.receipt\" ~/" + root + "/install.transaction; : > \"$marker\"; "
			    "if mv -f \"$file\" \"$installed\" && \"$installed\" start --socket " + socket +
			    " 8>&- && identity=$(\"$installed\" --version 8>&-) && test \"$identity\" = " + plan.version +
			    " && identity=$(\"$installed\" --protocol-version 8>&-) && test \"$identity\" = " +
			    std::to_string(kRunnerProtocolVersion) + " && identity=$(\"$installed\" --source-fingerprint 8>&-) && test \"$identity\" = " + artifact->source_fingerprint +
			    " && mv -f \"$metadata/uam-runner.manifest.json.new\" \"${installed%/*}/uam-runner.manifest.json\""
			    " && mv -f \"$metadata/uam-runner.sha256.new\" \"${installed%/*}/uam-runner.sha256\""
			    "; then :; else status=$?; if test -f \"$installed\"; then \"$installed\" stop --socket " + socket +
			    " 8>&-; fi; if test -f \"$backup\"; then "
			    "uam_restore_runner; else rm -f \"$installed\"; fi; "
			    "uam_restore_metadata; rm -f \"$marker\"; rm -f \"$backup\"; rm -rf \"$metadata\"; exit \"$status\"; fi";
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
			const std::string paths = "$directory=Join-Path $HOME '" + relative + "'; "
			    "$installed,$backup,$marker,$metadata=Get-UamTransactionPaths $directory '" + plan.nonce + "'; "
			    "$file=Join-Path $directory 'uam-runner.tmp-" + plan.nonce + ".exe'; ";
			const std::string checksum =
			    "if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -cne '" +
			    artifact->sha256 + "') { throw 'Runner checksum mismatch.' }; ";
			const std::string identity = "Test-UamRunner $file '" + plan.version + "' '" +
			    std::to_string(kRunnerProtocolVersion) + "' '" + artifact->source_fingerprint + "'; ";
			// Stage the snapshot on a finite connection before activation to keep every
			// Windows OpenSSH command below cmd.exe's limit. Each remains under the lease.
			const std::string prepare = PowerShellUtf8Command(WindowsRunnerOwnedScript(root, plan.nonce, WindowsRunnerCommandScript() + WindowsRunnerIdentityScript() +
			    WindowsRunnerMetadataPreparationScript() + paths +
			    "foreach ($path in @($file,$installed,$backup,$marker)) { Assert-UamPath $path }; " + checksum + identity +
			    "New-UamMetadata $installed $metadata '" + artifact->source_fingerprint + "' '" +
			    std::to_string(kRunnerProtocolVersion) + "' '" + artifact->sha256 + "'"));
			const std::string verify = PowerShellUtf8Command(WindowsRunnerOwnedScript(root, plan.nonce, WindowsRunnerTransactionScript() + paths + "try { " + checksum +
			    "Assert-UamPath $metadata $true; $ready=Join-Path $metadata 'ready'; Assert-UamPath $ready; "
			    "if (-not (Test-Path -LiteralPath $ready)) { throw 'The helper metadata snapshot is unavailable.' }; "
			    "if (Test-Path -LiteralPath $installed) { Copy-Item -LiteralPath $installed -Destination $backup -Force -ErrorAction Stop }; "
			    "Invoke-UamRunner $file stop 'Runner service is busy with active chats.' | Out-Null; "
			    "Assert-UamPath ($backup+'.receipt'); [IO.File]::WriteAllLines(($backup+'.receipt'), @('" + plan.version + "','" + plan.nonce + "','" + std::to_string(kRunnerProtocolVersion) + "')); "
			    "[IO.File]::Move(($backup+'.receipt'),(Join-Path (Split-Path $directory) 'install.transaction')); "
			    "New-Item -ItemType File -Path $marker -Force -ErrorAction Stop | Out-Null; "
			    "Set-UamRunnerFile $file $installed $backup; "
			    "Invoke-UamRunner $installed start 'Runner service could not start.' | Out-Null; "
			    "} catch { $failed=$_; try { Undo-UamTransaction $installed $backup $marker $metadata } "
			    "catch { throw ($failed.Exception.Message+' Rollback failed: '+$_.Exception.Message) }; throw $failed } "
			    "finally { Remove-UamOptional @($file) }"));
			const std::string publish = PowerShellUtf8Command(WindowsRunnerOwnedScript(root, plan.nonce, WindowsRunnerReplacementScript() + WindowsRunnerCommandScript() +
			    WindowsRunnerIdentityScript() + WindowsRunnerMetadataPublicationScript() + paths +
			    "$file=$installed; " + checksum + identity + "Publish-UamMetadata $installed $metadata '" +
			    artifact->source_fingerprint + "' '" + std::to_string(kRunnerProtocolVersion) + "' '" + artifact->sha256 + "'"));
			install_steps = {
			    {"Create private runner directory", SshCommand(plan.ssh_alias,
			        PowerShellUtf8Command(WindowsRunnerOwnedScript(root, plan.nonce,
			            "New-Item -ItemType Directory -Force -Path (Join-Path $HOME '" + relative + "') -ErrorAction Stop | Out-Null"))), ""},
			    {"Copy runner", {"scp", "-q", "-o", "BatchMode=yes", "-o",
			        "ConnectTimeout=10", artifact->path.string(), plan.ssh_alias + ":" + temporary}, ""},
			    {"Prepare runner metadata", SshCommand(plan.ssh_alias, prepare), ""},
			    {"Verify and activate runner", SshCommand(plan.ssh_alias, verify), ""},
			    {"Verify and publish runner metadata", SshCommand(plan.ssh_alias, publish), ""},
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
			if (step.label == "Verify and activate runner") result.activation_attempted = true;
			if (step.label == "Prepare runner metadata" || step.label == "Verify and activate runner") activation_started = true;
			output.clear();
			diagnostic.clear();
			if (!(step.label == "Copy runner" ? RunStep(step, output, diagnostic, result.error, stop_token) :
			      result.install_lease->Run(step, output, result.error, stop_token)))
			{
				if (activation_started)
				{
					std::string rollback_error;
					result.rollback_confirmed = FinalizeBootstrapPlan(plan, result, false, &rollback_error);
					if (!result.rollback_confirmed &&
					    !rollback_error.empty())
						result.error += " Rollback failed: " + rollback_error;
				}
				return result;
			}
		}
		result.ok = true;
		return result;
	}

	bool RunBootstrapStepForTests(const BootstrapStep& step, IPlatformProcessService& service, std::string& output, std::string& error)
	{
		std::string diagnostic;
		return RunStep(step, output, diagnostic, error, {}, service);
	}

	bool RunGuardedBootstrapStepForTests(const BootstrapStep& step, IPlatformProcessService& service, std::string& output, std::string& error)
	{
		BootstrapInstallLease lease{service};
		lease.authorized = true;
		return lease.Run(step, output, error, {});
	}

	bool FinalizeBootstrapPlan(const BootstrapPlan& plan, const BootstrapResult& result,
	                           bool keep_new_runner, std::string* error_out,
	                           std::stop_token stop_token, bool* committed_out)
	{
		if (committed_out) *committed_out = false;
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
		if (keep_new_runner && !result.install_lease) return fail("The installation lease is unavailable; commit cannot be confirmed.");
		std::string lease_error;
		const std::shared_ptr<BootstrapInstallLease> lease = result.install_lease;
		if (!lease || !lease->Held(lease_error)) return fail(std::move(lease_error));
		const std::string command = BootstrapFinalizationCommand(plan, result.platform, keep_new_runner);
		BootstrapStep step{"Finalize runner activation", SshCommand(plan.ssh_alias, command), ""};
		std::string output;
		std::string diagnostic;
		std::string error;
		const bool finalized = lease->Run(step, output, error, stop_token);
		std::istringstream receipt_lines(output);
		std::size_t receipts = 0;
		for (std::string line; std::getline(receipt_lines, line);)
			if (uam::strings::Trim(line) == "UAM_INSTALL_COMMITTED:" + plan.nonce) ++receipts;
		const bool committed = keep_new_runner && receipts == 1;
		if (committed_out) *committed_out = committed;
		if (!finalized) return fail(std::move(error));
		if (keep_new_runner && !committed) return fail("The remote helper commit could not be confirmed. Retry setup to reconcile it.");
		lease->Release();
		if (keep_new_runner && result.platform == "windows")
		{
			// Counts only: helper paths can contain private folder names.
			const auto count = [&output](std::string_view marker)
			{
				std::size_t found = 0;
				for (std::size_t at = output.find(marker); at != std::string::npos; at = output.find(marker, at + 1)) ++found;
				return found;
			};
			const std::size_t retired = count("UAM_HELPER_RETIRED:"), kept = count("UAM_HELPER_KEPT_"), failed = count("UAM_HELPER_RETIREMENT_FAILED:");
			if (retired + kept + failed > 0)
				uam::diagnostics::Write("Remote helper activation retired " + std::to_string(retired) + " older helper(s); kept " +
				                        std::to_string(kept) + " busy or unchecked; " + std::to_string(failed) + " retirement failure(s).");
		}
		return true;
	}
}

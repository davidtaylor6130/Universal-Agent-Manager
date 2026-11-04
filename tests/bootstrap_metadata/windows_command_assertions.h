#pragma once

#include "remote/runner_bootstrap.h"
#include "common/utils/base64.h"

#include <stdexcept>
#include <string>
#include <vector>

// Shared generator assertions: the core transport fixture and the isolated
// injected-process target must check the same Windows transaction contract.
namespace uam_test::bootstrap_command_checks
{
	inline void Require(bool condition, int line)
	{
		if (!condition) throw std::runtime_error("Windows bootstrap command contract failed at windows_command_assertions.h:" + std::to_string(line));
	}

	inline void WindowsLock(const uam::remote::BootstrapPlan& plan, const std::vector<std::string>& argv)
	{
		Require(!argv.empty(), __LINE__);
		Require(argv.back().size() < 8000, __LINE__);
		const std::string marker = "FromBase64String('";
		const std::size_t wrapper = argv.back().find(marker);
		Require(wrapper != std::string::npos, __LINE__);
		const std::size_t start = wrapper + marker.size();
		const std::size_t end = argv.back().find("'", start);
		Require(end != std::string::npos, __LINE__);
		std::string script;
		Require(uam::base64::Decode(argv.back().substr(start, end - start), script), __LINE__);
		Require(script.find("UAM_INSTALL_LOCK_READY:" + plan.nonce) != std::string::npos, __LINE__);
		Require(script.find("install.release-" + plan.nonce) != std::string::npos, __LINE__);
		Require(script.find("[Console]::WriteLine('UAM_INSTALL_LOCK_READY:") != std::string::npos, __LINE__);
		Require(script.find("finally{try{$operation=Open-UamOperation $root $true;try{$lock.SetLength(0)") != std::string::npos, __LINE__);
		Require(script.find("powershell.exe") == std::string::npos, __LINE__);
		Require(script.find("Restore-UamRunnerFile $old $bin;Restore-UamMetadata $bin $meta;") != std::string::npos, __LINE__);
		Require(script.find("if ($restart){Invoke-UamRunner $bin start") != std::string::npos, __LINE__);
		Require(script.find("Undo-UamTransaction $installed $backup $marker $metadata $true") == std::string::npos, __LINE__);
		Require(script.find("$journal=Join-Path") == std::string::npos, __LINE__);
		Require(script.find("Busy; rollback refused.") != std::string::npos, __LINE__);
		Require(script.find("Copy-Item -LiteralPath $backup -Destination $installed") == std::string::npos, __LINE__);
		const std::size_t helper = script.find("function Set-UamRunnerFile");
		Require(helper != std::string::npos, __LINE__);
		Require(script.find("function Set-UamRunnerFile", helper + 1) == std::string::npos, __LINE__);
	}

	inline void WindowsArtifact(const uam::remote::BootstrapPlan& plan, const std::string& decoded_commands)
	{
		Require(decoded_commands.find("Get-FileHash") != std::string::npos, __LINE__);
		Require(decoded_commands.find("Remove-UamOptional @($mark);Remove-UamOptional @($old,($old+'.receipt'));Clear-UamMetadata $meta") != std::string::npos, __LINE__);
		Require(decoded_commands.find("if (Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path -Force}") != std::string::npos, __LINE__);
		Require(decoded_commands.find("Remove-Item -LiteralPath $backup,$marker -Force -ErrorAction SilentlyContinue") == std::string::npos, __LINE__);
		Require(decoded_commands.find("Copy-Item -LiteralPath $installed -Destination $backup") !=
		           std::string::npos, __LINE__);
		Require(decoded_commands.find("New-Item -ItemType File -Path $marker") !=
		           std::string::npos, __LINE__);
		Require(decoded_commands.find("if (Test-Path -LiteralPath $mark)") !=
		           std::string::npos, __LINE__);
		Require(decoded_commands.find("Remove-UamOptional @($marker)") !=
		           std::string::npos, __LINE__);
		Require(decoded_commands.find("Copy-Item -LiteralPath $backup -Destination $installed") == std::string::npos, __LINE__);
		Require(decoded_commands.find("[IO.File]::Replace($src,$dst,$save)") != std::string::npos, __LINE__);
		Require(decoded_commands.find("[IO.File]::Replace($src,$dst,[NullString]::Value)") != std::string::npos, __LINE__);
		Require(decoded_commands.find("Copy-Item -LiteralPath $old -Destination $restore -Force") != std::string::npos, __LINE__);
		Require(decoded_commands.find("Restore-UamRunnerFile $old $bin") != std::string::npos, __LINE__);
		Require(decoded_commands.find("$failed.Exception.Message+' Rollback failed: '+$_.Exception.Message") != std::string::npos, __LINE__);
		Require(decoded_commands.find("Busy; rollback refused.") != std::string::npos, __LINE__);
		Require(decoded_commands.find("$ProgressPreference='SilentlyContinue'") != std::string::npos, __LINE__);
		const std::size_t stop_guard = decoded_commands.find("Runner service is busy with active chats.");
		Require(stop_guard < decoded_commands.find("Set-UamRunnerFile $file $installed $backup"), __LINE__);
		Require(decoded_commands.find("@('--protocol-version',$protocol)") != std::string::npos, __LINE__);
		Require(decoded_commands.find("@('--source-fingerprint',$fingerprint)") != std::string::npos, __LINE__);
		Require(decoded_commands.find("Test-UamRunner $file '" + plan.version + "'") != std::string::npos, __LINE__);
		Require(decoded_commands.find("$file=$installed;") != std::string::npos, __LINE__);
		Require(decoded_commands.find("New-UamMetadata $installed $metadata") != std::string::npos, __LINE__);
		Require(decoded_commands.find("Publish-UamMetadata $installed $metadata") != std::string::npos, __LINE__);
		Require(decoded_commands.find(plan.previous_runner_directory + "/" + plan.previous_version + "/uam-runner.exe") ==
		           std::string::npos, __LINE__);
		const auto rollback_stop = decoded_commands.find("Invoke-UamRunner $bin stop");
		const auto restore = decoded_commands.find("Restore-UamRunnerFile $old $bin");
		Require(rollback_stop != std::string::npos && restore != std::string::npos && rollback_stop < restore, __LINE__);
		const auto authorisation = decoded_commands.find("$a=[IO.File]::Open");
		const auto recovery = decoded_commands.find("$journal=Join-Path");
		Require(authorisation != std::string::npos && recovery != std::string::npos && authorisation < recovery, __LINE__);
		Require(decoded_commands.find("Undo-UamTransaction $installed $backup $marker $metadata $true", recovery) != std::string::npos, __LINE__);
		Require(decoded_commands.find("Previous runner") == std::string::npos, __LINE__);
		Require(decoded_commands.find("Start-Sleep -Milliseconds 100") != std::string::npos, __LINE__);
		Require(decoded_commands.find("finally{Remove-UamOptional @($file)}") != std::string::npos, __LINE__);
		Require(decoded_commands.find(plan.runner_directory + "/" + plan.version + "/uam-runner.exe") != std::string::npos, __LINE__);
		Require(decoded_commands.find("Join-Path $HOME '" + plan.runner_directory + "/" + plan.version + "/uam-runner.exe'") !=
		           std::string::npos, __LINE__);
		Require(decoded_commands.find("'$HOME") == std::string::npos, __LINE__);
	}
}

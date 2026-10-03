#pragma once

#include "common/config/execution_host_config.h"
#include "common/utils/base64.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace uam::remote
{
	/// <summary>Build an authenticated interactive workspace shell without interpreting controller paths.</summary>
	inline std::vector<std::string> BuildRemoteWorkspaceTerminalArgv(const ExecutionHost& host, std::string_view directory)
	{
		if (host.id == uam::execution_hosts::kLocalHostId || !uam::execution_hosts::IsSafeSshAlias(host.ssh_alias) ||
		    !uam::execution_hosts::IsAbsoluteRemotePath(host.platform, directory)) return {};
		std::string command;
		if (host.platform == "linux")
		{
			std::string quoted = "'";
			for (const char character : directory) quoted += character == '\'' ? "'\\''" : std::string(1, character);
			quoted += "'";
			command = "cd -- " + quoted + " && exec \"${SHELL:-/bin/sh}\" -l";
		}
		else if (host.platform == "windows")
		{
			const std::string script = "$ErrorActionPreference='Stop'; $directory=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('" +
			    uam::base64::Encode(directory) + "')); Set-Location -LiteralPath $directory;";
			std::string utf16;
			for (const char character : script) { utf16 += character; utf16 += '\0'; }
			command = "powershell.exe -NoLogo -NoProfile -NoExit -EncodedCommand " + uam::base64::Encode(utf16);
		}
		else return {};
		return {"ssh", "-tt", "-o", "BatchMode=yes", "-o", "ClearAllForwardings=yes", "-o", "ConnectTimeout=10", host.ssh_alias, command};
	}

	/// <summary>Encode native argv for a new Windows PowerShell window without cmd.exe interpolation.</summary>
	inline std::string WindowsTerminalEncodedCommand(const std::vector<std::string>& argv)
	{
		const std::string script = "$a=ConvertFrom-Json ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('" +
		    uam::base64::Encode(nlohmann::json(argv).dump()) + "'))); $arguments=@($a | Select-Object -Skip 1); & $a[0] @arguments;";
		std::string utf16;
		for (const char character : script) { utf16 += character; utf16 += '\0'; }
		return uam::base64::Encode(utf16);
	}
}

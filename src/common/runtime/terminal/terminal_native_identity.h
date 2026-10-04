#pragma once

#include "common/provider/provider_ids.h"
#include "common/runtime/terminal/terminal_idle_classifier.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/utils/string_utils.h"
#include "common/utils/uuid.h"

#include <string>
#include <string_view>

namespace uam
{
	/// <summary>Terminal capability/cursor replies are transport input, not deliberate user interaction.</summary>
	inline bool IsNativeTerminalResponse(std::string_view bytes)
	{
		if (bytes.starts_with("\x1b[" ) && bytes.size() >= 4 &&
		    (bytes.back() == 'R' || bytes.back() == 'c' || bytes.back() == 'u'))
		{
			for (const char character : bytes.substr(2, bytes.size() - 3))
				if ((character < '0' || character > '9') && character != ';' && character != '?' && character != '>') return false;
			return true;
		}
		if (!bytes.starts_with("\x1b]10;rgb:") && !bytes.starts_with("\x1b]11;rgb:")) return false;
		const std::size_t suffix = bytes.ends_with("\x1b\\") ? 2 : bytes.ends_with("\a") ? 1 : 0;
		if (suffix == 0) return false;
		for (const char character : bytes.substr(9, bytes.size() - 9 - suffix))
			if ((character < '0' || character > '9') && (character < 'a' || character > 'f') &&
			    (character < 'A' || character > 'F') && character != '/') return false;
		return true;
	}

	/// <summary>Reads only the correlated native status panel, never an arbitrary UUID from conversation output.</summary>
	inline std::string NativeStatusSessionId(std::string_view provider_id, std::string_view output)
	{
		const std::string text = StripTerminalControlSequencesForLifecycle(output);
		const bool codex = provider_id == provider_ids::kCodexCli;
		const bool gemini = provider_id == provider_ids::kGeminiCli;
		if ((!codex && !gemini) ||
		    (codex && (text.find("OpenAI Codex") == std::string::npos || text.find("Token usage:") == std::string::npos)) ||
		    (gemini && (text.find("Session Stats") == std::string::npos || text.find("Interaction Summary") == std::string::npos))) return {};
		const std::string_view label = codex ? "Session:" : "Session ID:";
		std::string identity;
		std::size_t begin = 0;
		while ((begin = text.find(label, begin)) != std::string::npos)
		{
			begin += label.size();
			while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
			const std::string candidate = text.substr(begin, 36);
			if (!uuid::IsCanonicalUuid(candidate)) return {};
			const std::size_t end = begin + candidate.size();
			if (end < text.size() && text[end] != ' ' && text[end] != '\n' && text[end] != '\t') return {};
			if (!identity.empty() && identity != candidate) return {};
			identity = candidate;
		}
		return identity;
	}

	/// <summary>Uses the verified minimum native version; later replies must still match the bounded status panel contract.</summary>
	inline std::string NativeSessionStatusCommand(std::string_view provider_id, std::string_view version)
	{
		if (provider_id == provider_ids::kCodexCli && CliProviderVersionAtLeast(version, "0.159.2")) return "/status";
		if (provider_id == provider_ids::kGeminiCli && CliProviderVersionAtLeast(version, "0.38.1")) return "/stats session";
		return {};
	}
}

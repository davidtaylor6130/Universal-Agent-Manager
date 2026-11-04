#pragma once

#include "common/provider/provider_ids.h"
#include "common/utils/string_utils.h"
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace uam::native_agents
{
struct HistoryReference
{
	std::string provider_id;
	std::string parent_session_id;
	std::string agent_id;
	std::string execution_host_id;
};

inline std::string_view ReferencePrefix(std::string_view provider, bool compact = false)
{
	if (provider == provider_ids::kCopilotCli) return compact ? "copilot-a32-" : "copilot-agent-";
	if (provider == provider_ids::kClaudeCli) return compact ? "claude-a32-" : "claude-agent-";
	if (provider == provider_ids::kGeminiCli) return compact ? "gemini-a32-" : "gemini-agent-";
	return {};
}

/// A read-only UAM reference, never a provider resume identity.
inline std::string BuildHistoryId(std::string_view provider, std::string_view parent, std::string_view agent, std::string_view host)
{
	const auto prefix = ReferencePrefix(provider);
	if (prefix.empty() || parent.empty() || parent.size() > 128 || parent.front() == '-' ||
		!std::ranges::all_of(parent, [](unsigned char byte) { return strings::IsAsciiAlnum(byte) || byte == '-' || byte == '_'; }) ||
		agent.empty() || agent.size() > 128 || host.empty() || host.size() > 128 || agent.find('\0') != agent.npos || host.find('\0') != host.npos) return {};
	const auto hex = [](std::string_view input)
	{
		constexpr char digits[] = "0123456789abcdef";
		std::string result;
		for (const unsigned char byte : input)
		{
			result.push_back(digits[byte >> 4]);
			result.push_back(digits[byte & 15]);
		}
		return result;
	};
	std::string reference = std::string(prefix) + hex(parent) + "-" + hex(agent) + "-" + hex(host);
	if (reference.size() <= 240) return reference; // Preserve all existing short references byte for byte.
	const auto base32 = [](std::string_view input)
	{
		constexpr char digits[] = "abcdefghijklmnopqrstuvwxyz234567";
		std::string result;
		unsigned int accumulator = 0, bits = 0;
		for (const unsigned char byte : input)
		{
			accumulator = (accumulator << 8) | byte;
			bits += 8;
			while (bits >= 5)
			{
				bits -= 5;
				result.push_back(digits[(accumulator >> bits) & 31]);
			}
			accumulator &= (1u << bits) - 1;
		}
		if (bits) result.push_back(digits[(accumulator << (5 - bits)) & 31]);
		return result;
	};
	reference = std::string(ReferencePrefix(provider, true)) + base32(parent) + "-" + base32(agent) + "-" + base32(host);
	return reference.size() <= 240 ? reference : std::string{};
}

inline std::optional<HistoryReference> ParseHistoryId(std::string_view reference)
{
	std::string_view provider;
	bool compact = false;
	for (const auto candidate : {provider_ids::kCopilotCli, provider_ids::kClaudeCli, provider_ids::kGeminiCli})
	{
		if (reference.starts_with(ReferencePrefix(candidate))) { provider = candidate; break; }
		if (reference.starts_with(ReferencePrefix(candidate, true))) { provider = candidate; compact = true; break; }
	}
	if (provider.empty() || reference.size() > 240) return std::nullopt;
	const std::string original(reference);
	reference.remove_prefix(ReferencePrefix(provider, compact).size());
	const auto unhex = [](std::string_view input) -> std::optional<std::string>
	{
		if (input.empty() || input.size() % 2 != 0) return std::nullopt;
		const auto digit = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1; };
		std::string result;
		for (std::size_t index = 0; index < input.size(); index += 2)
		{
			const int high = digit(input[index]), low = digit(input[index + 1]);
			if (high < 0 || low < 0 || (high == 0 && low == 0)) return std::nullopt;
			result.push_back(static_cast<char>(high * 16 + low));
		}
		return result;
	};
	const auto unbase32 = [](std::string_view input) -> std::optional<std::string>
	{
		if (input.empty()) return std::nullopt;
		std::string result;
		unsigned int accumulator = 0, bits = 0;
		for (const unsigned char ch : input)
		{
			const int digit = ch >= 'a' && ch <= 'z' ? ch - 'a' : ch >= '2' && ch <= '7' ? ch - '2' + 26 : -1;
			if (digit < 0) return std::nullopt;
			accumulator = (accumulator << 5) | static_cast<unsigned int>(digit);
			bits += 5;
			if (bits >= 8)
			{
				bits -= 8;
				const unsigned char byte = static_cast<unsigned char>(accumulator >> bits);
				if (byte == 0) return std::nullopt;
				result.push_back(static_cast<char>(byte));
			}
			accumulator &= (1u << bits) - 1;
		}
		if (accumulator != 0) return std::nullopt;
		return result;
	};
	const auto first = reference.find('-');
	const auto second = first == reference.npos ? reference.npos : reference.find('-', first + 1);
	if (first == reference.npos || second == reference.npos || reference.find('-', second + 1) != reference.npos) return std::nullopt;
	const auto decode = [&](std::string_view input) { return compact ? unbase32(input) : unhex(input); };
	const auto parent = decode(reference.substr(0, first));
	const auto agent = decode(reference.substr(first + 1, second - first - 1));
	const auto host = decode(reference.substr(second + 1));
	if (!parent || !agent || !host || BuildHistoryId(provider, *parent, *agent, *host) != original) return std::nullopt;
	return HistoryReference{std::string(provider), *parent, *agent, *host};
}
}

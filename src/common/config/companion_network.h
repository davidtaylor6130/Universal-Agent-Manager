#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <string>
#include <string_view>
#include <vector>

namespace uam::companion
{
	/// <summary>Updates a managed IP-based HTTPS proxy without changing tokens or certificate storage.</summary>
	inline bool RepairLanAddress(nlohmann::json& config, nlohmann::json& proxy,
	                             const std::string& address, const std::vector<std::string>& local_addresses)
	{
		const auto private_ipv4 = [](std::string_view value)
		{
			std::array<int, 4> octets{};
			for (std::size_t index = 0; index < octets.size(); ++index)
			{
				const std::size_t dot = value.find('.');
				const std::string_view part = value.substr(0, dot);
				const std::from_chars_result parsed = std::from_chars(part.data(), part.data() + part.size(), octets[index]);
				if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || octets[index] < 0 || octets[index] > 255) return false;
				if (index == 3) { if (dot != std::string_view::npos) return false; }
				else { if (dot == std::string_view::npos) return false; value.remove_prefix(dot + 1); }
			}
			return octets[0] == 10 || (octets[0] == 192 && octets[1] == 168) ||
			    (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31);
		};
		if (std::find(local_addresses.begin(), local_addresses.end(), address) == local_addresses.end() || !private_ipv4(address) ||
		    !config.is_object() || !config.contains("origin") || !config["origin"].is_string() ||
		    !proxy.is_object() || !proxy.contains("apps") || !proxy["apps"].is_object() ||
		    !proxy["apps"].contains("http")) return false;
		const std::string origin = config["origin"].get<std::string>();
		const std::size_t colon = origin.rfind(':');
		if (!origin.starts_with("https://") || colon == std::string::npos || colon <= 8) return false;
		const std::string previous = origin.substr(8, colon - 8);
		const std::string endpoint = origin.substr(colon);
		unsigned int port = 0;
		const std::from_chars_result parsed = std::from_chars(endpoint.data() + 1, endpoint.data() + endpoint.size(), port);
		if (!private_ipv4(previous) || parsed.ec != std::errc{} || parsed.ptr != endpoint.data() + endpoint.size() || port == 0 || port > 65535) return false;
		bool has_host = false;
		bool has_listener = false;
		const auto inspect = [&](const auto& self, const nlohmann::json& value) -> void
		{
			if (value.is_string())
			{
				has_host = has_host || value == previous;
				has_listener = has_listener || value == previous + endpoint;
			}
			else if (value.is_structured()) for (const nlohmann::json& child : value) self(self, child);
		};
		inspect(inspect, proxy["apps"]["http"]);
		if (!has_host || !has_listener) return false;
		const auto replace = [&](const auto& self, nlohmann::json& value) -> void
		{
			if (value.is_string())
			{
				if (value == previous) value = address;
				else if (value == previous + endpoint) value = address + endpoint;
			}
			else if (value.is_structured()) for (nlohmann::json& child : value) self(self, child);
		};
		replace(replace, proxy["apps"]["http"]);
		if (proxy["apps"].contains("tls")) replace(replace, proxy["apps"]["tls"]);
		config["origin"] = "https://" + address + endpoint;
		return true;
	}
}

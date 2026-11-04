#pragma once
#include "common/companion/request_transfer.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace uam::companion
{
	// Share only the configured proxy's public CA. No keys or arbitrary certificate paths
	// come from the phone. The native client validates the resulting chain and hostname.
	inline std::string ReadCompanionPairingCertificate(const nlohmann::json& config)
	{
		const auto read_bounded = [](const std::filesystem::path& path, std::size_t limit) -> std::string {
			std::error_code error;
			if (std::filesystem::file_size(path, error) > limit || error) return {};
			std::ifstream stream(path, std::ios::binary);
			if (!stream) return {};
			std::string data(limit + 1, '\0');
			stream.read(data.data(), static_cast<std::streamsize>(data.size()));
			const auto count = static_cast<std::size_t>(stream.gcount());
			if (count > limit) return {};
			data.resize(count);
			return data;
		};
		if (!config.contains("proxy_config") || !config["proxy_config"].is_string()) return {};
		const auto proxy = nlohmann::json::parse(read_bounded(config["proxy_config"].get<std::string>(), 1024 * 1024), nullptr, false);
		if (!proxy.is_object() || !proxy.contains("storage") || !proxy["storage"].is_object()) return {};
		const auto& storage = proxy["storage"];
		if (!storage.contains("module") || !storage["module"].is_string() || storage["module"] != "file_system" || !storage.contains("root") || !storage["root"].is_string()) return {};
		const auto text = read_bounded(std::filesystem::path(storage["root"].get<std::string>()) / "pki/authorities/local/root.crt", 16 * 1024);
		constexpr std::string_view begin = "-----BEGIN CERTIFICATE-----", end = "-----END CERTIFICATE-----";
		const auto first = text.find(begin), last = text.find(end);
		if (first != 0 || last == std::string::npos || text.find(begin, begin.size()) != std::string::npos) return {};
		std::string encoded;
		for (std::size_t index = begin.size(); index < last; ++index)
		{
			const char ch = text[index];
			if (ch != '\r' && ch != '\n' && ch != ' ' && ch != '\t') encoded += ch;
		}
		const auto size = Base64DecodedSize(encoded);
		return size && *size > 0 && *size <= 8192 ? encoded : std::string{};
	}
}

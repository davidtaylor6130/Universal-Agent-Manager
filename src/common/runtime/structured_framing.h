#pragma once

#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>

namespace uam::structured_framing
{

	enum class Status { Incomplete, Complete, Invalid };

	struct Frame
	{
		Status status = Status::Incomplete;
		std::size_t body_offset = 0;
		std::size_t body_bytes = 0;
		std::size_t consumed_bytes = 0;
	};

	inline bool EqualAsciiIgnoreCase(std::string_view first, std::string_view second)
	{
		if (first.size() != second.size()) return false;
		for (std::size_t index = 0; index < first.size(); ++index)
		{
			const auto lower = [](unsigned char ch) { return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch; };
			if (lower(first[index]) != lower(second[index])) return false;
		}
		return true;
	}

	/// Parses one SDK/LSP frame without consuming a partial header or body.
	inline Frame Read(std::string_view input, std::size_t max_body_bytes = 64 * 1024 * 1024)
	{
		constexpr std::size_t max_header_bytes = 8192;
		const std::size_t end = input.find("\r\n\r\n");
		if (end == std::string_view::npos) return {input.size() > max_header_bytes ? Status::Invalid : Status::Incomplete};
		if (end > max_header_bytes) return {Status::Invalid};
		bool has_length = false;
		std::size_t length = 0;
		std::size_t position = 0;
		while (position < end)
		{
			std::size_t line_end = input.find("\r\n", position);
			if (line_end == std::string_view::npos || line_end > end) return {Status::Invalid};
			const std::string_view line = input.substr(position, line_end - position);
			const std::size_t colon = line.find(':');
			if (colon == std::string_view::npos || colon == 0) return {Status::Invalid};
			if (EqualAsciiIgnoreCase(line.substr(0, colon), "Content-Length"))
			{
				if (has_length) return {Status::Invalid};
				std::string_view value = line.substr(colon + 1);
				while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
				while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
				if (value.empty()) return {Status::Invalid};
				const auto parsed = std::from_chars(value.data(), value.data() + value.size(), length);
				if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || length == 0 || length > max_body_bytes) return {Status::Invalid};
				has_length = true;
			}
			position = line_end + 2;
		}
		if (!has_length) return {Status::Invalid};
		const std::size_t offset = end + 4;
		if (input.size() - offset < length) return {Status::Incomplete, offset, length, 0};
		return {Status::Complete, offset, length, offset + length};
	}

	inline std::string Encode(std::string_view body)
	{
		return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
	}
}

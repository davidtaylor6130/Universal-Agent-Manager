#pragma once

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace bridge
{
	inline std::string Trim(const std::string& value)
	{
		const auto start = value.find_first_not_of(" \t\r\n");

		if (start == std::string::npos)
		{
			return "";
		}

		const auto end = value.find_last_not_of(" \t\r\n");
		return value.substr(start, end - start + 1);
	}

	inline std::string ToLowerAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		return value;
	}

	inline bool EqualsIgnoreCase(const std::string& lhs, const std::string& rhs)
	{
		return ToLowerAscii(lhs) == ToLowerAscii(rhs);
	}

	inline bool WriteTextFile(const std::filesystem::path& path, const std::string& text, std::string* error_out = nullptr)
	{
		std::error_code ec;
		std::filesystem::create_directories(path.parent_path(), ec);

		if (ec)
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to create directory for file '" + path.string() + "': " + ec.message();
			}

			return false;
		}

		std::ofstream out(path, std::ios::binary | std::ios::trunc);

		if (!out.good())
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to open file for writing: " + path.string();
			}

			return false;
		}

		out << text;

		if (!out.good())
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to write file: " + path.string();
			}

			return false;
		}

		return true;
	}

	inline std::string ReadTextFile(const std::filesystem::path& path)
	{
		std::ifstream in(path, std::ios::binary);

		if (!in.good())
		{
			return "";
		}

		std::ostringstream buffer;
		buffer << in.rdbuf();
		return buffer.str();
	}

	inline std::int64_t UnixEpochSecondsNow()
	{
		const auto now = std::chrono::system_clock::now();
		return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
	}

	inline std::string RandomHex(const std::size_t digits)
	{
		static thread_local std::mt19937 rng(std::random_device{}());
		std::uniform_int_distribution<int> nibble(0, 15);
		std::string out;
		out.reserve(digits);

		for (std::size_t i = 0; i < digits; ++i)
		{
			const int value = nibble(rng);
			out.push_back(static_cast<char>(value < 10 ? ('0' + value) : ('a' + (value - 10))));
		}

		return out;
	}

	inline std::string BuildId(const std::string& prefix)
	{
		const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		return prefix + "-" + std::to_string(now_ms) + "-" + RandomHex(8);
	}

	inline int CountApproxTokens(const std::string& text)
	{
		std::istringstream in(text);
		int count = 0;
		std::string token;

		while (in >> token)
		{
			++count;
		}

		return std::max(1, count);
	}

	inline bool ParseInt(const std::string& text, int* value_out)
	{
		if (value_out == nullptr)
		{
			return false;
		}

		char* end = nullptr;
		errno = 0;
		const long parsed = std::strtol(text.c_str(), &end, 10);

		if (end == nullptr || *end != '\0' || errno != 0 || parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max())
		{
			return false;
		}

		*value_out = static_cast<int>(parsed);
		return true;
	}

	inline std::string ReplaceAll(std::string value, const std::string& from, const std::string& to)
	{
		if (from.empty())
		{
			return value;
		}

		std::size_t start = 0;

		while ((start = value.find(from, start)) != std::string::npos)
		{
			value.replace(start, from.size(), to);
			start += to.size();
		}

		return value;
	}

	inline std::string ShellSingleQuote(const std::string& value)
	{
		std::string out;
		out.reserve(value.size() + 2);
		out.push_back('\'');

		for (const char ch : value)
		{
			if (ch == '\'')
			{
				out += "'\"'\"'";
			}
			else
			{
				out.push_back(ch);
			}
		}

		out.push_back('\'');
		return out;
	}
} // namespace bridge

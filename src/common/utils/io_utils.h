#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <mutex>
#include <random>
#include <sstream>
#include <string>

namespace uam::io
{
	inline std::string ReadTextFile(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::in | std::ios::binary);
		if (!file)
		{
			return "";
		}
		std::ostringstream ss;
		ss << file.rdbuf();
		return ss.str();
	}

	inline std::filesystem::path MakeTempWritePath(const std::filesystem::path& path)
	{
		const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
		const std::uint64_t token = static_cast<std::uint64_t>(now) ^ static_cast<std::uint64_t>(std::random_device{}());
		return path.string() + ".tmp." + std::to_string(token);
	}

	inline bool AtomicWriteTextFile(const std::filesystem::path& path, const std::string& content)
	{
		static std::mutex write_mutex;
		std::lock_guard<std::mutex> lock(write_mutex);

		const std::filesystem::path parent = path.parent_path();
		std::error_code ec;
		if (!parent.empty())
		{
			std::filesystem::create_directories(parent, ec);
			if (ec)
			{
				return false;
			}
		}

		const std::filesystem::path temp_path = MakeTempWritePath(path);
		const std::filesystem::path backup_path = path.string() + ".bak";

		{
			std::ofstream file(temp_path, std::ios::out | std::ios::binary | std::ios::trunc);
			if (!file)
			{
				return false;
			}

			file.write(content.data(), static_cast<std::streamsize>(content.size()));
			file.flush();

			if (!file.good())
			{
				file.close();
				std::filesystem::remove(temp_path, ec);
				return false;
			}
		}

		if (std::filesystem::exists(path, ec) && !ec)
		{
			std::filesystem::remove(backup_path, ec);
			ec.clear();
			std::filesystem::rename(path, backup_path, ec);
			if (ec)
			{
				std::filesystem::remove(temp_path, ec);
				return false;
			}
		}

		std::filesystem::rename(temp_path, path, ec);
		if (ec)
		{
			std::error_code restore_ec;
			if (std::filesystem::exists(backup_path, restore_ec) && !restore_ec)
			{
				std::filesystem::rename(backup_path, path, restore_ec);
			}
			std::filesystem::remove(temp_path, ec);
			return false;
		}

		std::filesystem::remove(backup_path, ec);
		return true;
	}

	inline bool WriteTextFile(const std::filesystem::path& path, const std::string& content)
	{
		return AtomicWriteTextFile(path, content);
	}
}

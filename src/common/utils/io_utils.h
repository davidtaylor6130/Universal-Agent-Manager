#pragma once

#include "common/paths/path_utils.h"
#include "common/utils/time_utils.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>

namespace uam::io
{
	inline constexpr const char* kTempWritePathSuffix = ".tmp.";
	inline constexpr const char* kBackupPathSuffix = ".bak";

	inline bool TryReadFile(const std::filesystem::path& path, std::string& out)
	{
		out.clear();
		std::ifstream file(path, std::ios::in | std::ios::binary);
		if (!file)
		{
			return false;
		}

		std::ostringstream ss;
		ss << file.rdbuf();
		if (file.bad())
		{
			out.clear();
			return false;
		}
		out = ss.str();
		return true;
	}

	inline bool TryReadTextFile(const std::filesystem::path& path, std::string& out)
	{
		return TryReadFile(path, out);
	}

	inline std::string ReadTextFile(const std::filesystem::path& path)
	{
		std::string text;
		TryReadTextFile(path, text);
		return text;
	}

	template <typename LineVisitor> inline bool ForEachTextFileLine(const std::filesystem::path& path, LineVisitor visitor)
	{
		std::ifstream file(path, std::ios::in | std::ios::binary);
		if (!file)
		{
			return false;
		}

		std::string line;
		while (std::getline(file, line))
		{
			if (!visitor(line))
			{
				break;
			}
		}
		return !file.bad();
	}

	inline std::optional<std::string> ReadFirstTextFileLine(const std::filesystem::path& path)
	{
		std::optional<std::string> first_line;
		ForEachTextFileLine(path,
		                    [&first_line](const std::string& line)
		                    {
			                    first_line = line;
			                    return false;
		                    });
		return first_line;
	}

	template <typename ByteVisitor> inline bool ForEachBinaryFileByte(const std::filesystem::path& path, ByteVisitor visitor)
	{
		std::ifstream file(path, std::ios::in | std::ios::binary);
		if (!file)
		{
			return false;
		}

		char ch = '\0';
		while (file.get(ch))
		{
			if (!visitor(ch))
			{
				break;
			}
		}
		return !file.bad();
	}

	inline bool TryReadBinaryFile(const std::filesystem::path& path, std::string& out)
	{
		return TryReadFile(path, out);
	}

	inline std::filesystem::path MakeTempWritePath(const std::filesystem::path& path)
	{
		const std::uint64_t monotonic_token = static_cast<std::uint64_t>(uam::time::SteadyEpochNanosecondsNow());
		const std::uint64_t random_token = static_cast<std::uint64_t>(std::random_device{}());
		const std::uint64_t token = monotonic_token ^ random_token;
		return path.string() + kTempWritePathSuffix + std::to_string(token);
	}

	inline std::filesystem::path MakeBackupPath(const std::filesystem::path& path)
	{
		return path.string() + kBackupPathSuffix;
	}

	inline bool AtomicWriteFile(const std::filesystem::path& path, std::string_view content)
	{
		static std::mutex write_mutex;
		std::lock_guard<std::mutex> lock(write_mutex);

		const std::filesystem::path parent = path.parent_path();
		std::error_code ec;
		if (!parent.empty())
		{
			if (!uam::paths::CreateDirectoriesNoThrow(parent, &ec))
			{
				return false;
			}
		}

		const std::filesystem::path temp_path = MakeTempWritePath(path);
		const std::filesystem::path backup_path = MakeBackupPath(path);

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
				uam::paths::RemoveFileNoThrow(temp_path, &ec);
				return false;
			}
		}

		if (uam::paths::PathExistsNoThrow(path))
		{
			uam::paths::RemoveFileNoThrow(backup_path, &ec);
			ec.clear();
			if (!uam::paths::RenameNoThrow(path, backup_path, &ec))
			{
				uam::paths::RemoveFileNoThrow(temp_path, &ec);
				return false;
			}
		}

		if (!uam::paths::RenameNoThrow(temp_path, path, &ec))
		{
			std::error_code restore_ec;
			if (uam::paths::PathExistsNoThrow(backup_path))
			{
				uam::paths::RenameNoThrow(backup_path, path, &restore_ec);
			}
			uam::paths::RemoveFileNoThrow(temp_path, &ec);
			return false;
		}

		uam::paths::RemoveFileNoThrow(backup_path, &ec);
		return true;
	}

	inline bool WriteTextFile(const std::filesystem::path& path, std::string_view content)
	{
		return AtomicWriteFile(path, content);
	}

	inline bool WriteBinaryFile(const std::filesystem::path& path, std::string_view content)
	{
		return AtomicWriteFile(path, content);
	}
} // namespace uam::io

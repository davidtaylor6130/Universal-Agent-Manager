#pragma once

#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>

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

	inline bool WriteTextFile(const std::filesystem::path& path, const std::string& content)
	{
		std::ofstream file(path, std::ios::out | std::ios::binary | std::ios::trunc);
		if (!file)
		{
			return false;
		}
		file.write(content.data(), content.size());
		return file.good();
	}
}

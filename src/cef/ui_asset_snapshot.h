#pragma once

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

namespace uam::cef
{
	/// <summary>Owns a bounded copy of one UI build for the browser process lifetime.</summary>
	class UiAssetSnapshot
	{
	  public:
		static constexpr std::size_t kMaxBytes = 64 * 1024 * 1024;
		static constexpr std::size_t kMaxFileBytes = 16 * 1024 * 1024;
		static constexpr std::size_t kMaxFiles = 4096;

		explicit UiAssetSnapshot(const std::filesystem::path& root)
		{
			std::size_t total = 0;
			for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(root))
			{
				if (entry.is_symlink()) throw std::runtime_error("UI snapshot contains a symbolic link.");
				if (entry.is_directory()) continue;
				if (!entry.is_regular_file()) throw std::runtime_error("UI snapshot contains a non-file resource.");
				const std::filesystem::file_time_type modified = entry.last_write_time();
				const std::uintmax_t size = entry.file_size();
				if (size > kMaxFileBytes || size > kMaxBytes - total || m_files.size() >= kMaxFiles)
					throw std::runtime_error("UI snapshot exceeds its resource limit.");
				std::ifstream input(entry.path(), std::ios::binary);
				std::string bytes(static_cast<std::size_t>(size), '\0');
				if (!input || !input.read(bytes.data(), static_cast<std::streamsize>(size)) || input.peek() != std::char_traits<char>::eof() || entry.last_write_time() != modified)
					throw std::runtime_error("UI resource changed or could not be read during startup.");
				total += bytes.size();
				m_files.emplace(entry.path().lexically_relative(root).generic_string(), std::move(bytes));
			}
			if (!m_files.contains("index.html")) throw std::runtime_error("UI snapshot has no index.html.");
			const std::string* manifest_bytes = Find(".vite/manifest.json");
			if (!manifest_bytes) throw std::runtime_error("UI snapshot has no build manifest.");
			const nlohmann::json manifest = nlohmann::json::parse(*manifest_bytes);
			if (!manifest.is_object() || !manifest.contains("index.html"))
				throw std::runtime_error("UI build manifest has no entry point.");
			for (const nlohmann::json& chunk : manifest)
			{
				if (!chunk.is_object() || !chunk.contains("file") || !Find(chunk.at("file").get<std::string>()))
					throw std::runtime_error("UI build manifest references a missing chunk.");
				for (const char* list : {"css", "assets"})
				{
					if (!chunk.contains(list)) continue;
					for (const nlohmann::json& resource : chunk.at(list))
						if (!Find(resource.get<std::string>())) throw std::runtime_error("UI build manifest references a missing resource.");
				}
				for (const char* list : {"imports", "dynamicImports"})
				{
					if (!chunk.contains(list)) continue;
					for (const nlohmann::json& dependency : chunk.at(list))
						if (!manifest.contains(dependency.get<std::string>())) throw std::runtime_error("UI build manifest references an unknown dependency.");
				}
			}
			if (Find("index.html")->find(manifest.at("index.html").at("file").get<std::string>()) == std::string::npos)
				throw std::runtime_error("UI startup document and build manifest do not match.");
		}

		const std::string* Find(const std::string& relative) const
		{
			const std::map<std::string, std::string>::const_iterator found = m_files.find(relative);
			return found == m_files.end() ? nullptr : &found->second;
		}

	  private:
		std::map<std::string, std::string> m_files;
	};

	/// <summary>Prevents renderer recovery loops; explicit user recovery opens a new budget.</summary>
	class UiRecoveryBudget
	{
	  public:
		bool AllowAutomaticRecovery()
		{
			if (m_attempts >= 3) return false;
			++m_attempts;
			return true;
		}
		void Reset()
		{
			m_attempts = 0;
		}
	  private:
		unsigned int m_attempts = 0;
	};
}

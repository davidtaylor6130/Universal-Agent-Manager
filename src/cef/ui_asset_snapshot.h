#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

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
			struct PendingFile
			{
				std::filesystem::directory_entry entry;
				std::filesystem::file_time_type modified;
				std::uintmax_t size;
				std::string bytes;
			};
			std::vector<PendingFile> files;
			std::size_t total = 0;
			for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(root))
			{
				if (entry.is_symlink()) throw std::runtime_error("UI snapshot contains a symbolic link.");
				if (entry.is_directory()) continue;
				if (!entry.is_regular_file()) throw std::runtime_error("UI snapshot contains a non-file resource.");
				const std::filesystem::file_time_type modified = entry.last_write_time();
				const std::uintmax_t size = entry.file_size();
				if (size > kMaxFileBytes || size > kMaxBytes - total || files.size() >= kMaxFiles)
					throw std::runtime_error("UI snapshot exceeds its resource limit.");
				total += static_cast<std::size_t>(size);
				files.push_back({entry, modified, size, {}});
			}
			// Admit the complete bounded inventory before allocating file bytes. Each worker owns disjoint buffers;
			// the map and manifest become visible only after all readers have joined successfully.
			std::atomic<bool> cancelled = false;
			std::array<std::future<void>, 4> readers;
			const std::size_t reader_count = std::min(files.size(), readers.size());
			try
			{
				for (std::size_t reader = 0; reader < reader_count; ++reader)
				{
					readers[reader] = std::async(std::launch::async, [&files, &cancelled, reader, reader_count]
					{
						try
						{
							for (std::size_t index = reader; index < files.size() && !cancelled.load(); index += reader_count)
							{
								PendingFile& file = files[index];
								const std::filesystem::path& path = file.entry.path();
								// Enumeration may precede this read: recheck the path before opening it.
								const auto status = std::filesystem::symlink_status(path);
								if (std::filesystem::is_symlink(status)) throw std::runtime_error("UI snapshot contains a symbolic link.");
								if (!std::filesystem::is_regular_file(status)) throw std::runtime_error("UI snapshot contains a non-file resource.");
								std::ifstream input(path, std::ios::binary);
								file.bytes.resize(static_cast<std::size_t>(file.size));
								if (!input || !input.read(file.bytes.data(), static_cast<std::streamsize>(file.size)) || input.peek() != std::char_traits<char>::eof() || std::filesystem::last_write_time(path) != file.modified)
									throw std::runtime_error("UI resource changed or could not be read during startup.");
							}
						}
						catch (...)
						{
							cancelled.store(true);
							throw;
						}
					});
				}
				for (std::size_t reader = 0; reader < reader_count; ++reader) readers[reader].get();
			}
			catch (...)
			{
				cancelled.store(true);
				// Async futures drain before cancelled/files are destroyed, including a partial launch failure.
				throw;
			}
			for (PendingFile& file : files)
				m_files.emplace(file.entry.path().lexically_relative(root).generic_string(), std::move(file.bytes));
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

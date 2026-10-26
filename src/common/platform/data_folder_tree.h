#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace uam::io
{
	enum class AtomicWriteStage;
}

namespace uam::platform
{
	/// Private stopped-profile IO. POSIX operations are relative to retained directory descriptors;
	/// Windows retains every ancestor without delete sharing and rejects reparse points/hard links.
	class DataFolderTree final
	{
	  public:
		struct Entry
		{
			std::filesystem::path relative;
			bool directory = false;
			std::uint64_t bytes = 0;
			std::uint32_t mode = 0;
			std::string sha256;
			std::string object;
			std::string modified;
		};
		using Inventory = std::vector<Entry>;
		struct Publication
		{
			bool confirmed = false;
			bool replaced = false;
			std::string error;
		};
		using WriteGate = std::function<bool(io::AtomicWriteStage)>;
		enum class OpenStage
		{
			IdentityRetained,
			DirectoryOpened
		};
		using OpenObserver = std::function<void(OpenStage)>;
		using ReadObserver = std::function<void(const std::filesystem::path&)>;
		class ReadWitness final
		{
		  public:
			bool Current() const { return m_current(); }
			bool SameParent(const ReadWitness& other) const { return m_parent == other.m_parent; }
		  private:
			friend class DataFolderTree;
			ReadWitness(std::function<bool()> current, std::string parent) : m_current(std::move(current)), m_parent(std::move(parent)) {}
			std::function<bool()> m_current;
			std::string m_parent;
		};
		static std::shared_ptr<DataFolderTree> Open(const std::filesystem::path& root, std::string& error, const OpenObserver& observer = {}, const ReadObserver& read_observer = {});
		~DataFolderTree();
		bool Current() const;
		std::string Identity() const;
		std::filesystem::path RootPath() const;
		bool Scan(Inventory& entries, std::string& error) const;
		bool MakeDirectory(const std::filesystem::path& relative, std::string& error) const;
		bool CopyEntry(const Entry& entry, const DataFolderTree& destination, std::string& error) const;
		bool Read(const std::filesystem::path& relative, std::size_t limit, std::string& bytes, std::string& error, std::shared_ptr<const ReadWitness>* retained = nullptr) const;
		Publication Publish(const std::filesystem::path& relative, const std::string& bytes, const WriteGate& gate = {}) const;
		static std::string Digest(const Inventory& entries);
		static std::string ObjectDigest(const Inventory& entries);

	  private:
		struct Impl;
		explicit DataFolderTree(std::unique_ptr<Impl> impl);
		std::unique_ptr<Impl> m_impl;
	};
} // namespace uam::platform

#pragma once

#include "common/models/app_models.h"
#include "common/utils/hash_utils.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uam::io
{
	enum class AtomicWriteStage;
}

namespace uam::platform { class AtomicTempWriterOrigin; }

namespace uam::chat_pages
{
	struct Span
	{
		std::uint64_t offset = 0;
		std::uint64_t length = 0;
		std::uint64_t content_hash = 0;
	};
	struct Index
	{
		std::string source_revision;
		std::string messages_digest;
		std::string attachment_owner;
		std::uint64_t source_size = 0;
		std::uint64_t array_begin = 0;
		std::uint64_t array_end = 0;
		std::uint64_t message_count = 0;
		std::uint64_t records_begin = 0;
		std::optional<std::vector<std::size_t>> user_indices;
	};

	/// <summary>Shares the existing frontend fingerprint across streamed messages and hydrated views.</summary>
	void AppendMessageDigest(std::uint64_t& hash, const Message& message);
	std::string FinishMessageDigest(std::uint64_t hash, std::size_t count);
	std::filesystem::path IndexPath(const std::filesystem::path& primary);
	bool ReadSpan(std::ifstream& input, Span span, std::string& text);
	std::optional<Index> ReadIndex(const std::filesystem::path& path, std::string_view revision);
	bool ReadMetadata(std::ifstream& primary, const Index& index, std::string& text);
	bool ReadMessageSpans(const std::filesystem::path& index_path, const Index& index,
	    std::size_t begin, std::size_t end, std::vector<Span>& spans);

	/// <summary>Stores fixed-size offsets incrementally, keeping only bounded user-navigation details in memory.</summary>
	class Builder
	{
	public:
		/// An empty owner builds offsets only; repositories must not reuse that
		/// index as an attachment ownership validation receipt.
		explicit Builder(const std::filesystem::path& primary, std::string_view owner = {});
		/// Exclusive opt-in private spool; a preexisting path is never adopted or removed.
		Builder(const std::filesystem::path& primary, std::string_view owner,
		    const std::filesystem::path& private_records_path);
		~Builder();
		bool Add(Span span, const Message& message);
		bool CopyRecords(const std::filesystem::path& path, const Index& index, std::uint64_t array_begin);
		bool Finish(const std::filesystem::path& temporary_index, std::string_view revision,
		    std::uint64_t source_size, std::uint64_t array_begin, std::uint64_t array_end);
		/// Opt-in observers only: actual exclusive creation and successfully emitted index bytes.
		/// Existing Finish retains its original path and invokes no observers.
		bool Finish(const std::filesystem::path& temporary_index, std::string_view revision,
		    std::uint64_t source_size, std::uint64_t array_begin, std::uint64_t array_end,
		    const std::function<bool(io::AtomicWriteStage)>& stage_observer,
		    const std::function<bool(std::string_view)>& emitted_observer);
		/// Opt-in own writer descriptor identity; existing Finish overloads are unchanged.
		bool FinishWithOrigin(const std::filesystem::path& temporary_index, std::string_view revision,
		    std::uint64_t source_size, std::uint64_t array_begin, std::uint64_t array_end,
		    const std::function<bool(io::AtomicWriteStage)>& stage_observer,
		    const std::function<bool(std::string_view)>& emitted_observer,
		    std::shared_ptr<const platform::AtomicTempWriterOrigin>& origin_out);
		std::size_t Count() const { return m_count; }
	private:
		struct PrivateRecords;
		bool AppendRecord(std::string_view record);
		bool SealPrivateRecords();
		std::unique_ptr<PrivateRecords> m_privateRecords;
		std::filesystem::path m_recordsPath;
		std::ofstream m_records;
		// Creation alone does not authorize removing later changed private spool bytes.
		// Legacy cleanup remains on its original path without private witness IO.
		bool m_recordsOwned = true;
		std::size_t m_count = 0;
		std::string m_digestOverride;
		std::string m_attachmentOwner;
		std::uint64_t m_hash = hashing::kFnv1a64OffsetBasis;
		std::optional<std::vector<std::size_t>> m_users = std::vector<std::size_t>{};
	};

	/// <summary>Validates old primary JSON while discovering message offsets without a whole-transcript DOM.</summary>
	bool Scan(const std::filesystem::path& primary, Builder& builder,
	    const std::function<std::optional<Message>(std::string_view)>& decode,
	    std::uint64_t& array_begin, std::uint64_t& array_end,
	    const std::function<bool()>& cancelled = {});
}

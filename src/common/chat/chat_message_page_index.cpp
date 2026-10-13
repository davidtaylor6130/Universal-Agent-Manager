#include "common/chat/chat_message_page_index.h"
#include "common/utils/io_utils.h"
#include <nlohmann/json.hpp>
#include <array>
#include <limits>

namespace uam::chat_pages
{
	namespace
	{
		void FingerprintHashString(std::uint64_t& hash, const std::string& value)
		{
			const std::uint64_t value_hash = std::hash<std::string>{}(value);
			hashing::UpdateFnv1a64(hash, reinterpret_cast<const unsigned char*>(&value_hash), sizeof(value_hash));
		}
		void FingerprintHashBool(std::uint64_t& hash, bool value)
		{
			const unsigned char byte = value ? 1 : 0;
			hashing::UpdateFnv1a64(hash, &byte, 1);
		}
	}
	void AppendMessageDigest(std::uint64_t& hash, const Message& message)
	{
		FingerprintHashString(hash, (message.role == MessageRole::Assistant ? "assistant" : message.role == MessageRole::System ? "system" : "user"));
		FingerprintHashString(hash, message.created_at);
		FingerprintHashString(hash, message.provider);
		FingerprintHashString(hash, message.model_id);
		FingerprintHashString(hash, message.content);
		FingerprintHashString(hash, std::to_string(message.processing_time_ms));
		FingerprintHashString(hash, message.checkpoint_sha);
		FingerprintHashString(hash, message.checkpoint_parent_sha);
		FingerprintHashString(hash, message.thoughts);
		FingerprintHashString(hash, message.plan_summary);
		FingerprintHashString(hash, std::to_string(message.tool_calls.size()));
		for (const ToolCall& tool_call : message.tool_calls)
		{
			FingerprintHashString(hash, tool_call.id);
			FingerprintHashString(hash, tool_call.name);
			FingerprintHashString(hash, tool_call.kind);
			FingerprintHashString(hash, tool_call.approval_status);
			FingerprintHashString(hash, tool_call.task_id);
			FingerprintHashString(hash, tool_call.child_activity);
			FingerprintHashString(hash, tool_call.status);
			// The native refresh guard needs content changes even when output is deferred.
			FingerprintHashString(hash, tool_call.args_json);
			FingerprintHashString(hash, tool_call.result_text);
			FingerprintHashBool(hash, tool_call.is_sub_agent);
			FingerprintHashString(hash, tool_call.sub_agent_id);
			FingerprintHashString(hash, tool_call.sub_agent_title);
		}
		FingerprintHashString(hash, std::to_string(message.plan_entries.size()));
		for (const MessagePlanEntry& entry : message.plan_entries)
		{
			FingerprintHashString(hash, entry.content);
			FingerprintHashString(hash, entry.priority);
			FingerprintHashString(hash, entry.status);
		}
		FingerprintHashString(hash, std::to_string(message.blocks.size()));
		for (const MessageBlock& block : message.blocks)
		{
			FingerprintHashString(hash, block.type);
			FingerprintHashString(hash, block.text);
			FingerprintHashString(hash, block.tool_call_id);
			FingerprintHashString(hash, block.request_id_json);
			FingerprintHashString(hash, std::to_string(block.compaction_tokens_before));
			FingerprintHashString(hash, std::to_string(block.compaction_tokens_after));
		}
		FingerprintHashString(hash, std::to_string(message.markdown_store_files.size()));
		for (const std::string& file : message.markdown_store_files)
		{
			FingerprintHashString(hash, file);
		}
		FingerprintHashString(hash, std::to_string(message.markdown_store_prompt_blocks.size()));
		for (const std::string& prompt_block : message.markdown_store_prompt_blocks)
		{
			FingerprintHashString(hash, prompt_block);
		}
		FingerprintHashString(hash, std::to_string(message.attachments.size()));
		for (const MessageAttachment& attachment : message.attachments)
		{
			FingerprintHashString(hash, attachment.id);
			FingerprintHashString(hash, attachment.name);
			FingerprintHashString(hash, attachment.kind);
			FingerprintHashString(hash, attachment.mime_type);
			FingerprintHashString(hash, attachment.path);
			FingerprintHashString(hash, std::to_string(attachment.size_bytes));
			FingerprintHashBool(hash, attachment.copied);
			FingerprintHashString(hash, attachment.preview_key);
			FingerprintHashString(hash, attachment.preview_origin_path);
		}
		FingerprintHashBool(hash, message.interrupted);
		FingerprintHashString(hash, message.stop_reason);
		FingerprintHashBool(hash, message.acp_prompt_not_sent);
		FingerprintHashBool(hash, message.priority_steer);
		FingerprintHashBool(hash, message.continues_turn);
	}
	std::string FinishMessageDigest(std::uint64_t hash, std::size_t count)
	{
		FingerprintHashString(hash, std::to_string(count));
		return hashing::Hex64Padded(hash);
	}
	namespace
	{
		constexpr std::string_view kMagic = "UAMPAGE2";
		constexpr std::uint64_t kRecordBytes = 32;
		constexpr std::size_t kMetadataLimit = 256 * 1024 * 1024;
		std::string Encode(std::uint64_t value)
		{
			std::string bytes(8, '\0');
			for (std::size_t index = 0; index < 8; ++index) bytes[index] = static_cast<char>(value >> (index * 8));
			return bytes;
		}
		std::uint64_t HeaderChecksum(std::string_view bytes)
		{
			std::uint64_t hash = hashing::kFnv1a64OffsetBasis;
			hashing::UpdateFnv1a64(hash, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
			return hash;
		}
		bool ReadNumber(std::istream& input, std::uint64_t& value)
		{
			std::array<unsigned char, 8> bytes{};
			if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) return false;
			value = 0;
			for (std::size_t index = 0; index < bytes.size(); ++index) value |= std::uint64_t(bytes[index]) << (index * 8);
			return true;
		}
		bool ReadString(std::istream& input, std::string& value)
		{
			std::uint64_t size = 0;
			if (!ReadNumber(input, size) || size > 256) return false;
			value.resize(static_cast<std::size_t>(size));
			return static_cast<bool>(input.read(value.data(), static_cast<std::streamsize>(size)));
		}
		std::string Record(Span span, std::uint64_t hash, std::uint64_t ordinal)
		{
			return Encode(span.offset) + Encode(span.length) + Encode(hash) + Encode(ordinal);
		}
	}

	std::filesystem::path IndexPath(const std::filesystem::path& primary)
	{
		std::filesystem::path path = primary;
		path += ".pages";
		return path;
	}

	bool ReadSpan(std::ifstream& input, Span span, std::string& text)
	{
		if (span.offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
		    span.length > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()) || span.length > text.max_size()) return false;
		input.clear();
		input.seekg(static_cast<std::streamoff>(span.offset));
		text.resize(static_cast<std::size_t>(span.length));
		return static_cast<bool>(input.read(text.data(), static_cast<std::streamsize>(span.length)));
	}

	std::optional<Index> ReadIndex(const std::filesystem::path& path, std::string_view revision)
	{
		std::ifstream input(path, std::ios::binary);
		std::string magic(kMagic.size(), '\0');
		if (!input.read(magic.data(), magic.size()) || magic != kMagic) return std::nullopt;
		Index index;
		std::uint64_t users = 0;
		if (!ReadString(input, index.source_revision) || index.source_revision != revision ||
		    !ReadString(input, index.messages_digest) || index.messages_digest.size() != 16 ||
		    !ReadNumber(input, index.source_size) || !ReadNumber(input, index.array_begin) ||
		    !ReadNumber(input, index.array_end) || !ReadNumber(input, index.message_count) || !ReadNumber(input, users) ||
		    index.array_begin >= index.array_end || index.array_end >= index.source_size ||
		    index.message_count > std::numeric_limits<std::size_t>::max()) return std::nullopt;
		if (users != std::numeric_limits<std::uint64_t>::max())
		{
			if (users > 32768 || users > index.message_count) return std::nullopt;
			index.user_indices.emplace();
			for (std::uint64_t cursor = 0; cursor < users; ++cursor)
			{
				std::uint64_t ordinal = 0;
				if (!ReadNumber(input, ordinal) || ordinal >= index.message_count ||
				    (!index.user_indices->empty() && ordinal <= index.user_indices->back())) return std::nullopt;
				index.user_indices->push_back(static_cast<std::size_t>(ordinal));
			}
		}
		const std::streampos position = input.tellg();
		if (position < 0) return std::nullopt;
		std::uint64_t checksum = 0;
		if (!ReadNumber(input, checksum)) return std::nullopt;
		std::string header(static_cast<std::size_t>(position), '\0');
		input.seekg(0);
		if (!input.read(header.data(), static_cast<std::streamsize>(header.size())) ||
		    HeaderChecksum(header) != checksum) return std::nullopt;
		index.records_begin = static_cast<std::uint64_t>(position) + 8;
		std::error_code error;
		const std::uintmax_t bytes = std::filesystem::file_size(path, error);
		if (error || index.records_begin > bytes || index.message_count > (bytes - index.records_begin) / kRecordBytes ||
		    bytes - index.records_begin != index.message_count * kRecordBytes) return std::nullopt;
		return index;
	}

	bool ReadMetadata(std::ifstream& primary, const Index& index, std::string& text)
	{
		const std::uint64_t suffix_start = index.array_end + 1;
		if (index.array_begin > kMetadataLimit || index.source_size - suffix_start > kMetadataLimit - index.array_begin) return false;
		std::string prefix, suffix;
		if (!ReadSpan(primary, {0, index.array_begin}, prefix) ||
		    !ReadSpan(primary, {suffix_start, index.source_size - suffix_start}, suffix)) return false;
		text = prefix + "[]" + suffix;
		return true;
	}

	bool ReadMessageSpans(const std::filesystem::path& path, const Index& index,
	    std::size_t begin, std::size_t end, std::vector<Span>& spans)
	{
		if (begin > end || end > index.message_count) return false;
		std::ifstream input(path, std::ios::binary);
		input.seekg(static_cast<std::streamoff>(index.records_begin + begin * kRecordBytes));
		spans.clear();
		for (std::size_t ordinal = begin; ordinal < end; ++ordinal)
		{
			Span span;
			std::uint64_t stored_ordinal = 0;
			if (!ReadNumber(input, span.offset) || !ReadNumber(input, span.length) || !ReadNumber(input, span.content_hash) ||
			    !ReadNumber(input, stored_ordinal) || stored_ordinal != ordinal ||
			    span.offset <= index.array_begin || span.offset >= index.array_end || span.length == 0 ||
			    span.length > index.array_end - span.offset || (!spans.empty() && span.offset <= spans.back().offset)) return false;
			spans.push_back(span);
		}
		return true;
	}

	Builder::Builder(const std::filesystem::path& primary)
	    : m_recordsPath(io::MakeTempWritePath(IndexPath(primary))), m_records(m_recordsPath, std::ios::binary | std::ios::trunc)
	{
	}
	Builder::~Builder()
	{
		m_records.close();
		io::RemoveAtomicTempNoThrow(m_recordsPath);
	}
	bool Builder::Add(Span span, const Message& message)
	{
		std::uint64_t hash = hashing::kFnv1a64OffsetBasis;
		AppendMessageDigest(hash, message);
		m_records << Record(span, hash, m_count);
		if (!m_records) return false;
		AppendMessageDigest(m_hash, message);
		if (m_users && message.role == MessageRole::User)
		{
			if (m_users->size() == 32768) m_users.reset();
			else m_users->push_back(m_count);
		}
		++m_count;
		return true;
	}
	bool Builder::CopyRecords(const std::filesystem::path& path, const Index& index, std::uint64_t array_begin)
	{
		std::ifstream input(path, std::ios::binary);
		input.seekg(static_cast<std::streamoff>(index.records_begin));
		for (std::uint64_t ordinal = 0; ordinal < index.message_count; ++ordinal)
		{
			Span span;
			std::uint64_t stored_ordinal = 0;
			if (!ReadNumber(input, span.offset) || !ReadNumber(input, span.length) || !ReadNumber(input, span.content_hash) ||
			    !ReadNumber(input, stored_ordinal) || stored_ordinal != ordinal || span.offset <= index.array_begin ||
			    span.offset >= index.array_end || span.length > index.array_end - span.offset) return false;
			span.offset = span.offset - index.array_begin + array_begin;
			m_records << Record(span, span.content_hash, ordinal);
			if (!m_records) return false;
		}
		m_count = static_cast<std::size_t>(index.message_count);
		m_users = index.user_indices;
		m_digestOverride = index.messages_digest;
		return true;
	}
	bool Builder::Finish(const std::filesystem::path& temporary_index, std::string_view revision,
	    std::uint64_t source_size, std::uint64_t array_begin, std::uint64_t array_end)
	{
		m_records.flush();
		if (!m_records || revision.size() > 256) return false;
		m_records.close();
		const std::string digest = m_digestOverride.empty() ? FinishMessageDigest(m_hash, m_count) : m_digestOverride;
		std::string header = std::string(kMagic) + Encode(revision.size()) + std::string(revision) +
		    Encode(digest.size()) + digest + Encode(source_size) + Encode(array_begin) + Encode(array_end) + Encode(m_count) +
		    Encode(m_users ? m_users->size() : std::numeric_limits<std::uint64_t>::max());
		if (m_users) for (std::size_t ordinal : *m_users) header += Encode(ordinal);
		header += Encode(HeaderChecksum(header));
		io::AtomicWriteResult result;
		return io::WriteAndSyncAtomicTemp(temporary_index, [&](const std::function<bool(std::string_view)>& append)
		{
			if (!append(header)) return false;
			std::ifstream input(m_recordsPath, std::ios::binary);
			if (!input) return false;
			std::array<char, 65536> buffer{};
			while (input.read(buffer.data(), buffer.size()) || input.gcount() != 0)
				if (!append(std::string_view(buffer.data(), static_cast<std::size_t>(input.gcount())))) return false;
			return input.eof();
		}, {}, result);
	}

	namespace
	{
		class OffsetSax final : public nlohmann::json_sax<nlohmann::json>
		{
		public:
			OffsetSax(std::ifstream& input, std::ifstream& body, Builder& builder,
			    const std::function<std::optional<Message>(std::string_view)>& decode,
			    const std::function<bool()>& cancelled)
			    : m_input(input), m_body(body), m_builder(builder), m_decode(decode), m_cancelled(cancelled) {}
			bool null() override { return Scalar(); }
			bool boolean(bool) override { return Scalar(); }
			bool number_integer(number_integer_t) override { return Scalar(); }
			bool number_unsigned(number_unsigned_t) override { return Scalar(); }
			bool number_float(number_float_t, const string_t&) override { return Scalar(); }
			bool string(string_t&) override { return Scalar(); }
			bool binary(binary_t&) override { return false; }
			bool key(string_t& key) override
			{
				if (!Alive()) return false;
				if (m_depth == 1 && key == "messages")
				{
					if (found) return false;
					m_nextMessages = true;
				}
				return true;
			}
			bool start_object(std::size_t) override
			{
				if (!Alive() || m_nextMessages) return false;
				if (m_inMessages && m_depth == 2) m_messageStart = Position() - 1;
				++m_depth;
				return true;
			}
			bool end_object() override
			{
				if (!Alive() || m_depth == 0) return false;
				if (m_inMessages && m_depth == 3)
				{
					const Span span{m_messageStart, Position() - m_messageStart};
					std::string raw;
					if (!ReadSpan(m_body, span, raw)) return false;
					const std::optional<Message> message = m_decode(raw);
					if (!message || !m_builder.Add(span, *message)) return false;
				}
				--m_depth;
				return true;
			}
			bool start_array(std::size_t) override
			{
				if (!Alive() || (m_inMessages && m_depth == 2)) return false;
				if (m_nextMessages)
				{
					found = true;
					m_nextMessages = false;
					m_inMessages = true;
					array_begin = Position() - 1;
				}
				++m_depth;
				return true;
			}
			bool end_array() override
			{
				if (!Alive() || m_depth == 0) return false;
				if (m_inMessages && m_depth == 2)
				{
					array_end = Position() - 1;
					m_inMessages = false;
				}
				--m_depth;
				return true;
			}
			bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }
			std::uint64_t array_begin = 0;
			std::uint64_t array_end = 0;
			bool found = false;
		private:
			bool Alive() const { return !m_cancelled || !m_cancelled(); }
			bool Scalar() { return Alive() && !m_nextMessages && !(m_inMessages && m_depth == 2); }
			std::uint64_t Position() const { return static_cast<std::uint64_t>(m_input.tellg()); }
			std::ifstream& m_input;
			std::ifstream& m_body;
			Builder& m_builder;
			const std::function<std::optional<Message>(std::string_view)>& m_decode;
			const std::function<bool()>& m_cancelled;
			std::size_t m_depth = 0;
			std::uint64_t m_messageStart = 0;
			bool m_nextMessages = false;
			bool m_inMessages = false;
		};
	}
	bool Scan(const std::filesystem::path& primary, Builder& builder,
	    const std::function<std::optional<Message>(std::string_view)>& decode,
	    std::uint64_t& array_begin, std::uint64_t& array_end, const std::function<bool()>& cancelled)
	{
		std::ifstream input(primary, std::ios::binary), body(primary, std::ios::binary);
		if (!input || !body) return false;
		OffsetSax sax(input, body, builder, decode, cancelled);
		if (!nlohmann::json::sax_parse(input, &sax) || !sax.found) return false;
		array_begin = sax.array_begin;
		array_end = sax.array_end;
		return true;
	}
}

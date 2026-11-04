#include "common/usage/transcript_scan.h"
#include "common/utils/io_utils.h"
#include "common/utils/nlohmann_json_utils.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

namespace uam::usage
{
	namespace
	{
		std::string ReadGuard(std::ifstream& input, std::uintmax_t offset, std::size_t count)
		{
			input.clear();
			input.seekg(static_cast<std::streamoff>(offset));
			std::string bytes(count, '\0');
			input.read(bytes.data(), static_cast<std::streamsize>(count));
			bytes.resize(static_cast<std::size_t>(input.gcount()));
			return bytes;
		}
	}

	namespace
	{
		class GeminiProjection : public nlohmann::json_sax<nlohmann::json>
		{
		public:
			std::string session;
			bool saw_messages = false;
			const std::function<bool()>& cancelled;
			std::function<bool(const nlohmann::json&)> message;
			GeminiProjection(const std::function<bool()>& stop, std::function<bool(const nlohmann::json&)> sink = {}) : cancelled(stop), message(std::move(sink)) {}
			bool null() override { return Scalar(nullptr); }
			bool boolean(bool value) override { return Scalar(value); }
			bool number_integer(number_integer_t value) override { return Scalar(value); }
			bool number_unsigned(number_unsigned_t value) override { return Scalar(value); }
			bool number_float(number_float_t value, const string_t&) override { return Scalar(value); }
			bool string(string_t& value) override
			{
				if (m_depth == 1 && m_key == "sessionId") { if (value.size() <= 512) session = value; }
				if ((m_depth == m_messageDepth || m_depth == m_tokensDepth) && value.size() > 8192 && (m_key == "id" || m_key == "model" || m_key == "timestamp")) return false;
				return Scalar(value);
			}
			bool binary(binary_t&) override { return false; }
			bool start_object(std::size_t) override
			{
				if (Stopped()) return false;
				++m_depth;
				if (m_arrayDepth && m_depth == m_arrayDepth + 1) { m_messageDepth = m_depth; m_value = nlohmann::json::object(); }
				else if (m_messageDepth && m_depth == m_messageDepth + 1 && m_key == "tokens") { m_tokensDepth = m_depth; m_value["tokens"] = nlohmann::json::object(); }
				return true;
			}
			bool key(string_t& value) override { if (Stopped()) return false; m_key = value; return true; }
			bool end_object() override
			{
				if (m_depth == m_messageDepth)
				{
					if (message && !message(m_value)) return false;
					m_value = nullptr; m_messageDepth = 0;
				}
				if (m_depth == m_tokensDepth) m_tokensDepth = 0;
				--m_depth; return !Stopped();
			}
			bool start_array(std::size_t) override
			{
				++m_depth;
				if (m_depth == 2 && m_key == "messages") { saw_messages = true; m_arrayDepth = m_depth; }
				return !Stopped();
			}
			bool end_array() override { if (m_depth == m_arrayDepth) m_arrayDepth = 0; --m_depth; return !Stopped(); }
			bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }
		private:
			bool Stopped() const { return cancelled && cancelled(); }
			bool Scalar(const nlohmann::json& value)
			{
				if (Stopped()) return false;
				if (m_messageDepth && m_depth == m_messageDepth && (m_key == "type" || m_key == "id" || m_key == "timestamp" || m_key == "model")) m_value[m_key] = value;
				else if (m_tokensDepth && m_depth == m_tokensDepth && (m_key == "input" || m_key == "cached" || m_key == "output" || m_key == "thoughts" || m_key == "tool" || m_key == "total")) m_value["tokens"][m_key] = value;
				return true;
			}
			int m_depth = 0, m_arrayDepth = 0, m_messageDepth = 0, m_tokensDepth = 0;
			std::string m_key;
			nlohmann::json m_value;
		};
	}

	TranscriptScan::TranscriptScan(std::string provider_id) : m_providerId(std::move(provider_id)) {}

	void TranscriptScan::Reset()
	{
		m_seenSize = 0;
		m_readOffset = 0;
		m_prefixGuard.clear();
		m_tailGuard.clear();
		m_pendingLine.clear();
		m_pendingOversized = false;
		m_recordLimitReached = false;
		m_invalidLines = 0;
		m_oversizedLines = 0;
		m_codexState = {};
		m_copilotState = {};
		m_geminiSessionId.clear();
		m_records.clear(); m_recordBase = 0;
	}

	bool TranscriptScan::CommitLine(const ScanOptions& options)
	{
		if (m_pendingOversized) ++m_oversizedLines;
		else if (!m_pendingLine.empty() && m_pendingLine != "\r")
		{
			const nlohmann::json event = nlohmann::json::parse(m_pendingLine, nullptr, false);
			if (event.is_discarded()) ++m_invalidLines;
			else if (m_providerId == provider_ids::kCopilotCli)
			{
				CopilotScanState next = m_copilotState;
				std::vector<Record> records = CopilotRecords(event, m_path.parent_path().filename().string(), next);
				if (records.size() > options.max_records - m_records.size()) { m_recordLimitReached = true; return false; }
				m_copilotState = std::move(next);
				for (Record& record : records) m_records.push_back(std::move(record));
			}
			else if (m_providerId == provider_ids::kGeminiCli)
			{
				if (!CommitGeminiEvent(event, options)) return false;
			}
			else
			{
				const CodexScanState previous_state = m_codexState;
				std::optional<Record> record = m_providerId == provider_ids::kCodexCli ? CodexRecord(event, m_codexState) : ClaudeRecord(event);
				if (record)
				{
					if (m_records.size() >= options.max_records)
					{
						m_codexState = previous_state;
						m_recordLimitReached = true;
						return false;
					}
					m_records.push_back(std::move(*record));
				}
			}
		}
		m_pendingLine.clear();
		m_pendingOversized = false;
		return true;
	}

	bool TranscriptScan::CommitGeminiEvent(const nlohmann::json& event, const ScanOptions& options)
	{
		const nlohmann::json* metadata = nlohmann_json::FindObjectField(event, "$set");
		if (!metadata) metadata = &event;
		const std::string_view identity = nlohmann_json::StringViewOrEmpty(*metadata, "sessionId");
		if (!identity.empty() && identity.size() <= 512) m_geminiSessionId = identity;
		const nlohmann::json* messages = nlohmann_json::FindArrayField(*metadata, "messages");
		const std::size_t count = messages ? messages->size() : 1;
		for (std::size_t index = 0; index < count; ++index)
		{
			const nlohmann::json& message = messages ? (*messages)[index] : event;
			std::optional<Record> record = GeminiRecord(message, m_geminiSessionId);
			if (!record) continue;
			if (m_records.size() >= options.max_records) { m_recordLimitReached = true; return false; }
			m_records.push_back(std::move(*record));
		}
		return true;
	}

	ScanResult TranscriptScan::RefreshGeminiSnapshot(const std::filesystem::path& path, const ScanOptions& options, const std::function<bool()>& cancelled)
	{
		ScanResult result;
		if (cancelled && cancelled()) { result.cancelled = true; return result; }
		std::filesystem::path native_path;
		std::string native_error;
		if (!io::NativeFilePath(path, native_path, native_error)) { result.error = "Could not resolve Gemini history path."; return result; }
		std::error_code error;
		const std::uintmax_t size = std::filesystem::file_size(native_path, error);
		if (error) { result.error = "Could not inspect Gemini history."; return result; }
		const std::filesystem::file_time_type modified = std::filesystem::last_write_time(native_path, error);
		if (error) { result.error = "Could not inspect Gemini history timestamp."; return result; }
		if (m_path != path || size != m_seenSize || modified != m_lastWriteTime)
		{
			Reset(); m_path = path; m_seenSize = size; m_lastWriteTime = modified; result.reset = true;
		}
		// Legacy sessions rewrite a JSON document. Bound its memory separately from incremental JSONL lines.
		if (size > 32 * 1024 * 1024) { m_oversizedLines = 1; result.error = "Gemini history exceeds the 32 MiB snapshot limit; totals are incomplete."; return result; }
		if (m_recordLimitReached) { result.error = "Gemini history exceeds the cached-record limit; totals are incomplete."; return result; }
		if (m_readOffset == size && m_pendingLine.empty() && !result.reset) { result.complete = true; return result; }
#if defined(_WIN32)
		// A provider may atomically replace this snapshot while its usage is being scanned.
		// Keep the old handle readable without preventing replacement; the revision check
		// below rejects a mixed or superseded snapshot before accounting is published.
		struct SnapshotHandle
		{
			HANDLE value;
			~SnapshotHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
		} input{CreateFileW(native_path.c_str(), GENERIC_READ,
		    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		    FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
		LARGE_INTEGER offset{}; offset.QuadPart = static_cast<LONGLONG>(m_readOffset);
		if (input.value == INVALID_HANDLE_VALUE || !SetFilePointerEx(input.value, offset, nullptr, FILE_BEGIN))
		{ result.error = "Could not open Gemini history."; return result; }
#else
		std::ifstream input;
		if (!io::OpenReadFile(path, input)) { result.error = "Could not open Gemini history."; return result; }
		input.seekg(static_cast<std::streamoff>(m_readOffset));
#endif
		std::array<char, 32768> buffer{};
		while (m_readOffset < size && result.bytes_read < options.byte_budget)
		{
			if (cancelled && cancelled()) { result.cancelled = true; return result; }
			const std::size_t requested = static_cast<std::size_t>(std::min<std::uintmax_t>({buffer.size(), size - m_readOffset, options.byte_budget - result.bytes_read}));
#if defined(_WIN32)
			DWORD count = 0;
			if (!ReadFile(input.value, buffer.data(), static_cast<DWORD>(requested), &count, nullptr))
			{ result.error = "Gemini history could not be read during scanning."; return result; }
			const std::size_t received = count;
#else
			input.read(buffer.data(), static_cast<std::streamsize>(requested));
			const std::size_t received = static_cast<std::size_t>(input.gcount());
#endif
			if (!received) { result.error = "Gemini history changed or could not be read during scanning."; return result; }
			m_pendingLine.append(buffer.data(), received); m_readOffset += received; result.bytes_read += received;
		}
		if (m_readOffset != size) return result;
		const std::uintmax_t final_size = std::filesystem::file_size(native_path, error);
		if (error) { result.error = "Could not verify Gemini history after reading."; return result; }
		const std::filesystem::file_time_type final_modified = std::filesystem::last_write_time(native_path, error);
		if (error || final_size != size || final_modified != modified)
		{
			Reset(); result.reset = true; result.error = "Gemini history changed during scanning. Refresh to retry."; return result;
		}
		const nlohmann::json event = nlohmann::json::parse(m_pendingLine, nullptr, false);
		m_pendingLine.clear();
		if (event.is_discarded() || !nlohmann_json::FindArrayField(event, "messages") || nlohmann_json::StringViewOrEmpty(event, "sessionId").empty())
		{
			++m_invalidLines; result.error = "Gemini history is incomplete or invalid."; return result;
		}
		if (!CommitGeminiEvent(event, options)) { result.error = "Gemini history exceeds the cached-record limit; totals are incomplete."; return result; }
		result.complete = true;
		return result;
	}

	ScanResult TranscriptScan::Refresh(const std::filesystem::path& path, const ScanOptions& options, const std::function<bool()>& cancelled, const std::function<bool(const std::vector<Record>&, std::size_t, std::string&)>& snapshot_sink)
	{
		ScanResult result;
		if (m_providerId != provider_ids::kCodexCli && m_providerId != provider_ids::kClaudeCli && m_providerId != provider_ids::kGeminiCli && m_providerId != provider_ids::kCopilotCli)
		{
			result.error = "Historical transcript usage is unavailable for this provider.";
			return result;
		}
		if (options.byte_budget == 0 || options.byte_budget > 64 * 1024 * 1024 || options.max_line_bytes == 0 ||
		    options.max_line_bytes > 8 * 1024 * 1024 || options.max_records == 0 || options.max_records > 100000)
		{
			result.error = "Invalid usage scan limits.";
			return result;
		}
				if (m_providerId == provider_ids::kGeminiCli && path.extension() == ".json" && snapshot_sink)
		{
			std::error_code error;
			const std::uintmax_t size = std::filesystem::file_size(path, error);
			if (error) { result.error = "Could not inspect Gemini history."; return result; }
			const std::filesystem::file_time_type modified = std::filesystem::last_write_time(path, error);
			if (error) { result.error = "Could not inspect Gemini history timestamp."; return result; }
			if (m_path == path && size == m_seenSize && modified == m_lastWriteTime && m_readOffset == size) { result.complete = true; return result; }
			Reset(); m_path = path; result.reset = true;
			std::ifstream input;
			if (!io::OpenReadFile(path, input)) { result.error = "Could not open Gemini history."; return result; }
			GeminiProjection header(cancelled);
			if (!nlohmann::json::sax_parse(input, &header) || header.session.empty() || !header.saw_messages)
			{ result.cancelled = cancelled && cancelled(); if (!result.cancelled) result.error = "Gemini history is incomplete or invalid."; return result; }
			input.clear(); input.seekg(0);
			std::vector<Record> batch;
			const std::function<bool()> flush = [&]()
			{
				if (batch.empty()) return true;
				if (!snapshot_sink(batch, m_recordBase, result.error)) return false;
				m_recordBase += batch.size(); batch.clear(); return true;
			};
			GeminiProjection projection(cancelled, [&](const nlohmann::json& message)
			{
				std::optional<Record> record = GeminiRecord(message, header.session);
				if (record) batch.push_back(std::move(*record));
				return batch.size() < 2048 || flush();
			});
			if (!nlohmann::json::sax_parse(input, &projection) || !flush())
			{ result.cancelled = cancelled && cancelled(); if (!result.cancelled && result.error.empty()) result.error = "Gemini history is incomplete or invalid."; return result; }
			const std::uintmax_t final_size = std::filesystem::file_size(path, error);
			const std::filesystem::file_time_type final_modified = std::filesystem::last_write_time(path, error);
			if (error || final_size != size || final_modified != modified) { result.error = "Gemini history changed during scanning. Refresh to retry."; return result; }
			m_seenSize = size; m_lastWriteTime = modified; m_readOffset = size; result.bytes_read = size; result.complete = true;
			return result;
		}
		if (m_providerId == provider_ids::kGeminiCli && path.extension() == ".json") return RefreshGeminiSnapshot(path, options, cancelled);
		if (cancelled && cancelled()) { result.cancelled = true; return result; }
		std::filesystem::path native_path;
		std::string native_error;
		if (!io::NativeFilePath(path, native_path, native_error)) { result.error = "Could not resolve the usage transcript path."; return result; }
		std::error_code error;
		const std::uintmax_t size = std::filesystem::file_size(native_path, error);
		if (error || size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamoff>::max()))
		{
			result.error = "Could not inspect the usage transcript.";
			return result;
		}
		const std::filesystem::file_time_type modified = std::filesystem::last_write_time(native_path, error);
		if (error) { result.error = "Could not inspect the usage transcript timestamp."; return result; }
		std::ifstream input;
		if (!io::OpenReadFile(path, input)) { result.error = "Could not open the usage transcript."; return result; }
		const bool changed_path = m_path != path;
		bool rewritten = size < m_readOffset || (!m_path.empty() && size <= m_seenSize && modified != m_lastWriteTime);
		if (!changed_path && !rewritten && m_readOffset > 0)
		{
			// shortcut: provider logs are append-only; prefix/tail guards detect replacement without rehashing the full transcript.
			rewritten = ReadGuard(input, 0, m_prefixGuard.size()) != m_prefixGuard ||
			    ReadGuard(input, m_readOffset - m_tailGuard.size(), m_tailGuard.size()) != m_tailGuard;
		}
		if (changed_path || rewritten)
		{
			Reset();
			m_path = path;
			result.reset = true;
		}
		if (m_recordLimitReached && options.stream_batches && m_records.empty())
		{
			m_recordLimitReached = false;
			if (!CommitLine(options)) { result.error = "Usage receipt cannot fit in a scan batch."; return result; }
		}
		if (m_recordLimitReached)
		{
			result.error = "Usage transcript exceeds the cached-record limit; totals are incomplete.";
			return result;
		}
		input.clear();
		input.seekg(static_cast<std::streamoff>(m_readOffset));
		std::array<char, 32768> buffer{};
		while (m_readOffset < size && result.bytes_read < options.byte_budget)
		{
			if (cancelled && cancelled()) { result.cancelled = true; break; }
			const std::size_t remaining = static_cast<std::size_t>(std::min<std::uintmax_t>(size - m_readOffset, options.byte_budget - result.bytes_read));
			const std::size_t requested = std::min(buffer.size(), remaining);
			input.read(buffer.data(), static_cast<std::streamsize>(requested));
			const std::size_t received = static_cast<std::size_t>(input.gcount());
			if (received == 0) { result.error = "Usage transcript changed or could not be read during scanning."; break; }
			std::size_t cursor = 0;
			while (cursor < received)
			{
				const char* newline = std::find(buffer.data() + cursor, buffer.data() + received, '\n');
				const std::size_t segment_size = static_cast<std::size_t>(newline - (buffer.data() + cursor));
				if (!m_pendingOversized)
				{
					if (segment_size > options.max_line_bytes - m_pendingLine.size())
					{
						m_pendingLine.clear();
						m_pendingOversized = true;
					}
					else m_pendingLine.append(buffer.data() + cursor, segment_size);
				}
				cursor += segment_size;
				m_readOffset += segment_size;
				result.bytes_read += segment_size;
				if (newline == buffer.data() + received) break;
				++cursor; ++m_readOffset; ++result.bytes_read;
				if (!CommitLine(options))
				{
					if (options.stream_batches) result.batch_full = true;
					else result.error = "Usage transcript exceeds the cached-record limit; totals are incomplete.";
					break;
				}
			}
			if (!result.error.empty() || result.batch_full) break;
		}
		m_seenSize = size;
		m_lastWriteTime = modified;
		m_prefixGuard = ReadGuard(input, 0, static_cast<std::size_t>(std::min<std::uintmax_t>(64, m_readOffset)));
		const std::size_t tail_size = static_cast<std::size_t>(std::min<std::uintmax_t>(64, m_readOffset));
		m_tailGuard = ReadGuard(input, m_readOffset - tail_size, tail_size);
		result.complete = result.error.empty() && !result.cancelled && m_readOffset == size && m_pendingLine.empty() && !m_pendingOversized;
		return result;
	}
}

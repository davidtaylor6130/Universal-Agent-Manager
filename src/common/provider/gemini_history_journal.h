#pragma once

#include <nlohmann/json.hpp>
#include "common/utils/io_utils.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <stop_token>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace uam::gemini
{
	// Gemini 0.64.0 ChatRecordingService's append-only record grammar. Retain the
	// current conversation, not every obsolete version of every appended record.
	class HistoryJournal
	{
	  public:
		using Json = nlohmann::json;
		explicit HistoryJournal(bool include_messages = true) : include_messages_(include_messages) {}

		bool Apply(std::string_view line, bool allow_incomplete_tail = false)
		{
			if (line.find_first_not_of(" \t\r\n") == std::string_view::npos) return true;
			Json record = Json::parse(line.begin(), line.end(), nullptr, false);
			if (record.is_discarded()) return allow_incomplete_tail;
			return ApplyRecord(std::move(record));
		}

		bool ApplyRecord(Json record)
		{
			if (!record.is_object()) return false;
			if (const auto rewind = record.find("$rewindTo"); rewind != record.end() && rewind->is_string())
			{
				if (!include_messages_) return true;
				const auto found = std::ranges::find(order_, rewind->get_ref<const std::string&>());
				const auto first = found == order_.end() ? order_.begin() : found;
				for (auto item = first; item != order_.end(); ++item) messages_.erase(*item);
				order_.erase(first, order_.end());
			}
			else if (const auto patch = record.find("$patch"); patch != record.end() && patch->is_object())
			{
				if (!include_messages_) return true;
				Patch(*patch);
				if (const auto updates = patch->find("updates"); updates != patch->end() && updates->is_array())
					for (const auto& update : *updates) if (update.is_object()) Patch(update);
				if (const auto remove = patch->find("removeIds"); remove != patch->end() && remove->is_array())
				{
					for (const auto& id : *remove) if (id.is_string()) messages_.erase(id.get_ref<const std::string&>());
					std::erase_if(order_, [&](const auto& id) { return !messages_.contains(id); });
				}
				if (const auto ordered = patch->find("orderIds"); ordered != patch->end() && ordered->is_array())
				{
					std::unordered_set<std::string> selected;
					std::vector<std::string> suffix;
					for (const auto& value : *ordered)
						if (value.is_string())
						{
							const auto& id = value.get_ref<const std::string&>();
							if (messages_.contains(id) && selected.insert(id).second) suffix.push_back(id);
						}
					std::erase_if(order_, [&](const auto& id) { return selected.contains(id); });
					order_.insert(order_.end(), suffix.begin(), suffix.end());
				}
			}
			else if (const auto id = record.find("id"); id != record.end() && id->is_string()) Put(std::move(record));
			else if (const auto update = record.find("$set"); update != record.end() && update->is_object())
			{
				if (const auto replacement = update->find("messages"); include_messages_ && replacement != update->end() && replacement->is_array())
				{
					messages_.clear(); order_.clear();
					for (auto& message : *replacement) Put(std::move(message));
				}
				return Metadata(*update);
			}
			else if (record.contains("sessionId") || record.contains("projectHash") || record.contains("parentSessionId"))
			{
				if (const auto initial = record.find("messages"); include_messages_ && initial != record.end() && initial->is_array())
					for (auto& message : *initial) Put(std::move(message));
				return Metadata(record);
			}
			return true;
		}

		std::optional<Json> Finish()
		{
			if (!metadata_.contains("sessionId") || !metadata_["sessionId"].is_string() ||
			    metadata_["sessionId"].get_ref<const std::string&>().empty() ||
			    !metadata_.contains("projectHash") || !metadata_["projectHash"].is_string() ||
			    metadata_["projectHash"].get_ref<const std::string&>().empty()) return std::nullopt;
			if (include_messages_)
			{
				Json messages = Json::array();
				for (const auto& id : order_) messages.push_back(std::move(messages_.at(id)));
				metadata_["messages"] = std::move(messages);
			}
			return std::move(metadata_);
		}

	  private:
		bool include_messages_;
		Json metadata_ = Json::object();
		std::unordered_map<std::string, Json> messages_;
		std::vector<std::string> order_;

		void Put(Json message)
		{
			if (!include_messages_ || !message.is_object() || message.contains("$patch")) return;
			const auto id = message.find("id");
			if (id == message.end() || !id->is_string()) return;
			const auto key = id->get<std::string>();
			if (!messages_.contains(key)) order_.push_back(key);
			messages_.insert_or_assign(key, std::move(message));
		}

		bool Metadata(const Json& record)
		{
			for (const auto* key : {"sessionId", "projectHash", "parentSessionId"})
			{
				const auto value = record.find(key);
				const auto existing = metadata_.find(key);
				if (value == record.end()) continue;
				if (!value->is_string() || value->get_ref<const std::string&>().empty() ||
				    (existing != metadata_.end() && *value != *existing)) return false;
			}
			for (auto item = record.begin(); item != record.end(); ++item)
				if (item.key() != "messages") metadata_[item.key()] = item.value();
			return true;
		}

		void Patch(const Json& patch)
		{
			const auto id = patch.find("id");
			if (id == patch.end() || !id->is_string()) return;
			const auto found = messages_.find(id->get_ref<const std::string&>());
			if (found == messages_.end()) return;
			auto& message = found->second;
			if (patch.contains("content")) message["content"] = patch["content"];
			if (!message.contains("type") || !message["type"].is_string() || message["type"] != "gemini" || !message.contains("toolCalls") || !message["toolCalls"].is_array() ||
			    !patch.contains("toolCalls") || !patch["toolCalls"].is_array()) return;
			for (const auto& update : patch["toolCalls"])
			{
				if (!update.is_object() || !update.contains("id") || !update["id"].is_string() || !update.contains("result")) continue;
				for (auto& tool : message["toolCalls"])
					if (tool.is_object() && tool.contains("id") && tool["id"] == update["id"]) { tool["result"] = update["result"]; break; }
			}
		}
	};

	inline std::optional<nlohmann::json> FoldHistoryJournal(std::string_view text, bool include_messages = true)
	{
		HistoryJournal journal(include_messages);
		std::size_t offset = 0;
		while (offset < text.size())
		{
			const auto end = text.find('\n', offset);
			const bool tail = end == std::string_view::npos;
			if (!journal.Apply(text.substr(offset, tail ? text.size() - offset : end - offset), tail)) return std::nullopt;
			if (tail) break;
			offset = end + 1;
		}
		return journal.Finish();
	}

	namespace detail
	{
		// The boundary belongs to this opened stream, never an earlier pathname target.
		inline std::optional<nlohmann::json> ReadHistoryStream(std::ifstream& input, std::uintmax_t max_bytes,
		    bool include_messages, bool allow_legacy, std::stop_token stop_token = {})
		{
			input.seekg(0, std::ios::end);
			const auto end_position = input.tellg();
			if (end_position < std::streampos(0)) return std::nullopt;
			const auto initial_size = static_cast<std::uintmax_t>(static_cast<std::streamoff>(end_position));
			if (max_bytes > 0 && initial_size > max_bytes) return std::nullopt;
			input.seekg(0, std::ios::beg);
			if (!input) return std::nullopt;
			HistoryJournal journal(include_messages);
			std::optional<nlohmann::json> single_document;
			bool first_record = true, legacy_fallback = false;
			const auto consume = [&](std::string_view line, bool tail) -> bool
			{
				if (line.find_first_not_of(" \t\r\n") == line.npos) return true;
				if (single_document)
				{
					if (!journal.ApplyRecord(std::move(*single_document))) return false;
					single_document.reset();
				}
				if (allow_legacy && first_record)
				{
					first_record = false;
					auto record = nlohmann::json::parse(line.begin(), line.end(), nullptr, false);
					if (record.is_discarded())
					{
						// Only failure of the first record can be a pretty legacy document.
						// Never reinterpret a corrupt journal interior as a legacy rewrite.
						legacy_fallback = !tail;
						return tail;
					}
					if (record.is_object() && record.contains("sessionId") && !record.contains("id"))
					{
						single_document = std::move(record);
						return true;
					}
					return journal.ApplyRecord(std::move(record));
				}
				return journal.Apply(line, tail);
			};
			std::array<char, 64 * 1024> buffer;
			std::uintmax_t remaining = initial_size;
			std::string pending;
			std::size_t scanned = 0;
			while (remaining > 0 && !legacy_fallback)
			{
				if (stop_token.stop_requested()) return std::nullopt;
				const auto count = static_cast<std::streamsize>(std::min<std::uintmax_t>(buffer.size(), remaining));
				input.read(buffer.data(), count);
				if (input.gcount() != count) return std::nullopt;
				remaining -= static_cast<std::uintmax_t>(count);
				pending.append(buffer.data(), static_cast<std::size_t>(count));
				std::size_t offset = 0;
				while (true)
				{
					const auto end = pending.find('\n', scanned);
					if (end == std::string::npos) break;
					if (!consume(std::string_view(pending).substr(offset, end - offset), false))
					{
						if (legacy_fallback) break;
						return std::nullopt;
					}
					offset = end + 1;
					scanned = offset;
				}
				// The pending prefix was already searched; scan only newly appended bytes.
				pending.erase(0, offset);
				scanned = pending.size();
			}
			if (legacy_fallback)
			{
				input.clear();
				input.seekg(0, std::ios::beg);
				if (!input || initial_size > std::string{}.max_size()) return std::nullopt;
				std::string text;
				while (text.size() < initial_size)
				{
					if (stop_token.stop_requested()) return std::nullopt;
					const auto count = static_cast<std::streamsize>(std::min<std::uintmax_t>(buffer.size(), initial_size - text.size()));
					input.read(buffer.data(), count);
					if (input.gcount() != count) return std::nullopt;
					text.append(buffer.data(), static_cast<std::size_t>(count));
				}
				auto document = nlohmann::json::parse(text, nullptr, false);
				if (!document.is_object()) return std::nullopt;
				single_document = std::move(document);
			}
			else if (!pending.empty() && !consume(pending, true)) return std::nullopt;
			if (stop_token.stop_requested()) return std::nullopt;
			if (single_document)
			{
				for (const auto* key : {"sessionId", "projectHash", "parentSessionId"})
					if (const auto value = single_document->find(key); value != single_document->end() &&
					    (!value->is_string() || value->get_ref<const std::string&>().empty())) return std::nullopt;
				if (!include_messages) single_document->erase("messages");
				return single_document;
			}
			return journal.Finish();
		}

		template <typename OpenFile>
		inline std::optional<nlohmann::json> ReadHistoryFile(const std::filesystem::path& path, std::uintmax_t max_bytes,
		    bool include_messages, bool allow_legacy, OpenFile&& open_file, std::stop_token stop_token = {})
		{
			if (stop_token.stop_requested()) return std::nullopt;
			std::ifstream input;
			if (!open_file(path, input) || stop_token.stop_requested()) return std::nullopt;
			return ReadHistoryStream(input, max_bytes, include_messages, allow_legacy, stop_token);
		}
	}

	template <typename OpenFile = decltype(&io::OpenReadFile)>
	inline std::optional<nlohmann::json> FoldHistoryJournalFile(const std::filesystem::path& path,
	    std::uintmax_t max_bytes = 0, bool include_messages = true, OpenFile open_file = &io::OpenReadFile)
	{
		return detail::ReadHistoryFile(path, max_bytes, include_messages, false, open_file);
	}

	inline std::optional<nlohmann::json> ReadHistoryDocumentFile(const std::filesystem::path& path,
	    std::uintmax_t max_bytes = 0, bool include_messages = true, std::stop_token stop_token = {})
	{
		return detail::ReadHistoryFile(path, max_bytes, include_messages, true, &io::OpenReadFile, stop_token);
	}

	inline std::optional<nlohmann::json> ReadHistoryMetadata(std::string_view text)
	{
		auto legacy = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
		if (legacy.is_object()) return legacy;
		return FoldHistoryJournal(text, false);
	}
	inline std::optional<nlohmann::json> ReadHistoryMetadataFile(const std::filesystem::path& path, std::stop_token stop_token = {})
	{
		return ReadHistoryDocumentFile(path, 0, false, stop_token);
	}

} // namespace uam::gemini

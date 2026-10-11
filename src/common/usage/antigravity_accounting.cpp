#include "common/usage/antigravity_accounting.h"
#include "common/utils/string_utils.h"
#include <algorithm>
#include <functional>
#include <cmath>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <set>
#include <unordered_map>

namespace uam::usage
{
	namespace
	{
		struct Field
		{
			std::uint32_t number;
			unsigned wire;
			std::uint64_t integer = 0;
			std::string_view bytes;
		};
		class Fields
		{
		public:
			explicit Fields(std::string_view bytes)
			{
				if (bytes.size() > 8 * 1024 * 1024) throw std::runtime_error("Oversized metadata");
				std::size_t position = 0;
				const std::function<std::uint64_t()> varint = [&]()
				{
					std::uint64_t value = 0;
					for (unsigned index = 0; index < 10; ++index)
					{
						if (position == bytes.size()) throw std::runtime_error("Truncated varint");
						const unsigned char byte = static_cast<unsigned char>(bytes[position++]);
						if (index == 9 && byte > 1) throw std::runtime_error("Overflowing varint");
						value |= std::uint64_t(byte & 127) << (index * 7);
						if (byte < 128) return value;
					}
					throw std::runtime_error("Overflowing varint");
				};
				while (position < bytes.size())
				{
					const std::uint64_t tag = varint();
					if (tag < 8 || tag / 8 > 536870911 || m_values.size() == 4096) throw std::runtime_error("Invalid fields");
					Field field{static_cast<std::uint32_t>(tag / 8), static_cast<unsigned>(tag % 8)};
					if (field.wire == 0) field.integer = varint();
					else if (field.wire == 1 || field.wire == 2 || field.wire == 5)
					{
						const std::uint64_t length = field.wire == 2 ? varint() : field.wire == 1 ? 8 : 4;
						if (length > bytes.size() - position) throw std::runtime_error("Truncated field");
						field.bytes = bytes.substr(position, static_cast<std::size_t>(length)); position += static_cast<std::size_t>(length);
					}
					else throw std::runtime_error("Unsupported wire type");
					m_values.push_back(field);
				}
			}
			const Field* First(std::uint32_t number) const
			{
				for (const Field& field : m_values) if (field.number == number) return &field;
				return nullptr;
			}
			std::int64_t Number(std::uint32_t number) const
			{
				const Field* field = First(number);
				if (!field) return 0;
				if (field->wire != 0 || field->integer > static_cast<std::uint64_t>(kMaxExactTokenCount)) throw std::runtime_error("Invalid accounting counter");
				return static_cast<std::int64_t>(field->integer);
			}
			std::string_view Bytes(std::uint32_t number) const
			{
				const Field* field = First(number);
				if (!field) return {};
				if (field->wire != 2) throw std::runtime_error("Invalid nested accounting field");
				return field->bytes;
			}
			std::string Text(std::uint32_t number) const
			{
				const std::string_view bytes = Bytes(number);
				if (bytes.size() > 8192 || std::ranges::any_of(bytes, [](unsigned char value) { return value < 32; })) throw std::runtime_error("Invalid accounting text");
				const std::string text = strings::Trim(bytes);
				(void)nlohmann::json(text).dump(); // Reject malformed UTF-8 before creating a persisted identity or model name.
				return text;
			}
			const std::vector<Field>& All() const { return m_values; }
		private:
			std::vector<Field> m_values;
		};

		std::optional<std::int64_t> Timestamp(const Fields& fields)
		{
			const std::int64_t seconds = fields.Number(1), nanos = fields.Number(2);
			if (seconds == 0) return std::nullopt;
			if (seconds > (kMaxExactTokenCount - 999) / 1000 || nanos >= 1000000000) throw std::runtime_error("Invalid timestamp");
			return seconds * 1000 + nanos / 1000000;
		}
		std::string NumericModel(std::int64_t id)
		{
			// Observed Antigravity enum values; an unknown enum keeps its identity and remains unpriced.
			static const std::map<std::int64_t, std::string> models{{246,"gemini-2.5-pro"},{312,"gemini-2.5-flash"},{313,"gemini-2.5-flash-thinking"},{329,"gemini-2.5-flash-thinking"},{330,"gemini-2.5-flash-lite"},
			    {281,"claude-sonnet-4"},{282,"claude-sonnet-4"},{290,"claude-opus-4"},{291,"claude-opus-4"},{333,"claude-sonnet-4-5"},{334,"claude-sonnet-4-5"},{340,"claude-haiku-4-5"},{341,"claude-haiku-4-5"},
			    {1026,"claude-opus-4-6"},{1035,"claude-sonnet-4-6"},{1016,"gemini-3.1-pro"},{1036,"gemini-3.1-pro"},{1037,"gemini-3.1-pro"},{1018,"gemini-3-flash-preview"},{1084,"gemini-3-flash-preview"},{1047,"gemini-3-flash-preview"}};
			const std::map<std::int64_t, std::string>::const_iterator found = models.find(id);
			return found == models.end() ? std::string{} : found->second;
		}
		std::string Model(std::string name, std::int64_t id)
		{
			if (name.empty()) return !NumericModel(id).empty() ? NumericModel(id) : id > 0 ? "antigravity-model-" + std::to_string(id) : "";
			if (name.size() > 512) throw std::runtime_error("Oversized model name");
			if (name.back() == ')')
			{
				const std::size_t suffix = name.rfind('(');
				if (suffix != std::string::npos) name = strings::Trim(name.substr(0, suffix));
			}
			name = strings::TrimAndLowerAscii(name); std::ranges::replace(name, ' ', '-');
			if (name.starts_with("claude-"))
			{
				const std::size_t split = name.find('-', 7), end = split == std::string::npos ? split : name.find('-', split + 1);
				const std::string version = name.substr(7, split == std::string::npos ? split : split - 7);
				const std::string family = split == std::string::npos ? "" : name.substr(split + 1, end == std::string::npos ? end : end - split - 1);
				if ((version == "4" || version.starts_with("4.")) && (family == "sonnet" || family == "opus" || family == "haiku"))
					name = "claude-" + family + "-" + version + (end == std::string::npos ? "" : name.substr(end));
				std::ranges::replace(name, '.', '-');
			}
			return name;
		}
	}

	std::optional<AntigravityMetadata> DecodeAntigravityMetadata(std::string_view bytes, std::string_view session_id,
	    std::int64_t row_index, bool step, std::int64_t fallback_ms, std::optional<std::int64_t> trajectory_ms, std::string_view generation_model)
	{
		try
		{
			if (session_id.empty() || session_id.size() > 8192 || row_index < 0 || fallback_ms < 0 || fallback_ms > kMaxExactTokenCount ||
			    (trajectory_ms && (*trajectory_ms <= 0 || *trajectory_ms > kMaxExactTokenCount)) || generation_model.size() > 512) return std::nullopt;
			(void)nlohmann::json(std::string(session_id)).dump();
			(void)nlohmann::json(std::string(generation_model)).dump();
			const Fields root(bytes);
			if (!step && !root.First(1)) return std::nullopt;
			const Fields data(step ? bytes : root.Bytes(1)), model(step ? data.Bytes(24) : root.Bytes(1));
			AntigravityMetadata decoded;
			decoded.model = Model(!model.Text(step ? 12 : 19).empty() ? model.Text(step ? 12 : 19) : model.Text(step ? 8 : 21), model.Number(step ? 1 : 3));
			decoded.timestamp_ms = step ? Timestamp(Fields(data.Bytes(8))) : Timestamp(Fields(Fields(data.Bytes(9)).Bytes(4)));
			if (step && !decoded.timestamp_ms) decoded.timestamp_ms = Timestamp(Fields(data.Bytes(1)));
			const std::int64_t timestamp = decoded.timestamp_ms.value_or(trajectory_ms.value_or(fallback_ms));
			std::vector<std::pair<std::size_t, std::string_view>> usages;
			std::size_t retry_index = 0;
			if (data.First(step ? 9 : 4)) usages.emplace_back(0, data.Bytes(step ? 9 : 4));
			for (const Field& field : data.All())
				if (field.number == (step ? 28 : 17))
				{
					if (field.wire != 2 || ++retry_index > 256) return std::nullopt;
					const Fields retry(field.bytes);
					if (retry.First(2)) usages.emplace_back(retry_index, retry.Bytes(2));
				}
			for (const std::pair<std::size_t, std::string_view>& part : usages)
			{
				const std::size_t index = part.first;
				const Fields usage(part.second);
				AntigravityUsageCandidate candidate; Record& record = candidate.record;
				record.provider_id = provider_ids::kAntigravityCli; record.session_id = session_id; record.timestamp_ms = timestamp;
				record.tokens.input = usage.Number(2); record.tokens.cached_input = usage.Number(5); record.tokens.cache_write = usage.Number(4);
				const std::int64_t reasoning = usage.Number(9), completion = usage.Number(10);
				if (reasoning > kMaxExactTokenCount - completion) return std::nullopt;
				record.tokens.output = std::max(usage.Number(3), reasoning + completion); record.tokens.reasoning = std::min(reasoning, record.tokens.output);
				std::int64_t total = 0;
				for (std::int64_t count : {record.tokens.input, record.tokens.cached_input, record.tokens.cache_write, record.tokens.output})
				{ if (count > kMaxExactTokenCount - total) return std::nullopt; total += count; }
				if (!total) continue;
				const std::int64_t usage_model = usage.Number(1);
				record.model_id = NumericModel(usage_model);
				if (record.model_id.empty() && usage_model > 0 && usage_model != model.Number(step ? 1 : 3)) record.model_id = Model("", usage_model);
				if (record.model_id.empty()) record.model_id = !decoded.model.empty() ? decoded.model : step ? std::string(generation_model) : "";
				if (record.model_id.empty()) record.model_id = Model("", usage.Number(1));
				if (record.model_id.empty()) record.model_id = "antigravity-unknown";
				for (std::uint32_t field : {11, 12, 7})
				{
					const std::string identity = usage.Text(field);
					if (!identity.empty()) candidate.aliases.push_back(nlohmann::json::array({field, identity}).dump());
				}
				candidate.location_id = nlohmann::json::array({"antigravity", session_id, step ? "step" : "generation", row_index, index}).dump();
				record.dedupe_key = candidate.aliases.empty() ? candidate.location_id : "antigravity:" + candidate.aliases.front();
				candidate.timestamp_quality = decoded.timestamp_ms ? 2 : trajectory_ms ? 1 : 0;
			candidate.is_step = step;
				decoded.candidates.push_back(std::move(candidate));
			}
			return decoded;
		}
		catch (const std::exception&) { return std::nullopt; }
	}

	std::optional<std::vector<Record>> MergeAntigravityCandidates(const std::vector<const AntigravityUsageCandidate*>& candidates, std::vector<std::size_t>* group_owners)
	{
		if (candidates.size() > 500000) return std::nullopt;
		struct Group
		{
			std::size_t parent, size = 1, owner;
			int quality;
			std::size_t model_owner;
			bool step_model;
			Record record;
		};
		std::vector<Group> groups;
		std::unordered_map<std::string, std::size_t> identities, identified_locations;
		std::size_t bytes = 0;
		const std::function<std::size_t(std::size_t)> root = [&](std::size_t index)
		{
			std::size_t parent = index;
			while (groups[parent].parent != parent) parent = groups[parent].parent;
			while (index != parent) { const std::size_t next = groups[index].parent; groups[index].parent = parent; index = next; }
			return parent;
		};
		for (const AntigravityUsageCandidate* candidate : candidates)
		{
			if (!candidate || candidate->record.provider_id != provider_ids::kAntigravityCli || candidate->record.dedupe_key.empty()) return std::nullopt;
			const Record& record = candidate->record;
			const std::size_t weight = sizeof(Group) + record.model_id.size() + record.session_id.size() + record.dedupe_key.size() + candidate->location_id.size();
			if (weight > 64 * 1024 * 1024 - bytes) return std::nullopt;
			bytes += weight;
			const std::size_t index = groups.size(); groups.push_back({index, 1, index, candidate->timestamp_quality, index, candidate->is_step, record});
			std::vector<std::string> aliases = candidate->aliases;
			if (aliases.empty() && !candidate->location_id.empty()) aliases.push_back("location:" + candidate->location_id);
			else if (!candidate->location_id.empty())
			{
				const std::pair<std::unordered_map<std::string, std::size_t>::iterator, bool> location = identified_locations.try_emplace(candidate->location_id, index);
				if (!location.second && record.timestamp_ms < candidates[location.first->second]->record.timestamp_ms) location.first->second = index;
			}
			if (aliases.empty()) aliases.push_back("fallback:" + record.dedupe_key);
			for (const std::string& alias : aliases)
			{
				if (alias.empty() || alias.size() > 16384 || alias.size() > 64 * 1024 * 1024 - bytes) return std::nullopt;
				bytes += alias.size();
				const std::unordered_map<std::string, std::size_t>::const_iterator existing = identities.find(alias);
				if (existing != identities.end())
				{
					std::size_t left = root(index), right = root(existing->second);
					if (left != right)
					{
						if (groups[left].size < groups[right].size) std::swap(left, right);
						Group& target = groups[left]; Group& source = groups[right];
						const Group& first = target.owner < source.owner ? target : source;
						const Group& best_time = source.quality > target.quality || (source.quality == target.quality && source.record.timestamp_ms < target.record.timestamp_ms) ? source : target;
						const Group* model = source.step_model > target.step_model || (source.step_model == target.step_model && source.model_owner < target.model_owner) ? &source : &target;
						const Group* other = model == &target ? &source : &target;
						if (model->record.model_id == "antigravity-unknown" && other->record.model_id != "antigravity-unknown") model = other;
						Record merged = first.record; merged.timestamp_ms = best_time.record.timestamp_ms; merged.model_id = model->record.model_id;
						const std::size_t model_owner = model->model_owner; const bool step_model = model->step_model;
						merged.tokens.input = std::max(target.record.tokens.input, source.record.tokens.input);
						merged.tokens.cached_input = std::max(target.record.tokens.cached_input, source.record.tokens.cached_input);
						merged.tokens.cache_write = std::max(target.record.tokens.cache_write, source.record.tokens.cache_write);
						merged.tokens.output = std::max(target.record.tokens.output, source.record.tokens.output);
						merged.tokens.reasoning = std::min(merged.tokens.output, std::max(target.record.tokens.reasoning, source.record.tokens.reasoning));
						merged.dedupe_key = std::min(target.record.dedupe_key, source.record.dedupe_key);
						const int quality = best_time.quality; const std::size_t owner = first.owner;
						target.record = std::move(merged); target.quality = quality; target.owner = owner; target.model_owner = model_owner; target.step_model = step_model; target.size += source.size; source.parent = left;
					}
				}
				identities[alias] = index;
			}
		}
		// A provisional row can become identified, but its location must not bridge different billed requests after regeneration.
		for (std::size_t index = 0; index < candidates.size(); ++index)
		{
			const AntigravityUsageCandidate& candidate = *candidates[index];
			if (!candidate.aliases.empty() || candidate.location_id.empty()) continue;
			const std::unordered_map<std::string, std::size_t>::const_iterator identified = identified_locations.find(candidate.location_id);
			if (identified == identified_locations.end()) continue;
			const std::size_t provisional_root = root(index), identified_root = root(identified->second);
			if (provisional_root == identified_root) continue;
			Group& provisional = groups[provisional_root]; Group& target = groups[identified_root];
			if (provisional.quality > target.quality || (provisional.quality == target.quality && provisional.record.timestamp_ms < target.record.timestamp_ms))
			{ target.record.timestamp_ms = provisional.record.timestamp_ms; target.quality = provisional.quality; }
			provisional.parent = identified_root;
		}
		std::vector<Record> records;
		for (std::size_t index = 0; index < groups.size(); ++index)
			if (groups[index].parent == index)
			{
				std::int64_t total = 0;
				for (std::int64_t count : {groups[index].record.tokens.input, groups[index].record.tokens.cached_input, groups[index].record.tokens.cache_write, groups[index].record.tokens.output})
				{ if (count < 0 || count > kMaxExactTokenCount - total) return std::nullopt; total += count; }
				records.push_back(std::move(groups[index].record));
			}
		if (group_owners)
		{
			group_owners->clear(); group_owners->reserve(groups.size());
			for (std::size_t index = 0; index < groups.size(); ++index) group_owners->push_back(groups[root(index)].owner);
		}
		return records;
	}

	std::optional<Record> EncodeAntigravityAccountingRecord(const AntigravityUsageCandidate& candidate)
	{
		try
		{
			Record record = candidate.record;
			std::vector<std::string> aliases = candidate.aliases;
			std::sort(aliases.begin(), aliases.end());
			aliases.erase(std::unique(aliases.begin(), aliases.end()), aliases.end());
			record.dedupe_key = nlohmann::json::array({"antigravity-accounting-v1", candidate.timestamp_quality, aliases, candidate.location_id}).dump();
			return DecodeAntigravityAccountingRecord(record) ? std::optional<Record>{std::move(record)} : std::nullopt;
		}
		catch (const nlohmann::json::exception&) { return std::nullopt; }
	}

	std::optional<AntigravityUsageCandidate> DecodeAntigravityAccountingRecord(const Record& record)
	{
		if (record.provider_id != provider_ids::kAntigravityCli || record.dedupe_key.size() > 8192 || !record.dedupe_key.starts_with(kAntigravityAccountingPrefix) ||
		    record.timestamp_ms < 0 || record.timestamp_ms >= kMaxExactTokenCount || record.model_id.empty() || record.model_id.size() > 512 || record.session_id.size() > 8192 ||
		    record.record_count != 1 || record.unidentified_records || record.speed != "standard") return std::nullopt;
		std::int64_t total = 0;
		for (const std::int64_t count : {record.tokens.input, record.tokens.cached_input, record.tokens.cache_write, record.tokens.output})
		{ if (count < 0 || count > kMaxExactTokenCount - total) return std::nullopt; total += count; }
		if (record.tokens.reasoning < 0 || record.tokens.reasoning > record.tokens.output) return std::nullopt;
		for (const std::optional<double> cost : {record.reported_cost_usd, record.estimated_cost_usd})
			if (cost && (!std::isfinite(*cost) || *cost < 0)) return std::nullopt;
		try
		{
			const nlohmann::json key = nlohmann::json::parse(record.dedupe_key);
			if (!key.is_array() || key.size() != 4 || key[0] != "antigravity-accounting-v1" || !key[1].is_number_integer() || key[1] < 0 || key[1] > 2 ||
			    !key[2].is_array() || key[2].size() > 3 || !key[3].is_string() || key.dump() != record.dedupe_key) return std::nullopt;
			AntigravityUsageCandidate candidate;
			candidate.record = record; candidate.timestamp_quality = key[1].get<int>(); candidate.location_id = key[3].get<std::string>();
			const nlohmann::json location = nlohmann::json::parse(candidate.location_id, nullptr, false);
			if (!location.is_array() || location.size() != 5 || location[0] != "antigravity" || location[1] != record.session_id || (location[2] != "step" && location[2] != "generation") ||
			    !location[3].is_number_integer() || location[3] < 0 || location[3] > kMaxExactTokenCount || !location[4].is_number_integer() || location[4] < 0 || location[4] > 256 || location.dump() != candidate.location_id) return std::nullopt;
			candidate.is_step = location[2] == "step";
			std::set<int> kinds;
			for (const nlohmann::json& value : key[2])
			{
				if (!value.is_string()) return std::nullopt;
				const std::string alias = value.get<std::string>();
				const nlohmann::json native = nlohmann::json::parse(alias, nullptr, false);
				if (!native.is_array() || native.size() != 2 || !native[0].is_number_integer() || (native[0] != 7 && native[0] != 11 && native[0] != 12) ||
				    !native[1].is_string() || native[1].get_ref<const std::string&>().empty() || native.dump() != alias || !kinds.insert(native[0].get<int>()).second) return std::nullopt;
				if (std::ranges::any_of(native[1].get_ref<const std::string&>(), [](unsigned char value) { return value < 32; })) return std::nullopt;
				candidate.aliases.push_back(alias);
			}
			if (!std::is_sorted(candidate.aliases.begin(), candidate.aliases.end())) return std::nullopt;
			return candidate;
		}
		catch (const nlohmann::json::exception&) { return std::nullopt; }
	}

	bool DecodeAntigravityTrajectoryTimestamp(std::string_view bytes, std::optional<std::int64_t>& timestamp_ms)
	{
		timestamp_ms.reset();
		try { timestamp_ms = Timestamp(Fields(Fields(bytes).Bytes(2))); return true; }
		catch (const std::exception&) { return false; }
	}
}

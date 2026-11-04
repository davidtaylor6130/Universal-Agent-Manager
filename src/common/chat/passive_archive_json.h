#pragma once

// Private, archive-only JSON codec. Typed runtime queue decoding remains separate.
#include <nlohmann/json.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace uam::passive_archive_json
{
	inline constexpr std::size_t kMaxBytes = 16U * 1024U * 1024U;

	inline std::optional<std::string> DecimalMeaning(std::string_view token)
	{
		bool negative = false;
		std::size_t pos = 0;
		if (pos < token.size() && token[pos] == '-') { negative = true; ++pos; }
		std::string digits;
		std::int64_t fraction = 0;
		bool point = false;
		while (pos < token.size() && token[pos] != 'e' && token[pos] != 'E')
		{
			const char c = token[pos++];
			if (c == '.' && !point) { point = true; continue; }
			if (c < '0' || c > '9') return {};
			digits += c;
			if (point) ++fraction;
		}
		std::int64_t exponent = 0;
		if (pos < token.size())
		{
			++pos;
			bool minus = false;
			if (pos < token.size() && (token[pos] == '+' || token[pos] == '-')) minus = token[pos++] == '-';
			if (pos == token.size()) return {};
			while (pos < token.size())
			{
				const char c = token[pos++];
				// Such an exponent cannot describe a supported finite double except zero.
				if (c < '0' || c > '9' || exponent > 1000000) return {};
				exponent = exponent * 10 + c - '0';
			}
			if (minus) exponent = -exponent;
		}
		const auto first = digits.find_first_not_of('0');
		if (first == std::string::npos) return std::string("0");
		digits.erase(0, first);
		exponent -= fraction;
		while (digits.back() == '0') { digits.pop_back(); ++exponent; }
		return (negative ? "-" : "") + digits + "e" + std::to_string(exponent);
	}

	struct Document
	{
		std::string owner;
		bool read_only = false;
		nlohmann::json prompts = nlohmann::json::array();
		std::string canonical;
	};

	class ArchiveSax final : public nlohmann::json_sax<nlohmann::json>
	{
	public:
		Document document;
		bool null() override { return Scalar(nullptr); }
		bool boolean(bool value) override
		{
			if (depth == 1 && root_key == "imported_read_only") document.read_only = value;
			return Scalar(value);
		}
		bool number_integer(number_integer_t value) override { return Scalar(value); }
		bool number_unsigned(number_unsigned_t value) override { return Scalar(value); }
		bool number_float(number_float_t value, const string_t& token) override
		{
			if (!stack.empty())
			{
				if (token.size() > kMaxBytes - charged) return false;
				const auto before = DecimalMeaning(token);
				const auto after = DecimalMeaning(nlohmann::json(value).dump());
				if (!before || before != after) return false;
			}
			return Scalar(value);
		}
		bool string(string_t& value) override
		{
			if (!stack.empty() && value.size() > kMaxBytes - charged) return false;
			if (depth == 1 && root_key == "id")
			{
				if (value.size() > 256) return false;
				document.owner = value;
			}
			return Scalar(value);
		}
		bool binary(binary_t&) override { return false; }
		bool start_object(std::size_t) override { return Start(false); }
		bool start_array(std::size_t) override { return Start(true); }
		bool key(string_t& value) override
		{
			if (depth == 1)
			{
				root_key = value;
				if ((value == "id" || value == "imported_read_only" || value == "acp_queued_prompts") &&
				    !authority_keys.insert(value).second) return false;
			}
			if (!stack.empty())
			{
				if (!Charge(value.size() + 3) || stack.back().value.contains(value)) return false;
				stack.back().key = value;
			}
			return true;
		}
		bool end_object() override { return End(); }
		bool end_array() override { return End(); }
		bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }
	private:
		struct Frame { nlohmann::json value; std::string key; };
		std::vector<Frame> stack;
		std::size_t depth = 0, charged = 0;
		std::string root_key;
		std::unordered_set<std::string> authority_keys;
		bool seen_archive = false;
		bool Charge(std::size_t bytes)
		{
			if (bytes > kMaxBytes - charged) return false;
			charged += bytes;
			return true;
		}
		bool Put(nlohmann::json value)
		{
			if (stack.back().value.is_array())
			{
				if (stack.size() == 1 && stack.back().value.size() >= 32) return false;
				stack.back().value.push_back(std::move(value));
			}
			else stack.back().value[stack.back().key] = std::move(value);
			return true;
		}
		bool Scalar(nlohmann::json value)
		{
			if (depth == 1 && (root_key == "acp_queued_prompts" ||
			    (root_key == "imported_read_only" && !value.is_boolean()) ||
			    (root_key == "id" && !value.is_string()))) return false;
			return stack.empty() || (Charge(value.dump().size() + 1) && Put(std::move(value)));
		}
		bool Start(bool array)
		{
			if (depth == 0 && array) return false;
			if (depth == 1 && (root_key == "id" || root_key == "imported_read_only")) return false;
			if (depth == 1 && root_key == "acp_queued_prompts")
			{
				if (seen_archive || !array) return false;
				seen_archive = true;
				stack.push_back({nlohmann::json::array(), {}});
			}
			else if (!stack.empty()) stack.push_back({array ? nlohmann::json::array() : nlohmann::json::object(), {}});
			if (!stack.empty() && (!Charge(3) || stack.size() > 128)) return false;
			++depth;
			return true;
		}
		bool End()
		{
			if (depth == 0) return false;
			--depth;
			if (stack.empty()) return true;
			auto value = std::move(stack.back().value);
			stack.pop_back();
			if (stack.empty()) document.prompts = std::move(value);
			else return Put(std::move(value));
			return true;
		}
	};

	inline std::optional<Document> Parse(std::string_view source)
	try
	{
		ArchiveSax sax;
		if (!nlohmann::json::sax_parse(source.begin(), source.end(), &sax)) return {};
		sax.document.canonical = sax.document.prompts.dump();
		if (sax.document.canonical.size() > kMaxBytes) return {};
		return std::move(sax.document);
	}
	catch (...) { return {}; }
}

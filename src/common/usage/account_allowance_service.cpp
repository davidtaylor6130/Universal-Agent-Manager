#include "common/usage/account_allowance_service.h"
#include "common/provider/provider_ids.h"
#include "common/paths/path_utils.h"
#include "common/utils/hash_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace uam::usage
{
	namespace
	{
		std::int64_t Now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
		std::string Scope(const AccountAllowanceProviderConfig& config) { return hashing::Hex64Padded(hashing::Fnv1a64(nlohmann::json::array({config.provider_id, config.command}).dump())); }
		nlohmann::json EmptyRow(std::string_view provider)
		{
			return {{"providerId", provider}, {"status", "unavailable"}, {"limits", nullptr}, {"windows", nlohmann::json::array()}, {"error", "Account allowance is not available from this provider connection."}, {"primaryLabel", ""}, {"secondaryLabel", ""}};
		}
		std::optional<nlohmann::json> Windows(const nlohmann::json& value)
		{
			if (!value.is_array() || value.size() > 64) return std::nullopt;
			nlohmann::json result = nlohmann::json::array();
			for (const auto& item : value)
			{
				if (!item.is_object()) return std::nullopt;
				for (const char* key : {"id", "label"})
					if (!item.contains(key) || !item[key].is_string() || strings::IsBlank(item[key].get_ref<const std::string&>()) || item[key].get_ref<const std::string&>().size() > 128) return std::nullopt;
				for (const auto& existing : result) if (existing["id"] == item["id"]) return std::nullopt;
				if (!item.contains("usedPercent") || !item["usedPercent"].is_number() || !std::isfinite(item["usedPercent"].get<double>()) || item["usedPercent"] < 0 || item["usedPercent"] > 100) return std::nullopt;
				nlohmann::json clean = {{"id", item["id"]}, {"label", item["label"]}, {"usedPercent", item["usedPercent"]}, {"resetsAt", nullptr}, {"windowDurationMinutes", nullptr}};
				for (const char* key : {"resetsAt", "windowDurationMinutes"})
				{
					if (!item.contains(key) || item[key].is_null()) continue;
					if (!item[key].is_number_integer() || item[key] < 0 || item[key] > 253402300799LL || (std::string_view(key) == "windowDurationMinutes" && item[key] == 0)) return std::nullopt;
					clean[key] = item[key];
				}
				result.push_back(std::move(clean));
			}
			return result;
		}
		// The cache contains only presentation fields, never credentials or arbitrary provider payloads.
		nlohmann::json Limits(const nlohmann::json& value)
		{
			if (!value.is_object() || !value.contains("updatedAt") || !value["updatedAt"].is_number_integer() || value["updatedAt"] <= 0 || value["updatedAt"] > Now()) return nullptr;
			nlohmann::json result = {{"updatedAt", value["updatedAt"]}, {"primary", nullptr}, {"secondary", nullptr}, {"credits", nullptr}, {"individualLimit", nullptr}, {"spendControlReached", nullptr}};
			for (const char* key : {"limitId", "limitName", "planType", "rateLimitReachedType"})
			{
				if (value.contains(key) && !value[key].is_null() && (!value[key].is_string() || value[key].get_ref<const std::string&>().size() > 256)) return nullptr;
				result[key] = value.contains(key) && value[key].is_string() ? value[key] : nlohmann::json("");
			}
			for (const char* key : {"primary", "secondary"})
			{
				if (!value.contains(key) || value[key].is_null()) continue;
				const auto& window = value[key];
				if (!window.is_object() || !window.contains("usedPercent") || !window["usedPercent"].is_number()) return nullptr;
				const double percent = window["usedPercent"].get<double>();
				if (!std::isfinite(percent) || percent < 0 || percent > 100) return nullptr;
				nlohmann::json clean = {{"usedPercent", percent}, {"resetsAt", nullptr}, {"windowDurationMinutes", nullptr}};
				for (const char* field : {"resetsAt", "windowDurationMinutes"})
				{
					if (!window.contains(field) || window[field].is_null()) continue;
					if (!window[field].is_number_integer() || window[field] < 0 || window[field] > 253402300799LL || (std::string_view(field) == "windowDurationMinutes" && window[field] == 0)) return nullptr;
					clean[field] = window[field];
				}
				result[key] = std::move(clean);
			}
			if (value.contains("credits") && value["credits"].is_object())
			{
				const auto& credits = value["credits"];
				if (!credits.contains("hasCredits") || !credits["hasCredits"].is_boolean() || !credits.contains("unlimited") || !credits["unlimited"].is_boolean()) return nullptr;
				const auto balance = credits.value("balance", nlohmann::json(nullptr));
				if (!balance.is_null() && (!balance.is_string() || balance.get_ref<const std::string&>().size() > 128)) return nullptr;
				result["credits"] = {{"hasCredits", credits["hasCredits"]}, {"unlimited", credits["unlimited"]}, {"balance", balance}};
			}
			if (value.contains("spendControlReached") && value["spendControlReached"].is_boolean()) result["spendControlReached"] = value["spendControlReached"];
			if (value.contains("individualLimit") && value["individualLimit"].is_object())
			{
				const auto& individual = value["individualLimit"];
				if (!individual.contains("remainingPercent") || !individual["remainingPercent"].is_number() || !std::isfinite(individual["remainingPercent"].get<double>()) || individual["remainingPercent"] < 0 || individual["remainingPercent"] > 100 ||
				    !individual.contains("limit") || !individual["limit"].is_string() || individual["limit"].get_ref<const std::string&>().size() > 128 || !individual.contains("used") || !individual["used"].is_string() || individual["used"].get_ref<const std::string&>().size() > 128 ||
				    !individual.contains("resetsAt") || !individual["resetsAt"].is_number_integer() || individual["resetsAt"] < 0 || individual["resetsAt"] > 253402300799LL) return nullptr;
				result["individualLimit"] = {{"limit", individual["limit"]}, {"used", individual["used"]}, {"remainingPercent", individual["remainingPercent"]}, {"resetsAt", individual["resetsAt"]}};
			}
			return result;
		}
	}

	struct AccountAllowanceService::Provider
	{
		AccountAllowanceProviderConfig config;
		nlohmann::json row;
		std::uint64_t generation = 0;
		bool requested = false;
		bool in_flight = false;
		bool account_unavailable = false;
		std::optional<std::chrono::steady_clock::time_point> last_attempt;
	};

	AccountAllowanceService::AccountAllowanceService(std::filesystem::path root, std::vector<AccountAllowanceProviderConfig> providers, AccountAllowanceFetcher fetcher, AccountAllowanceClock clock)
	    : m_dataRoot(std::move(root)), m_fetcher(std::move(fetcher)), m_clock(std::move(clock))
	{
		for (const char* id : provider_ids::kAllCliProviderIds)
		{
			auto provider = std::make_unique<Provider>(); provider->config.provider_id = id; provider->row = EmptyRow(id); m_providers.push_back(std::move(provider));
		}
		SetProviders(std::move(providers));
		Restore();
		for (std::size_t index = 0; index < m_providers.size(); ++index) m_workers.emplace_back([this, index](std::stop_token stop) { Run(stop, index); });
	}

	AccountAllowanceService::~AccountAllowanceService()
	{
		for (auto& worker : m_workers) worker.request_stop();
		m_wake.notify_all();
		m_workers.clear();
	}

	void AccountAllowanceService::SetProviders(std::vector<AccountAllowanceProviderConfig> providers)
	{
		std::lock_guard lock(m_mutex);
		for (auto& provider : m_providers)
		{
			const auto found = std::ranges::find_if(providers, [&](const auto& config) { return config.provider_id == provider->config.provider_id; });
			AccountAllowanceProviderConfig next = found == providers.end() ? AccountAllowanceProviderConfig{provider->config.provider_id, {}} : *found;
			if (next == provider->config) continue;
			provider->config = std::move(next); ++provider->generation; provider->row = EmptyRow(provider->config.provider_id); provider->last_attempt = {}; provider->account_unavailable = false;
			provider->requested = !provider->config.command.empty();
			if (provider->requested) { provider->row["status"] = "loading"; provider->row["error"] = ""; }
		}
		m_wake.notify_all();
	}

	nlohmann::json AccountAllowanceService::Query(std::string_view host, bool refresh)
	{
		nlohmann::json rows = nlohmann::json::array(); bool refreshing = false;
		std::int64_t refresh_after_ms = 30000; bool has_targets = false;
		std::lock_guard lock(m_mutex);
		const auto now = m_clock();
		for (auto& provider : m_providers)
		{
			if (host != "local") { auto row = EmptyRow(provider->config.provider_id); row["scope"] = "remote-unavailable"; row["error"] = "Independent account allowances are not available on this remote connection."; rows.push_back(std::move(row)); continue; }
			if (!provider->config.command.empty() && !provider->requested && !provider->in_flight && (refresh || !provider->account_unavailable) &&
			    (!provider->last_attempt || now - *provider->last_attempt >= (refresh ? std::chrono::seconds(30) : std::chrono::seconds(300))))
			{ provider->requested = true; provider->row["status"] = "loading"; provider->row["error"] = ""; }
			refreshing = refreshing || provider->requested || provider->in_flight;
			if (!provider->config.command.empty())
			{
				has_targets = true;
				const auto elapsed = provider->last_attempt ? std::chrono::duration_cast<std::chrono::milliseconds>(now - *provider->last_attempt).count() : 30000;
				refresh_after_ms = std::min(refresh_after_ms, std::clamp<std::int64_t>(30000 - elapsed, 0, 30000));
			}
			auto row = provider->row;
			row["scope"] = Scope(provider->config);
			rows.push_back(std::move(row));
		}
		m_wake.notify_all();
		return {{"executionHostId", host}, {"providers", std::move(rows)}, {"refreshing", refreshing}, {"refreshAfterMs", has_targets ? refresh_after_ms : 0}};
	}

	void AccountAllowanceService::Run(std::stop_token stop, std::size_t index)
	{
		while (!stop.stop_requested())
		{
			AccountAllowanceProviderConfig config; std::uint64_t generation;
			{
				std::unique_lock lock(m_mutex);
				if (!m_wake.wait(lock, stop, [&] { return m_providers[index]->requested; })) return;
				auto& provider = *m_providers[index]; provider.requested = false; provider.in_flight = true; provider.last_attempt = m_clock(); config = provider.config; generation = provider.generation;
			}
			AccountAllowanceFetchResult result;
			try { if (m_fetcher) result = m_fetcher(config, m_dataRoot, stop); }
			catch (...) { result.error = "Could not read account allowance."; }
			if (stop.stop_requested()) return;
			bool save = false;
			{
				std::lock_guard lock(m_mutex); auto& provider = *m_providers[index]; provider.in_flight = false;
				if (provider.generation != generation) continue;
				provider.account_unavailable = result.account_unavailable;
				const auto windows = Windows(result.windows);
				const auto limits = windows ? Limits(result.limits) : nlohmann::json(nullptr);
				provider.row["error"] = strings::SafeLine(result.error, 512);
				if (!limits.is_null())
				{
					provider.row["limits"] = limits; provider.row["windows"] = *windows; provider.row["status"] = "ready"; provider.row["primaryLabel"] = strings::SafeLine(result.primary_label, 128); provider.row["secondaryLabel"] = strings::SafeLine(result.secondary_label, 128); save = true;
				}
				else
				{
					provider.row["status"] = "error";
					if (result.account_unavailable) { provider.row["limits"] = nullptr; provider.row["windows"] = nlohmann::json::array(); provider.row["primaryLabel"] = ""; provider.row["secondaryLabel"] = ""; save = true; }
					if (result.error.empty()) provider.row["error"] = "Account allowance is not available for the current login.";
				}
			}
			if (save) Save();
		}
	}

	void AccountAllowanceService::Restore()
	{
		const auto directory = m_dataRoot / "usage"; const auto path = directory / "account-allowances-v1.json";
		if (paths::IsLinkOrReparsePointNoThrow(directory) || paths::IsLinkOrReparsePointNoThrow(path)) return;
		std::string bytes; if (!io::TryReadTextFile(path, bytes, 256 * 1024)) return;
		const auto cache = nlohmann::json::parse(bytes, nullptr, false);
		if (!cache.is_object() || cache.value("version", nlohmann::json(nullptr)) != 1 || !cache.contains("providers") || !cache["providers"].is_array() || cache["providers"].size() > 6) return;
		for (const auto& saved : cache["providers"])
		{
			if (!saved.is_object() || !saved.contains("providerId") || !saved["providerId"].is_string() || !saved.contains("scope") || !saved["scope"].is_string()) continue;
			for (auto& provider : m_providers)
			{
				if (provider->config.command.empty() || provider->config.provider_id != saved["providerId"].get_ref<const std::string&>() || Scope(provider->config) != saved["scope"].get_ref<const std::string&>()) continue;
				const auto limits = Limits(saved.value("limits", nlohmann::json(nullptr))); if (limits.is_null()) continue;
				const auto windows = Windows(saved.value("windows", nlohmann::json::array())); if (!windows) continue;
				provider->row["limits"] = limits;
				provider->row["windows"] = *windows;
				for (const char* key : {"primaryLabel", "secondaryLabel"}) if (saved.contains(key) && saved[key].is_string()) provider->row[key] = strings::SafeLine(saved[key].get<std::string>(), 128);
			}
		}
	}

	void AccountAllowanceService::Save()
	{
		std::lock_guard save_lock(m_saveMutex); nlohmann::json rows = nlohmann::json::array();
		{
			std::lock_guard lock(m_mutex);
			for (const auto& provider : m_providers)
				if (!provider->row["limits"].is_null()) rows.push_back({{"providerId", provider->config.provider_id}, {"scope", Scope(provider->config)}, {"limits", provider->row["limits"]}, {"windows", provider->row["windows"]}, {"primaryLabel", provider->row["primaryLabel"]}, {"secondaryLabel", provider->row["secondaryLabel"]}});
		}
		const auto directory = m_dataRoot / "usage"; const auto path = directory / "account-allowances-v1.json";
		if (paths::IsLinkOrReparsePointNoThrow(directory) || paths::IsLinkOrReparsePointNoThrow(path) || !paths::CreateDirectoriesNoThrow(directory)) return;
		(void)io::AtomicWriteFile(path, nlohmann::json({{"version", 1}, {"providers", std::move(rows)}}).dump());
	}
}

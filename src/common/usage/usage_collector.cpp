#include "common/usage/usage_collector.h"
#include "common/utils/io_utils.h"

namespace uam::usage
{
	namespace
	{
		std::int64_t Now()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		}
		nlohmann::json EmptyMetadata()
		{
			nlohmann::json result{{"version", 3}, {"source", "native-transcripts"}, {"executionHostId", "local"}, {"complete", false},
			    {"filesChecked", 0}, {"filesTotal", 0}, {"bytesRead", 0}, {"listingLimited", false}};
			result["providers"] = nlohmann::json::array();
			for (const char* provider : provider_ids::kAllCliProviderIds)
				result["providers"].push_back({{"providerId", provider}, {"available", false}, {"files", 0}, {"errors", 0},
				    {"invalidLines", 0}, {"oversizedLines", 0}, {"suppressedForkRecords", 0}, {"pendingLines", 0}});
			return result;
		}
	}

	Collector::Collector(std::filesystem::path data_root)
	    : m_dataRoot(std::move(data_root)), m_store(m_dataRoot), m_pricing(m_dataRoot, PricingCache::PublicCatalogFetcher()), m_catalog(m_pricing.Catalog()), m_pricingMetadata(m_pricing.Metadata(Now())), m_metadata(EmptyMetadata()), m_worker([this](std::stop_token stop) { Run(stop); }) {}

	Collector::Collector(std::filesystem::path data_root, std::vector<HistorySource> sources, RateFetcher fetch_rates)
	    : m_dataRoot(std::move(data_root)), m_store(m_dataRoot), m_history(std::move(sources)), m_pricing(m_dataRoot, std::move(fetch_rates)), m_catalog(m_pricing.Catalog()), m_pricingMetadata(m_pricing.Metadata(Now())), m_metadata(EmptyMetadata()), m_worker([this](std::stop_token stop) { Run(stop); }) {}

	Collector::~Collector()
	{
		m_worker.request_stop();
		m_wake.notify_all();
		if (m_worker.joinable()) m_worker.join();
	}

	bool Collector::Publish(std::int64_t generated_ms, const nlohmann::json& metadata, std::string& error, const std::function<bool()>& cancelled)
	{
		std::optional<Ledger> ledger = m_store.Read(generated_ms, error, cancelled);
		if (!ledger) return false;
		const std::shared_ptr<const Ledger> published = std::make_shared<const Ledger>(std::move(*ledger));
		const std::string encoded = published->Encode().dump();
		if (encoded.size() > 64 * 1024 * 1024) { error = "Usage snapshot exceeds its storage limit."; return false; }
		if (cancelled()) return false;
		if (!io::AtomicWriteFile(m_dataRoot / "usage" / "snapshot-v1.json", encoded)) { error = "Could not save the usage snapshot."; return false; }
		std::lock_guard<std::mutex> lock(m_mutex);
		m_ledger = published; m_metadata = metadata; m_metadata["version"] = 3; m_error.clear();
		return true;
	}

	void Collector::Run(std::stop_token stop)
	{
		const std::function<bool()> cancelled = [stop]() { return stop.stop_requested(); };
		try
		{
			std::string saved;
			if (!m_dataRoot.empty() && io::TryReadTextFile(m_dataRoot / "usage" / "snapshot-v1.json", saved, 64 * 1024 * 1024))
			{
				std::optional<Ledger> ledger = Ledger::Decode(nlohmann::json::parse(saved, nullptr, false));
				if (ledger && ledger->GeneratedMs() <= Now())
				{
					std::lock_guard<std::mutex> lock(m_mutex);
					m_ledger = std::make_shared<const Ledger>(std::move(*ledger));
				}
			}
			m_pricing.Refresh(Now(), stop, false);
			{
				std::lock_guard<std::mutex> lock(m_mutex); m_catalog = m_pricing.Catalog(); m_aliases = m_pricing.Aliases(); m_pricingMetadata = m_pricing.Metadata(Now());
			}
			std::size_t generation = 0;
			while (!cancelled())
			{
				const std::int64_t as_of = Now();
				const std::int64_t start = std::max<std::int64_t>(0, as_of / 86400000 * 86400000 - 89LL * 86400000);
				const std::string refresh_id = "collector-" + std::to_string(++generation);
				std::string error;
				{
					std::lock_guard<std::mutex> lock(m_mutex); m_collecting = true;
				}
				const AccountingSink sink = [&](const std::string& source, const std::vector<Record>& records, std::size_t position, std::string& import_error)
				{
					std::vector<SourceRecord> batch;
					for (std::size_t index = position; index < records.size(); ++index)
					{
						if (cancelled()) return false;
						if (records[index].timestamp_ms < start || records[index].timestamp_ms >= as_of) continue;
						batch.push_back({&records[index], source, index});
						if (batch.size() == 2048)
						{
							if (!m_store.Import(batch, import_error, cancelled)) return false;
							batch.clear();
						}
					}
					return batch.empty() || m_store.Import(batch, import_error, cancelled);
				};
				nlohmann::json progress;
				std::chrono::steady_clock::time_point last_publish = std::chrono::steady_clock::now();
				do
				{
					progress = m_history.Collect(refresh_id, start, as_of, sink, cancelled);
					if (progress.contains("error")) { error = progress["error"].get<std::string>(); break; }
					if (progress.value("cancelled", false) || cancelled()) return;
					if (progress.value("complete", false) || std::chrono::steady_clock::now() - last_publish >= std::chrono::seconds(5))
					{
						if (!Publish(as_of, progress, error, cancelled)) break;
						last_publish = std::chrono::steady_clock::now();
					}
				} while (!progress.value("complete", false));
				{
					std::lock_guard<std::mutex> lock(m_mutex); m_error = error; m_collecting = false;
				}
				{
					std::lock_guard<std::mutex> lock(m_mutex); m_pricingMetadata["refreshing"] = true;
				}
				m_pricing.Refresh(Now(), stop);
				{
					std::lock_guard<std::mutex> lock(m_mutex); m_catalog = m_pricing.Catalog(); m_aliases = m_pricing.Aliases(); m_pricingMetadata = m_pricing.Metadata(Now());
				}
				std::unique_lock<std::mutex> wait_lock(m_waitMutex);
				m_wake.wait_for(wait_lock, stop, std::chrono::seconds(30), [] { return false; });
			}
		}
		catch (const std::exception&) { std::lock_guard<std::mutex> lock(m_mutex); m_error = "Usage collection failed. Background collection will restart with UAM."; m_collecting = false; }
	}

	nlohmann::json Collector::Query(int days, const std::map<ModelKey, ModelPrices>& prices) const
	{
		if (days != 1 && days != 7 && days != 30 && days != 90) return {{"error", "Choose a valid usage period."}};
		std::shared_ptr<const Ledger> ledger;
		std::shared_ptr<const RateCatalog> catalog;
		ModelAliases aliases;
		nlohmann::json result;
		{
			std::lock_guard<std::mutex> lock(m_mutex); ledger = m_ledger; catalog = m_catalog; aliases = m_aliases; result = m_metadata; result["pricing"] = m_pricingMetadata;
			result["collecting"] = m_collecting; result["collectionError"] = m_error;
		}
		if (!ledger) return {{"pending", true}, {"collectionError", result["collectionError"]}};
		const std::int64_t start = ledger->WindowStart(days), end = ledger->GeneratedMs();
		std::optional<Summary> summary = ledger->Query(start, end);
		if (!summary) return {{"error", "Usage totals exceed supported limits."}};
		std::map<ModelKey, ModelPrices> resolved_prices;
		std::map<ModelKey, ResolvedPrice> references;
		if (catalog)
			for (const std::pair<const ModelKey, Totals>& model : summary->models)
			{
				const std::optional<ResolvedPrice> reference = catalog->Resolve(model.first, aliases);
				if (!reference) continue;
				resolved_prices.emplace(model.first, reference->prices); references.emplace(model.first, *reference);
			}
		for (const std::pair<const ModelKey, ModelPrices>& price : prices) resolved_prices[price.first] = price.second;
		if (!resolved_prices.empty()) summary = ledger->Query(start, end, resolved_prices);
		if (!summary) return {{"error", "Usage totals or prices exceed supported limits."}};
		ApplyUsageSummary(result, *summary, start, end);
		for (nlohmann::json& row : result["models"])
		{
			const ModelKey key{row["providerId"].get<std::string>(), row["modelId"].get<std::string>(), row["speed"].get<std::string>()};
			const bool manual = prices.contains(key);
			if (!manual && !references.contains(key)) continue;
			const ModelPrices& selected = resolved_prices.at(key);
			row["priceReference"] = {{"kind", manual ? "manual" : "reference"}, {"referenceModel", manual ? std::get<1>(key) : references.at(key).reference_model},
			    {"source", manual ? "" : references.at(key).source}, {"verifiedDate", manual ? "" : references.at(key).verified_date},
			    {"cacheFallback", !manual && references.at(key).cache_fallback}, {"speedFallback", !manual && references.at(key).speed_fallback},
			    {"inputPerMillion", selected.input_per_million ? nlohmann::json(*selected.input_per_million) : nlohmann::json(nullptr)},
			    {"cachedInputPerMillion", selected.cached_input_per_million ? nlohmann::json(*selected.cached_input_per_million) : nlohmann::json(nullptr)},
			    {"cacheWritePerMillion", selected.cache_write_per_million ? nlohmann::json(*selected.cache_write_per_million) : nlohmann::json(nullptr)},
			    {"outputPerMillion", selected.output_per_million ? nlohmann::json(*selected.output_per_million) : nlohmann::json(nullptr)}};
		}
		result["asOfMs"] = end; result["windowDays"] = days;
		return result;
	}
}

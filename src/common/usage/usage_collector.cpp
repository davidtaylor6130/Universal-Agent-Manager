#include "common/usage/usage_collector.h"
#include "common/utils/io_utils.h"
#include "common/utils/base64.h"
#include "common/utils/hash_utils.h"
#include <fstream>

namespace uam::usage
{
	namespace
	{
		std::int64_t Now()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		}

	}

	Collector::Collector(std::filesystem::path data_root, bool export_accounting)
	    : m_dataRoot(std::move(data_root)), m_exportAccounting(export_accounting), m_store(m_dataRoot), m_pricing(m_dataRoot, PricingCache::PublicCatalogFetcher()), m_catalog(m_pricing.Catalog()), m_pricingMetadata(m_pricing.Metadata(Now())), m_metadata(EmptyHistoryMetadata()), m_worker([this](std::stop_token stop) { Run(stop); }) {}

	Collector::Collector(std::filesystem::path data_root, std::vector<HistorySource> sources, RateFetcher fetch_rates, bool export_accounting)
	    : m_dataRoot(std::move(data_root)), m_exportAccounting(export_accounting), m_store(m_dataRoot), m_history(std::move(sources)), m_pricing(m_dataRoot, std::move(fetch_rates)), m_catalog(m_pricing.Catalog()), m_pricingMetadata(m_pricing.Metadata(Now())), m_metadata(EmptyHistoryMetadata()), m_worker([this](std::stop_token stop) { Run(stop); }) {}

	Collector::~Collector()
	{
		m_worker.request_stop();
		m_wake.notify_all();
		if (m_worker.joinable()) m_worker.join();
	}

	bool Collector::Publish(std::int64_t generated_ms, const nlohmann::json& metadata, std::string& error, const std::function<bool()>& cancelled)
	{
		nlohmann::json published_metadata = metadata;
		published_metadata["complete"] = metadata.value("complete", false) && !metadata.value("listingLimited", false);
		for (const nlohmann::json& provider : metadata["providers"])
			for (const char* key : {"errors", "invalidLines", "oversizedLines", "pendingLines"}) if (provider.value(key, std::int64_t{0}) > 0) published_metadata["complete"] = false;
		std::optional<Ledger> ledger = m_store.ReadLifetimeLedger(generated_ms, error, cancelled);
		if (!ledger) return false;
		std::shared_ptr<const ReceiptSnapshot> receipts;
		std::shared_ptr<const std::string> accounting;
		std::string fingerprint;
		if (m_exportAccounting)
		{
			receipts = m_store.WriteReceiptSnapshot(generated_ms, published_metadata, error, cancelled);
			if (!receipts) return false;
			// Legacy clients retain their bounded document format; large histories use streamed receipts.
			if (receipts->bytes <= 64 * 1024 * 1024)
			{
				nlohmann::json rows = nlohmann::json::array();
				if (!receipts->Visit([&](const AccountingRecord& entry) { rows.push_back(EncodeAccountingRecord(entry)); return true; }, error, cancelled)) return false;
				std::string bytes = nlohmann::json({{"version", 2}, {"range", "all-recorded"}, {"asOfMs", generated_ms}, {"metadata", published_metadata}, {"facts", std::move(rows)}}).dump();
				if (bytes.size() <= 64 * 1024 * 1024)
				{ fingerprint = hashing::Hex64Padded(hashing::Fnv1a64(bytes)); accounting = std::make_shared<const std::string>(std::move(bytes)); }
			}
		}

		if (!ledger) { if (error.empty()) error = "Usage accounting totals exceed supported limits."; return false; }
		const std::shared_ptr<const Ledger> published = std::make_shared<const Ledger>(std::move(*ledger));
		const std::string encoded = published->Encode().dump();
		// The durable receipt index can rebuild unusually large cohort snapshots after restart.
		if (cancelled()) return false;
		if (encoded.size() <= 64 * 1024 * 1024 && !io::AtomicWriteFile(m_dataRoot / "usage" / "snapshot-v1.json", encoded)) { error = "Could not save the usage snapshot."; return false; }
		std::lock_guard<std::mutex> lock(m_mutex);
		m_ledger = published; m_receipts = std::move(receipts); m_accountingBytes = std::move(accounting); m_accountingFingerprint = std::move(fingerprint); m_metadata = published_metadata; m_metadata["version"] = 3; m_error.clear();
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
				const std::int64_t start = 0;
				const std::string refresh_id = "collector-" + std::to_string(++generation);
				std::string error;
				{
					std::lock_guard<std::mutex> lock(m_mutex); m_collecting = true;
				}
				const StreamingAccountingSink sink = [&](const std::string& source, const std::vector<Record>& records, std::size_t position, std::size_t base, std::string& import_error)
				{
					std::vector<SourceRecord> batch;
					for (std::size_t index = position; index < records.size(); ++index)
					{
						if (cancelled()) return false;
						if (records[index].timestamp_ms < start) continue;
						batch.push_back({&records[index], source, base + index});
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
					progress = m_history.CollectStream(refresh_id, start, as_of, sink, cancelled);
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

	nlohmann::json Collector::ExportAccounting(std::size_t offset, std::string_view fingerprint) const
	{
		std::shared_ptr<const std::string> bytes;
		std::string current_fingerprint;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_exportAccounting) return {{"error", "Accounting export is unavailable for this collector."}};
			if (!m_accountingBytes) return m_receipts ? nlohmann::json{{"error", "Update the usage client to load streamed lifetime receipts."}} : nlohmann::json{{"pending", true}, {"collectionError", m_error}};
			bytes = m_accountingBytes; current_fingerprint = m_accountingFingerprint;
		}
		if ((!fingerprint.empty() && fingerprint != current_fingerprint) || offset > bytes->size()) return {{"error", "Usage accounting changed while loading. Try again."}};
		const std::string_view chunk(bytes->data() + offset, std::min<std::size_t>(512 * 1024, bytes->size() - offset));
		return {{"dataBase64", base64::Encode(chunk)}, {"offset", offset}, {"totalBytes", bytes->size()}, {"fingerprint", current_fingerprint}};
	}

	std::shared_ptr<const std::string> Collector::AccountingSnapshot() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_accountingBytes;
	}

	std::shared_ptr<const ReceiptSnapshot> Collector::Receipts() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_receipts;
	}

	nlohmann::json Collector::ExportReceipts(std::uint64_t offset, std::string_view fingerprint) const
	{
		std::shared_ptr<const ReceiptSnapshot> snapshot;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_exportAccounting) return {{"error", "Accounting export is unavailable for this collector."}};
			const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
			std::erase_if(m_receiptExports, [now](const std::pair<const std::string, std::pair<std::shared_ptr<const ReceiptSnapshot>, std::chrono::steady_clock::time_point>>& entry) { return entry.second.second <= now; });
			if (fingerprint.empty())
			{
				if (offset != 0) return {{"error", "Choose the accounting snapshot before loading receipts."}};
				snapshot = m_receipts;
				if (!snapshot) return {{"pending", true}, {"collectionError", m_error}};
				if (!m_receiptExports.contains(snapshot->fingerprint) && m_receiptExports.size() >= 32) return {{"error", "Accounting export is busy. Try again shortly."}};
				m_receiptExports[snapshot->fingerprint] = {snapshot, now + std::chrono::minutes(2)};
			}
			else
			{
				const std::map<std::string, std::pair<std::shared_ptr<const ReceiptSnapshot>, std::chrono::steady_clock::time_point>>::iterator found = m_receiptExports.find(std::string(fingerprint));
				if (found == m_receiptExports.end()) return {{"error", "Usage accounting snapshot expired. Try again."}};
				snapshot = found->second.first; found->second.second = now + std::chrono::minutes(2);
			}
		}
		if (offset > snapshot->bytes) return {{"error", "Choose a valid accounting receipt offset."}};

		std::ifstream input(snapshot->path, std::ios::binary); input.seekg(static_cast<std::streamoff>(offset));
		std::string chunk(static_cast<std::size_t>(std::min<std::uint64_t>(512 * 1024, snapshot->bytes - offset)), '\0');
		input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
		if (!input || static_cast<std::size_t>(input.gcount()) != chunk.size()) return {{"error", "Could not read accounting receipts."}};
		return {{"format", "jsonl-v3"}, {"dataBase64", base64::Encode(chunk)}, {"offset", offset}, {"totalBytes", snapshot->bytes}, {"fingerprint", snapshot->fingerprint}};
	}

	std::pair<bool, std::string> Collector::CollectionStatus() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return {m_collecting, m_error};
	}

	nlohmann::json Collector::Query(int days, const std::map<ModelKey, ModelPrices>& prices) const
	{
		if (days != 0 && days != 1 && days != 7 && days != 30 && days != 90) return {{"error", "Choose a valid usage period."}};
		std::shared_ptr<const Ledger> ledger;
		nlohmann::json result;
		{
			std::lock_guard<std::mutex> lock(m_mutex); ledger = m_ledger; result = m_metadata;
			result["collecting"] = m_collecting; result["collectionError"] = m_error;
			if (!m_error.empty()) result["complete"] = false;
		}
		if (!ledger) return {{"pending", true}, {"collectionError", result["collectionError"]}};
		return Render(*ledger, std::move(result), days, prices);
	}

	nlohmann::json Collector::Render(const Ledger& selected, nlohmann::json result, int days, const std::map<ModelKey, ModelPrices>& prices) const
	{
		if (days != 0 && days != 1 && days != 7 && days != 30 && days != 90) return {{"error", "Choose a valid usage period."}};
		std::shared_ptr<const RateCatalog> catalog;
		ModelAliases aliases;
		{
			std::lock_guard<std::mutex> lock(m_mutex); catalog = m_catalog; aliases = m_aliases; result["pricing"] = m_pricingMetadata;
		}
		if (days == 0 && !selected.IncludesLifetime()) return {{"error", "All Time accounting is unavailable in this rolling snapshot."}};
		if (days == 0) result["historyCoverage"] = {{"kind", "recorded-and-available-history"}, {"previouslyPrunedReceiptsMayBeMissing", true}};
		const Ledger* ledger = &selected;
		const std::int64_t start = ledger->WindowStart(days), end = ledger->GeneratedMs();
		std::optional<Summary> summary = ledger->Query(start, end, {}, days == 0);
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
		if (!resolved_prices.empty()) summary = ledger->Query(start, end, resolved_prices, days == 0);
		if (!summary) return {{"error", "Usage totals or prices exceed supported limits."}};
		ApplyUsageSummary(result, *summary, start, end);
		if (days == 0) { result["chartStartMs"] = summary->chart_start_ms; result["chartBucketMs"] = summary->chart_bucket_ms; }
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

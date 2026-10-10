#include "common/usage/usage_pricing_cache.h"
#include "common/utils/io_utils.h"
#include "common/platform/platform_services.h"

namespace uam::usage
{
	namespace
	{
		constexpr std::size_t kDocumentLimit = 4 * 1024 * 1024;
		constexpr std::int64_t kRatesTtlMs = 86400000;
		constexpr std::int64_t kRetryMs = 600000;
	}

	PricingCache::PricingCache(std::filesystem::path data_root, RateFetcher fetcher) : m_dataRoot(std::move(data_root)), m_fetcher(std::move(fetcher))
	{
		RateCatalog references; references.AddVerifiedReferences(); m_catalog = std::make_shared<const RateCatalog>(std::move(references));
	}

	RateFetcher PricingCache::PublicCatalogFetcher()
	{
		return [](std::stop_token stop) -> std::optional<std::string>
		{
			if (stop.stop_requested()) return std::nullopt;
			// Static public URL only. Captured output, HTTPS and execution time are bounded by the platform service and curl.
			const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
			    std::string("curl --fail --silent --proto =https --connect-timeout 3 --max-time 8 --max-filesize 4194304 ") + kUsageRatesUrl, 10000, stop);
			if (!result.ok || result.timed_out || result.canceled || result.output_truncated || result.exit_code != 0 || result.output.empty() || result.output.size() > kDocumentLimit) return std::nullopt;
			return result.output;
		};
	}

	void PricingCache::Refresh(std::int64_t now_ms, std::stop_token stop, bool allow_fetch)
	{
		if (stop.stop_requested() || now_ms <= 0 || now_ms > kMaxExactTokenCount || m_dataRoot.empty()) return;
		std::string encoded;
		const std::filesystem::path directory = m_dataRoot / "usage";
		std::error_code path_error;
		if (std::filesystem::is_symlink(std::filesystem::symlink_status(directory, path_error))) { m_error = "Usage pricing storage cannot be a symbolic link."; return; }
		if (path_error && path_error != std::errc::no_such_file_or_directory) { m_error = "Could not inspect usage pricing storage."; return; }
		m_aliasError.clear();
		const std::filesystem::path alias_path = directory / "reference-models-v1.json";
		path_error.clear();
		const bool alias_exists = std::filesystem::exists(alias_path, path_error);
		if (path_error) m_aliasError = "Could not inspect reference-model mappings. Previous mappings were kept.";
		else if (!alias_exists) m_aliases.clear();
		else if (io::TryReadTextFile(alias_path, encoded, 256 * 1024))
		{
			const std::optional<ModelAliases> aliases = RateCatalog::ParseAliases(nlohmann::json::parse(encoded, nullptr, false));
			if (aliases) m_aliases = *aliases;
			else m_aliasError = "Reference-model mappings are invalid. Previous mappings were kept.";
		}
		else m_aliasError = "Could not read reference-model mappings. Previous mappings were kept.";
		const std::filesystem::path cache_path = directory / "rates-v1.json";
		if (!m_restored)
		{
			m_restored = true;
			if (io::TryReadTextFile(cache_path, encoded, kDocumentLimit + 1024))
			{
				const nlohmann::json saved = nlohmann::json::parse(encoded, nullptr, false);
				if (saved.is_object() && saved.contains("version") && saved["version"].is_number_integer() && saved["version"] == 1 && saved.contains("fetchedAtMs") && saved["fetchedAtMs"].is_number_integer() &&
				    saved["fetchedAtMs"] > 0 && saved["fetchedAtMs"] <= now_ms && saved.contains("document"))
				{
					RateCatalog catalog = RateCatalog::Parse(saved["document"]);
					if (catalog.Size() > 0)
					{
						catalog.AddVerifiedReferences(); m_catalog = std::make_shared<const RateCatalog>(std::move(catalog));
						m_fetchedAtMs = saved["fetchedAtMs"].get<std::int64_t>(); m_fromDisk = true;
					}
				}
			}
		}
		if (!allow_fetch || !m_fetcher || stop.stop_requested() || (m_fetchedAtMs > 0 && now_ms >= m_fetchedAtMs && now_ms - m_fetchedAtMs < kRatesTtlMs) ||
		    (m_lastAttemptMs > 0 && now_ms >= m_lastAttemptMs && now_ms - m_lastAttemptMs < kRetryMs)) return;
		m_lastAttemptMs = now_ms;
		std::optional<std::string> fetched;
		try { fetched = m_fetcher(stop); }
		catch (const std::exception&) { m_error = "Could not refresh the public rate catalog. Saved reference rates were kept."; return; }
		if (stop.stop_requested()) return;
		if (!fetched || fetched->size() > kDocumentLimit) { m_error = "Could not refresh the public rate catalog. Saved reference rates were kept."; return; }
		const nlohmann::json document = nlohmann::json::parse(*fetched, nullptr, false);
		RateCatalog catalog = RateCatalog::Parse(document);
		if (catalog.Size() == 0) { m_error = "The public rate catalog is invalid. Saved reference rates were kept."; return; }
		catalog.AddVerifiedReferences(); m_catalog = std::make_shared<const RateCatalog>(std::move(catalog)); m_fetchedAtMs = now_ms; m_fromDisk = false; m_error.clear();
		if (!paths::CreateDirectoriesNoThrow(directory) || !io::AtomicWriteFile(cache_path, nlohmann::json({{"version", 1}, {"fetchedAtMs", now_ms}, {"document", document}}).dump()))
			m_error = "Reference rates are available, but could not be saved for offline use.";
	}

	nlohmann::json PricingCache::Metadata(std::int64_t now_ms) const
	{
		return {{"refreshing", false}, {"source", kUsageRatesUrl}, {"status", m_fetchedAtMs == 0 ? "unavailable" : m_fromDisk || now_ms - m_fetchedAtMs >= kRatesTtlMs ? "cached" : "fresh"},
		    {"fetchedAtMs", m_fetchedAtMs == 0 ? nlohmann::json(nullptr) : nlohmann::json(m_fetchedAtMs)}, {"error", m_error}, {"aliasError", m_aliasError}};
	}
}

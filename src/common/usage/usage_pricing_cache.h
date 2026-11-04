#pragma once

#include "common/usage/usage_pricing.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>

namespace uam::usage
{
	using RateFetcher = std::function<std::optional<std::string>(std::stop_token)>;
	/// <summary>Worker-owned reference catalog and explicit local model mappings; no provider credentials are sent.</summary>
	class PricingCache
	{
	public:
		PricingCache(std::filesystem::path data_root, RateFetcher fetcher);
		void Refresh(std::int64_t now_ms, std::stop_token stop = {}, bool allow_fetch = true);
		std::shared_ptr<const RateCatalog> Catalog() const { return m_catalog; }
		const ModelAliases& Aliases() const { return m_aliases; }
		nlohmann::json Metadata(std::int64_t now_ms) const;
		static RateFetcher PublicCatalogFetcher();

	private:
		std::filesystem::path m_dataRoot;
		RateFetcher m_fetcher;
		std::shared_ptr<const RateCatalog> m_catalog;
		ModelAliases m_aliases;
		std::int64_t m_fetchedAtMs = 0;
		std::int64_t m_lastAttemptMs = 0;
		bool m_restored = false;
		bool m_fromDisk = false;
		std::string m_error;
		std::string m_aliasError;
	};
}

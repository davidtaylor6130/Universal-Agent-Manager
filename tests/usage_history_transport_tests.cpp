#include "test_harness.h"
#include "common/usage/history_transport.h"

using namespace uam_test;

namespace
{
	bool Refuses(uam::usage::HistoryTransport& transport, int browser, const std::string& owner, const nlohmann::json& page, std::size_t offset,
		uam::usage::HistoryTransport::Clock::time_point now = uam::usage::HistoryTransport::Clock::now())
	{
		try { transport.Page(browser, owner, page["snapshotId"].get<std::string>(), offset, now); return false; }
		catch (const std::runtime_error&) { return true; }
	}
}

UAM_TEST(UsageHistoryTransportPreservesOversizedModelsAndUtf8WithinResponseBudget)
{
	uam::usage::HistoryTransport transport;
	nlohmann::json original{{"asOfMs", 123456}, {"totals", {{"totalTokens", 987654}}}, {"models", nlohmann::json::array()}};
	for (int index = 0; index < 24001; ++index)
		original["models"].push_back({{"modelId", std::to_string(index) + std::string(480, 'x') + "模型🦄"}, {"totalTokens", index}});
	const std::string body = original.dump();
	UAM_ASSERT(body.size() > uam::usage::HistoryTransport::kResponseBytes);
	nlohmann::json page = transport.Start(1, "scope", "refresh", body);
	std::string assembled;
	for (;;)
	{
		UAM_ASSERT(page.dump().size() <= uam::usage::HistoryTransport::kResponseBytes);
		UAM_ASSERT_EQ(page["offset"].get<std::size_t>(), assembled.size());
		UAM_ASSERT_EQ(page["totalBytes"].get<std::size_t>(), body.size());
		assembled += page["chunk"].get<std::string>();
		if (page["nextOffset"].is_null()) break;
		page = transport.Page(1, "scope", page["snapshotId"].get<std::string>(), page["nextOffset"].get<std::size_t>());
	}
	UAM_ASSERT_EQ(assembled, body);
	UAM_ASSERT(nlohmann::json::parse(assembled) == original);
	UAM_ASSERT(Refuses(transport, 1, "scope", page, assembled.size()));
}

UAM_TEST(UsageHistoryTransportRejectsForeignStaleExpiredAndReorderedPages)
{
	uam::usage::HistoryTransport transport;
	const std::string body = nlohmann::json({{"value", std::string(3 * 1024 * 1024, 'a')}}).dump();
	const uam::usage::HistoryTransport::Clock::time_point now = uam::usage::HistoryTransport::Clock::now();
	const nlohmann::json first = transport.Start(1, "root-host-window-price-request", "refresh", body, now);
	const std::size_t next = first["nextOffset"].get<std::size_t>();
	UAM_ASSERT(Refuses(transport, 2, "root-host-window-price-request", first, next, now));
	UAM_ASSERT(Refuses(transport, 1, "different-root-or-host-or-price", first, next, now));
	UAM_ASSERT(Refuses(transport, 1, "root-host-window-price-request", first, next + 1, now));
	UAM_ASSERT(Refuses(transport, 1, "root-host-window-price-request", first, 0, now));
	const nlohmann::json newer = transport.Start(1, "new-request", "new", body, now);
	UAM_ASSERT(Refuses(transport, 1, "root-host-window-price-request", first, next, now));
	UAM_ASSERT(Refuses(transport, 1, "new-request", newer, newer["nextOffset"].get<std::size_t>(), now + std::chrono::seconds(120)));
	UAM_ASSERT(transport.Start(1, "scope", "small", "{\"models\":[]}", now) == nlohmann::json({{"models", nlohmann::json::array()}}));
}

UAM_TEST(UsageHistoryTransportEvictsAbandonedBrowserSnapshotsWithoutTruncation)
{
	uam::usage::HistoryTransport transport;
	const std::string body = nlohmann::json({{"value", std::string(3 * 1024 * 1024, '\n')}}).dump();
	const nlohmann::json abandoned = transport.Start(1, "scope", "refresh", body);
	for (int browser = 2; browser <= 9; ++browser) transport.Start(browser, "scope", "refresh", body);
	UAM_ASSERT(Refuses(transport, 1, "scope", abandoned, abandoned["nextOffset"].get<std::size_t>()));
}

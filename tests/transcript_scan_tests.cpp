#include "test_harness.h"
#include "common/usage/transcript_scan.h"

using namespace uam_test;

namespace
{
	std::string UsageLine(int index, int input = 12)
	{
		return nlohmann::json({{"type", "assistant"}, {"timestamp", "2026-10-10T12:00:00Z"}, {"sessionId", "session"}, {"requestId", "request-" + std::to_string(index)},
		    {"message", {{"id", "message-" + std::to_string(index)}, {"model", "model-\xe2\x98\x83"}, {"usage", {{"input_tokens", input}, {"output_tokens", 3}}}}}}).dump();
	}
}

UAM_TEST(UsageScanResumesSmallChunksAndWaitsForCompleteUtf8LinesWithoutDuplicates)
{
	TempDir temp("uam-usage-scan-append");
	const fs::path path = temp.root / "session.jsonl";
	const std::string first = UsageLine(1);
	const std::size_t split = first.find("\xe2\x98\x83") + 1;
	UAM_ASSERT(split > 1);
	UAM_ASSERT(uam::io::WriteTextFile(path, first.substr(0, split)));
	uam::usage::TranscriptScan scan(uam::provider_ids::kClaudeCli);
	uam::usage::ScanResult result = scan.Refresh(path);
	UAM_ASSERT(result.reset && result.error.empty() && !result.complete);
	UAM_ASSERT(scan.Records().empty());
	UAM_ASSERT_EQ(scan.InvalidLines(), std::size_t(0));
	const std::string appended = first + "\r\n" + UsageLine(2) + "\n";
	UAM_ASSERT(uam::io::WriteTextFile(path, appended));
	uam::usage::ScanOptions options;
	options.byte_budget = 17;
	std::size_t total_read = split;
	for (int attempt = 0; attempt < 100; ++attempt)
	{
		result = scan.Refresh(path, options);
		UAM_ASSERT(result.error.empty());
		UAM_ASSERT(!result.reset);
		UAM_ASSERT(result.bytes_read <= options.byte_budget);
		total_read += result.bytes_read;
		if (result.complete) break;
	}
	UAM_ASSERT(result.complete);
	UAM_ASSERT_EQ(total_read, appended.size());
	UAM_ASSERT_EQ(scan.Records().size(), std::size_t(2));
	UAM_ASSERT_EQ(scan.Records().front().model_id, std::string("model-\xe2\x98\x83"));
	result = scan.Refresh(path);
	UAM_ASSERT(result.complete && result.bytes_read == 0);
	UAM_ASSERT_EQ(scan.Records().size(), std::size_t(2));
}

UAM_TEST(UsageScanResetsOnTruncateSameSizeRewriteAndPathChangeAndPreservesCacheOnIoFailure)
{
	TempDir temp("uam-usage-scan-rewrite");
	const fs::path path = temp.root / "session.jsonl";
	UAM_ASSERT(uam::io::WriteTextFile(path, UsageLine(1) + "\n" + UsageLine(2) + "\n"));
	uam::usage::TranscriptScan scan(uam::provider_ids::kClaudeCli);
	UAM_ASSERT(scan.Refresh(path).complete);
	UAM_ASSERT_EQ(scan.Records().size(), std::size_t(2));
	UAM_ASSERT(uam::io::WriteTextFile(path, UsageLine(3, 13) + "\n"));
	uam::usage::ScanResult result = scan.Refresh(path);
	UAM_ASSERT(result.reset && result.complete);
	UAM_ASSERT_EQ(scan.Records().size(), std::size_t(1));
	UAM_ASSERT_EQ(scan.Records().front().tokens.input, 13LL);
	const fs::file_time_type prior_time = fs::last_write_time(path);
	UAM_ASSERT(uam::io::WriteTextFile(path, UsageLine(3, 14) + "\n"));
	fs::last_write_time(path, prior_time + std::chrono::seconds(1));
	result = scan.Refresh(path);
	UAM_ASSERT(result.reset && result.complete);
	UAM_ASSERT_EQ(scan.Records().front().tokens.input, 14LL);
	const fs::path other = temp.root / "other.jsonl";
	UAM_ASSERT(uam::io::WriteTextFile(other, UsageLine(4, 15) + "\n"));
	UAM_ASSERT(scan.Refresh(other).reset);
	UAM_ASSERT_EQ(scan.Records().front().tokens.input, 15LL);
	fs::remove(other);
	result = scan.Refresh(other);
	UAM_ASSERT(!result.error.empty() && !result.complete);
	UAM_ASSERT_EQ(scan.Records().size(), std::size_t(1));
}

UAM_TEST(UsageScanReportsOversizedMalformedAndLimitedHistoryAndSupportsCancellation)
{
	TempDir temp("uam-usage-scan-bounded");
	const fs::path path = temp.root / "session.jsonl";
	const std::string transcript = "not-json\n" + std::string(2048, 'x') + "\n" + UsageLine(1) + "\n" + UsageLine(2) + "\n";
	UAM_ASSERT(uam::io::WriteTextFile(path, transcript));
	uam::usage::TranscriptScan scan(uam::provider_ids::kClaudeCli);
	uam::usage::ScanOptions options;
	options.byte_budget = 127;
	options.max_line_bytes = 512;
	int cancellation_checks = 0;
	const uam::usage::ScanResult cancelled = scan.Refresh(path, options, [&cancellation_checks]() { return ++cancellation_checks > 1; });
	UAM_ASSERT(cancelled.cancelled && !cancelled.complete);
	UAM_ASSERT_EQ(cancelled.bytes_read, std::size_t(0));
	UAM_ASSERT(scan.Records().empty());
	uam::usage::ScanResult result;
	for (int attempt = 0; attempt < 100; ++attempt)
	{
		result = scan.Refresh(path, options);
		UAM_ASSERT(result.error.empty() && result.bytes_read <= options.byte_budget);
		if (result.complete) break;
	}
	UAM_ASSERT(result.complete);
	UAM_ASSERT_EQ(scan.Records().size(), std::size_t(2));
	UAM_ASSERT_EQ(scan.InvalidLines(), std::size_t(1));
	UAM_ASSERT_EQ(scan.OversizedLines(), std::size_t(1));
	uam::usage::TranscriptScan limited(uam::provider_ids::kClaudeCli);
	options.byte_budget = 32768;
	options.max_records = 1;
	result = limited.Refresh(path, options);
	UAM_ASSERT(!result.complete && result.error.find("incomplete") != std::string::npos);
	UAM_ASSERT_EQ(limited.Records().size(), std::size_t(1));
	UAM_ASSERT(!limited.Refresh(path, options).error.empty());
	options.byte_budget = 0;
	UAM_ASSERT(!scan.Refresh(path, options).error.empty());
	uam::usage::TranscriptScan unsupported(uam::provider_ids::kCopilotCli);
	UAM_ASSERT(unsupported.Refresh(path).error.find("unavailable") != std::string::npos);
}

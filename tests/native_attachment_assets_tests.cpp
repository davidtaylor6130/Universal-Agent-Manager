#include "test_harness.h"
#include "common/chat/native_attachment_assets.h"
#include "common/chat/chat_message_page_index.h"

#if defined(__APPLE__) || defined(_WIN32)

namespace
{
	namespace assets = uam::native_assets;
	const std::string kAssetOwner = "final-UAM-owner/opaque-native-id";
	const std::string kPngOriginal = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";
	const std::string kWaveOriginal = "UklGRiYAAABXQVZFZm10IBAAAAABAAEAQB8AAEAfAAABAAgAZGF0YQIAAACAgA==";

	assets::Payload Original(const std::string& encoded, const std::string& mime = "application/octet-stream", const std::shared_ptr<assets::PayloadBudget>& budget = std::make_shared<assets::PayloadBudget>())
	{
		std::string error;
		auto payload = assets::MakeInlinePayload(std::string(encoded), mime, budget, error);
		UAM_ASSERT(payload);
		UAM_ASSERT(error.empty());
		return payload;
	}

	std::string OriginalBytes(const std::string& encoded)
	{
		std::string bytes;
		UAM_ASSERT(uam::base64::Decode(encoded, bytes));
		return bytes;
	}

	std::filesystem::path Generation(const std::filesystem::path& root, const assets::PublicationReceipt& receipt)
	{
		return root / std::string(assets::kStoreDirectory) / receipt.owner_hash / receipt.generation;
	}

	void Publish(assets::PreparedBatch& batch)
	{
		std::string error;
		UAM_ASSERT(batch.MarkPublicationAttempted(error));
		UAM_ASSERT(batch.MarkPublicationAttempted(error)); // Retry is idempotent.
		UAM_ASSERT(batch.MarkPublished(error));
		UAM_ASSERT(batch.MarkPublished(error));
		UAM_ASSERT(error.empty());
	}

	void AssertOriginal(const std::filesystem::path& root, const std::string& owner, const assets::AssetReference& ref, const std::string& expected)
	{
		const auto read = assets::ReadOwned(root, owner, ref.key);
		UAM_ASSERT(read.ok);
		UAM_ASSERT(read.error.empty());
		UAM_ASSERT_EQ(read.bytes, expected);
	}
}

UAM_TEST(NativeAssetPayloadSharesReservationAndValidatesOriginalDigests)
{
	auto budget = std::make_shared<assets::PayloadBudget>();
	auto png = Original(kPngOriginal, "image/png", budget);
	UAM_ASSERT_EQ(png->DecodedSize(), std::size_t(70));
	UAM_ASSERT_EQ(png->Digest(), std::string("4ff6ab670a58c14270e034e2090d9a432caa263a14e0a25785386b0c12f880b5"));
	auto shared = png;
	UAM_ASSERT_EQ(shared.get(), png.get());
	UAM_ASSERT_EQ(budget->UsedCount(), std::size_t(1));
	UAM_ASSERT_EQ(budget->UsedBytes(), std::size_t(70));
	png.reset();
	UAM_ASSERT_EQ(budget->UsedBytes(), std::size_t(70));
	shared.reset();
	UAM_ASSERT_EQ(budget->UsedCount(), std::size_t(0));
	UAM_ASSERT_EQ(budget->UsedBytes(), std::size_t(0));
	const auto wave = Original(kWaveOriginal, "audio/wav", budget);
	UAM_ASSERT_EQ(wave->Digest(), std::string("b9ed3a70d0b20bdedf5ac61ed34bfff1f52338312018636a1906e1d83c353a97"));
	UAM_ASSERT_EQ(wave->DecodedSize(), std::size_t(46));
}

UAM_TEST(NativeAssetPayloadRejectsNoncanonicalBase64AndReferenceInputs)
{
	auto budget = std::make_shared<assets::PayloadBudget>();
	std::string error;
	for (const std::string input : {"A", "AA", "AA=", "AA===", "AB==", "AAB=", "=AAA", "AA==AA==", "AA==\n", "AA-_", "file:///private/original.png", "/private/original.png", "C:\\private\\original.png", "https://assets.invalid/original", "data:image/png;base64,AA=="})
	{
		UAM_ASSERT(!assets::MakeInlinePayload(std::string(input), "application/octet-stream", budget, error));
		UAM_ASSERT(!error.empty());
		UAM_ASSERT_EQ(budget->UsedBytes(), std::size_t(0));
		UAM_ASSERT_EQ(budget->UsedCount(), std::size_t(0));
	}
	UAM_ASSERT(!assets::MakeInlinePayload(std::string("AA=="), "image/png\nsecret", budget, error));
	std::stop_source stop;
	stop.request_stop();
	UAM_ASSERT(!assets::MakeInlinePayload(std::string(kPngOriginal), "image/png", budget, error, stop.get_token()));
	UAM_ASSERT(error.find("cancelled") != std::string::npos);
	UAM_ASSERT_EQ(budget->UsedCount(), std::size_t(0));
}

UAM_TEST(NativeAssetPayloadEnforcesOriginalAggregateAndCountBounds)
{
	std::string error;
	auto budget = std::make_shared<assets::PayloadBudget>();
	const std::size_t max_encoded = ((assets::kMaxOriginalBytes + 2) / 3) * 4;
	UAM_ASSERT(!assets::MakeInlinePayload(std::string(max_encoded + 4, 'A'), "application/octet-stream", budget, error));
	UAM_ASSERT(error.find("encoded size limit") != std::string::npos);
	// Same allowed encoded length but no padding decodes two bytes over 25 MiB.
	UAM_ASSERT(!assets::MakeInlinePayload(std::string(max_encoded, 'A'), "application/octet-stream", budget, error));
	UAM_ASSERT(error.find("decoded size limit") != std::string::npos);
	UAM_ASSERT_EQ(budget->UsedCount(), std::size_t(0));

	auto bytes_budget = std::make_shared<assets::PayloadBudget>(assets::Limits{1024, 5, 10});
	auto three = Original("YWJj", "application/octet-stream", bytes_budget);
	UAM_ASSERT(!assets::MakeInlinePayload(std::string("ZGVm"), "application/octet-stream", bytes_budget, error));
	UAM_ASSERT(error.find("aggregate") != std::string::npos);
	three.reset();
	UAM_ASSERT(assets::MakeInlinePayload(std::string("ZGVm"), "application/octet-stream", bytes_budget, error));

	auto count_budget = std::make_shared<assets::PayloadBudget>(assets::Limits{1024, 1024, 1});
	auto one = Original("AA==", "application/octet-stream", count_budget);
	UAM_ASSERT(!assets::MakeInlinePayload(std::string("AQ=="), "application/octet-stream", count_budget, error));
	UAM_ASSERT(error.find("count") != std::string::npos);
	auto invalid = std::make_shared<assets::PayloadBudget>(assets::Limits{assets::kMaxOriginalBytes + 1, assets::kMaxBatchBytes, assets::kMaxAssetCount});
	UAM_ASSERT(!assets::MakeInlinePayload(std::string("AA=="), "application/octet-stream", invalid, error));
}

UAM_TEST(NativeAssetStorePreservesPngWaveNulAndMimeIndependentOriginals)
{
	uam_test::TempDir temp("uam-native-originals");
	const std::string nul_bytes("\0A\0B\xff", 5);
	const auto png = Original(kPngOriginal, "image/png");
	const auto wave = Original(kWaveOriginal, "audio/wav");
	const auto opaque_png = Original(kPngOriginal, "application/x-opaque-original");
	const auto empty_original = Original("");
	UAM_ASSERT_EQ(empty_original->Digest(), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	std::string error;
	auto batch = assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {png, wave, Original(uam::base64::Encode(nul_bytes)), opaque_png, empty_original}, error);
	UAM_ASSERT(batch);
	UAM_ASSERT_EQ(batch->Assets().size(), std::size_t(5));
	UAM_ASSERT_EQ(batch->Assets()[0].key, batch->Assets()[3].key);
	UAM_ASSERT_EQ(batch->Assets()[3].mime_type, std::string("application/x-opaque-original"));
	UAM_ASSERT(batch->Assets()[0].key.find(kAssetOwner) == std::string::npos);
	UAM_ASSERT_EQ(batch->Assets()[0].digest, png->Digest());
	UAM_ASSERT(!assets::ReadOwned(temp.root, kAssetOwner, batch->Assets()[0].key).ok);
	const auto manifest = uam_test::ReadFile(Generation(temp.root, batch->Receipt()) / "prepared.receipt");
	UAM_ASSERT(manifest.find(kPngOriginal) == std::string::npos);
	Publish(*batch);
	AssertOriginal(temp.root, kAssetOwner, batch->Assets()[0], OriginalBytes(kPngOriginal));
	AssertOriginal(temp.root, kAssetOwner, batch->Assets()[1], OriginalBytes(kWaveOriginal));
	AssertOriginal(temp.root, kAssetOwner, batch->Assets()[2], nul_bytes);
	AssertOriginal(temp.root, kAssetOwner, batch->Assets()[3], OriginalBytes(kPngOriginal));
	AssertOriginal(temp.root, kAssetOwner, batch->Assets()[4], "");
}

UAM_TEST(NativeAssetStoreRejectsOversizedOrEmptyBatchesBeforeFilesystemMutation)
{
	uam_test::TempDir temp("uam-native-original-bounds");
	std::string error;
	UAM_ASSERT(!assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {}, error));
	UAM_ASSERT(!error.empty());
	const auto tiny = Original("AA==");
	UAM_ASSERT(!assets::PreparedBatch::Prepare(temp.root, kAssetOwner, std::vector<assets::Payload>(assets::kMaxAssetCount + 1, tiny), error));
	UAM_ASSERT(error.find("256") != std::string::npos);
	UAM_ASSERT(!assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {assets::Payload{}}, error));
	const auto large = Original(uam::base64::Encode(std::string(assets::kMaxOriginalBytes, '\0')));
	UAM_ASSERT(!assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {large, large, large}, error));
	UAM_ASSERT(error.find("50 MiB") != std::string::npos);
	UAM_ASSERT(!std::filesystem::exists(temp.root / std::string(assets::kStoreDirectory)));
}

UAM_TEST(NativeAssetStoreUnpublishedDestructionCleansOnlyItsPrivateGeneration)
{
	uam_test::TempDir temp("uam-native-original-cleanup");
	std::string error;
	const auto payload = Original(kPngOriginal, "image/png");
	auto old = assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {payload}, error);
	UAM_ASSERT(old);
	const auto old_ref = old->Assets().front();
	const auto old_dir = Generation(temp.root, old->Receipt());
	Publish(*old);
	old.reset();
	auto pending = assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {payload}, error);
	UAM_ASSERT(pending);
	const auto pending_dir = Generation(temp.root, pending->Receipt());
	UAM_ASSERT(pending_dir != old_dir);
	pending.reset();
	UAM_ASSERT(!std::filesystem::exists(pending_dir));
	UAM_ASSERT(std::filesystem::exists(old_dir));
	AssertOriginal(temp.root, kAssetOwner, old_ref, OriginalBytes(kPngOriginal));
}

UAM_TEST(NativeAssetStoreCancellationInterruptionAndExceptionCleanUnpublishedBatch)
{
	uam_test::TempDir temp("uam-native-original-cancel");
	std::string error;
	const auto png = Original(kPngOriginal, "image/png"), wave = Original(kWaveOriginal, "audio/wav");
	auto previous = assets::PreparedBatch::Prepare(temp.root, "other-owner", {wave}, error);
	UAM_ASSERT(previous);
	const auto previous_ref = previous->Assets().front();
	Publish(*previous);
	std::stop_source stopped;
	stopped.request_stop();
	UAM_ASSERT(!assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {png}, error, stopped.get_token()));
	UAM_ASSERT(error.find("cancelled") != std::string::npos);
	for (int mode : {0, 1, 2})
	{
		std::stop_source stop;
		const auto interrupted = assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {png, wave}, error, stop.get_token(),
		    [&](assets::PreparationPoint point, std::size_t index)
		    {
			    if (point == assets::PreparationPoint::OriginalSynced && index == 0)
			    {
				    if (mode == 0) stop.request_stop();
				    if (mode == 1) return false;
				    if (mode == 2) throw std::runtime_error("injected preparation failure");
			    }
			    return true;
		    });
		UAM_ASSERT(!interrupted);
		UAM_ASSERT(!error.empty());
		std::size_t generations = 0;
		for (const auto& owner : std::filesystem::directory_iterator(temp.root / std::string(assets::kStoreDirectory)))
			for (const auto& generation : std::filesystem::directory_iterator(owner.path())) { (void)generation; ++generations; }
		UAM_ASSERT_EQ(generations, std::size_t(1));
		AssertOriginal(temp.root, "other-owner", previous_ref, OriginalBytes(kWaveOriginal));
	}
	// Interrupt an actual original write after one bounded chunk, not just preflight.
	const auto large = Original(uam::base64::Encode(std::string(192 * 1024, 'x')));
	int chunks = 0;
	UAM_ASSERT(!assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {large}, error, {}, [&](assets::PreparationPoint point, std::size_t)
	{
		return point != assets::PreparationPoint::OriginalChunk || ++chunks < 2;
	}));
	UAM_ASSERT_EQ(chunks, 2);
	std::size_t remaining = 0;
	for (const auto& owner : std::filesystem::directory_iterator(temp.root / std::string(assets::kStoreDirectory)))
		for (const auto& generation : std::filesystem::directory_iterator(owner.path())) { (void)generation; ++remaining; }
	UAM_ASSERT_EQ(remaining, std::size_t(1));
	AssertOriginal(temp.root, "other-owner", previous_ref, OriginalBytes(kWaveOriginal));
}

UAM_TEST(NativeAssetStorePublicationAttemptRetainsOriginalsAcrossFalseAndException)
{
	uam_test::TempDir temp("uam-native-original-uncertainty");
	std::string error;
	assets::AssetReference ref;
	std::filesystem::path directory;
	struct PrimaryPublicationException {};
	try
	{
		auto batch = assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {Original(kPngOriginal, "image/png")}, error);
		UAM_ASSERT(batch);
		ref = batch->Assets().front();
		directory = Generation(temp.root, batch->Receipt());
		UAM_ASSERT(!batch->MarkPublished(error));
		UAM_ASSERT(batch->MarkPublicationAttempted(error));
		UAM_ASSERT_EQ(batch->Receipt().state, assets::PublicationState::AttemptedUncertain);
		// Exercise the real atomic failure boundary, including a visible new primary.
		const auto primary = temp.root / "test-primary.json";
		UAM_ASSERT(uam::io::AtomicWriteFileDetailed(primary, "previous primary").success);
		const auto primary_temp = uam::io::MakeTempWritePath(primary);
		const std::string primary_text = "{\"asset\":\"" + ref.key + "\"}";
		uam::io::AtomicWriteResult prepared;
		UAM_ASSERT(uam::io::WriteAndSyncAtomicTemp(primary_temp, std::string_view(primary_text), {}, prepared));
		const auto publication = uam::io::CommitSyncedAtomicTemp(primary, primary_temp, false, [](uam::io::AtomicWriteStage stage)
		{
			return stage != uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync;
		});
		UAM_ASSERT(!publication.success);
		UAM_ASSERT_EQ(uam_test::ReadFile(primary), primary_text);
		throw PrimaryPublicationException{};
	}
	catch (const PrimaryPublicationException&) {}
	UAM_ASSERT(std::filesystem::exists(directory));
	AssertOriginal(temp.root, kAssetOwner, ref, OriginalBytes(kPngOriginal));
}

UAM_TEST(NativeAssetStoreFailedAttemptReceiptRetainsOriginalsConservatively)
{
	uam_test::TempDir temp("uam-native-original-attempt-failure");
	std::string error;
	auto batch = assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {Original(kPngOriginal)}, error);
	UAM_ASSERT(batch);
	const auto ref = batch->Assets().front();
	const auto directory = Generation(temp.root, batch->Receipt());
	std::filesystem::create_directory(directory / "attempted.receipt");
	UAM_ASSERT(!batch->MarkPublicationAttempted(error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT_EQ(batch->Receipt().state, assets::PublicationState::AttemptedUncertain);
	UAM_ASSERT(!batch->MarkPublished(error));
	batch.reset();
	UAM_ASSERT_EQ(uam_test::ReadFile(directory / (ref.digest + ".bin")), OriginalBytes(kPngOriginal));
	UAM_ASSERT(!assets::ReadOwned(temp.root, kAssetOwner, ref.key).ok);
}

UAM_TEST(NativeAssetStoreRejectsForeignOwnerFakeKeysAndReadBounds)
{
	uam_test::TempDir temp("uam-native-original-keys");
	std::string error;
	auto batch = assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {Original("AA==")}, error);
	UAM_ASSERT(batch);
	const auto ref = batch->Assets().front();
	Publish(*batch);
	UAM_ASSERT(assets::IsKeyForOwner(kAssetOwner, ref.key));
	UAM_ASSERT(!assets::IsKeyForOwner("foreign-owner", ref.key));
	UAM_ASSERT(!assets::ReadOwned(temp.root, "foreign-owner", ref.key).ok);
	for (const std::string& fake : std::vector<std::string>{"../outside", "/private/file", "file:///private/file", "https://assets.invalid/file", "na1:../outside", ref.key + "/../outside", ref.key + "%2f.."})
	{
		UAM_ASSERT(!assets::IsKeyForOwner(kAssetOwner, fake));
		const auto read = assets::ReadOwned(temp.root, kAssetOwner, fake);
		UAM_ASSERT(!read.ok);
		UAM_ASSERT(read.bytes.empty());
	}
	UAM_ASSERT(!assets::ReadOwned(temp.root, kAssetOwner, ref.key, 0).ok);
	AssertOriginal(temp.root, kAssetOwner, ref, std::string(1, '\0'));
	std::stop_source stop;
	stop.request_stop();
	const auto cancelled = assets::ReadOwned(temp.root, kAssetOwner, ref.key, assets::kMaxOriginalBytes, stop.get_token());
	UAM_ASSERT(!cancelled.ok);
	UAM_ASSERT(cancelled.bytes.empty());
	UAM_ASSERT(cancelled.error.find("cancelled") != std::string::npos);
}

UAM_TEST(NativeAssetStoreRejectsTamperedDigestSizeAndUndeclaredOriginal)
{
	uam_test::TempDir temp("uam-native-original-tamper");
	std::string error;
	auto batch = assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {Original(kPngOriginal)}, error);
	UAM_ASSERT(batch);
	Publish(*batch);
	const auto ref = batch->Assets().front();
	const auto directory = Generation(temp.root, batch->Receipt());
	const auto file = directory / (ref.digest + ".bin");
	UAM_ASSERT(!assets::ReadOwned(temp.root, kAssetOwner, ref.key, 69).ok);
	std::string changed = OriginalBytes(kPngOriginal);
	changed.back() ^= 1;
	UAM_ASSERT(uam::io::WriteTextFile(file, changed));
	UAM_ASSERT(!assets::ReadOwned(temp.root, kAssetOwner, ref.key).ok);
	UAM_ASSERT(uam::io::WriteTextFile(file, OriginalBytes(kPngOriginal) + "x"));
	UAM_ASSERT(!assets::ReadOwned(temp.root, kAssetOwner, ref.key).ok);
	UAM_ASSERT(uam::io::WriteTextFile(file, OriginalBytes(kPngOriginal)));
	const auto other = Original(kWaveOriginal);
	UAM_ASSERT(uam::io::WriteTextFile(directory / (other->Digest() + ".bin"), OriginalBytes(kWaveOriginal)));
	const auto receipt = batch->Receipt();
	const std::string forged = "na1:" + receipt.owner_hash + ':' + receipt.generation + ':' + other->Digest() + ":46";
	UAM_ASSERT(assets::IsKeyForOwner(kAssetOwner, forged));
	UAM_ASSERT(!assets::ReadOwned(temp.root, kAssetOwner, forged).ok);
	AssertOriginal(temp.root, kAssetOwner, ref, OriginalBytes(kPngOriginal));
}

UAM_TEST(NativeAssetStoreTreatsEncodedReferencesAsBytesAndSupportsUnicodeRoot)
{
	uam_test::TempDir temp("uam-native-original-unicode");
	const auto root = temp.root / uam::paths::PathFromUtf8("originals-\xc3\xa9-\xe6\x96\x87");
	std::filesystem::create_directory(root);
	const auto source = temp.root / "source-must-stay.txt";
	UAM_ASSERT(uam::io::WriteTextFile(source, "sentinel"));
	const std::string references = "file:///not-authorized/source\nhttps://assets.invalid/original\n" + uam::paths::Utf8PathString(source);
	std::string error;
	auto batch = assets::PreparedBatch::Prepare(root, kAssetOwner, {Original(uam::base64::Encode(references))}, error);
	UAM_ASSERT(batch);
	Publish(*batch);
	AssertOriginal(root, kAssetOwner, batch->Assets().front(), references);
	UAM_ASSERT_EQ(uam_test::ReadFile(source), std::string("sentinel"));
	UAM_ASSERT(!std::filesystem::exists(temp.root / std::string(assets::kStoreDirectory)));
}

UAM_TEST(NativeAssetStoreRejectsNonDirectoryPrivateStorageWithoutOverwritingIt)
{
	uam_test::TempDir temp("uam-native-original-nondirectory");
	const auto storage = temp.root / std::string(assets::kStoreDirectory);
	UAM_ASSERT(uam::io::WriteTextFile(storage, "foreign storage sentinel"));
	std::string error;
	UAM_ASSERT(!assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {Original(kPngOriginal)}, error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT_EQ(uam_test::ReadFile(storage), std::string("foreign storage sentinel"));
}

UAM_TEST(NativeAssetStoreRejectsHardlinkedOriginalWithoutChangingItsOtherName)
{
	uam_test::TempDir temp("uam-native-original-hardlink");
	std::string error;
	auto batch = assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {Original(kPngOriginal)}, error);
	UAM_ASSERT(batch);
	Publish(*batch);
	const auto ref = batch->Assets().front();
	const auto file = Generation(temp.root, batch->Receipt()) / (ref.digest + ".bin");
	batch.reset();
	const auto outside = temp.root / "outside-hardlink";
	std::filesystem::create_hard_link(file, outside);
	UAM_ASSERT(!assets::ReadOwned(temp.root, kAssetOwner, ref.key).ok);
	UAM_ASSERT_EQ(uam_test::ReadFile(outside), OriginalBytes(kPngOriginal));
}

#if !defined(_WIN32)
UAM_TEST(NativeAssetStoreAllowsRootAliasesButRefusesPrivateLinks)
{
	uam_test::TempDir temp("uam-native-original-links");
	const auto actual = temp.root / "actual", alias = temp.root / "alias", outside = temp.root / "outside";
	std::filesystem::create_directory(actual);
	std::filesystem::create_directory(outside);
	std::filesystem::create_directory_symlink(actual, alias);
	std::string error;
	auto batch = assets::PreparedBatch::Prepare(alias, kAssetOwner, {Original(kPngOriginal)}, error);
	UAM_ASSERT(batch);
	Publish(*batch);
	const auto ref = batch->Assets().front();
	const auto receipt = batch->Receipt();
	batch.reset();
	AssertOriginal(alias, kAssetOwner, ref, OriginalBytes(kPngOriginal));
	const auto directory = Generation(actual, receipt);
	std::filesystem::remove(directory / (ref.digest + ".bin"));
	const auto external = outside / "original.bin";
	UAM_ASSERT(uam::io::WriteTextFile(external, OriginalBytes(kPngOriginal)));
	std::filesystem::create_symlink(external, directory / (ref.digest + ".bin"));
	UAM_ASSERT(!assets::ReadOwned(alias, kAssetOwner, ref.key).ok);
	const auto blocked = temp.root / "blocked";
	std::filesystem::create_directory(blocked);
	std::filesystem::create_directory_symlink(outside, blocked / std::string(assets::kStoreDirectory));
	UAM_ASSERT(!assets::PreparedBatch::Prepare(blocked, kAssetOwner, {Original(kPngOriginal)}, error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT_EQ(uam_test::ReadFile(external), OriginalBytes(kPngOriginal));
	UAM_ASSERT(!std::filesystem::exists(outside / receipt.owner_hash));
}

UAM_TEST(NativeAssetStoreRefusesOwnerAndGenerationAliases)
{
	uam_test::TempDir temp("uam-native-original-scope-links");
	std::string error;
	auto batch = assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {Original(kPngOriginal)}, error);
	UAM_ASSERT(batch);
	Publish(*batch);
	const auto ref = batch->Assets().front();
	const auto receipt = batch->Receipt();
	batch.reset();
	const auto directory = Generation(temp.root, receipt), parked = temp.root / "parked";
	std::filesystem::rename(directory, parked);
	std::filesystem::create_directory_symlink(parked, directory);
	UAM_ASSERT(!assets::ReadOwned(temp.root, kAssetOwner, ref.key).ok);
	std::filesystem::remove(directory);
	std::filesystem::rename(parked, directory);
	const auto owner = directory.parent_path(), parked_owner = temp.root / "parked-owner";
	std::filesystem::rename(owner, parked_owner);
	std::filesystem::create_directory_symlink(parked_owner, owner);
	UAM_ASSERT(!assets::ReadOwned(temp.root, kAssetOwner, ref.key).ok);
	UAM_ASSERT(!assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {Original(kPngOriginal)}, error));
	std::filesystem::remove(owner);
	std::filesystem::rename(parked_owner, owner);
	AssertOriginal(temp.root, kAssetOwner, ref, OriginalBytes(kPngOriginal));
}

UAM_TEST(NativeAssetStoreChangedDirectoryCannotAuthorizeForeignCleanup)
{
	uam_test::TempDir temp("uam-native-original-directory-replacement");
	std::string error;
	auto batch = assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {Original(kPngOriginal)}, error);
	UAM_ASSERT(batch);
	const auto directory = Generation(temp.root, batch->Receipt()), parked = temp.root / "original-generation-moved";
	std::filesystem::rename(directory, parked);
	std::filesystem::create_directory(directory);
	UAM_ASSERT(uam::io::WriteTextFile(directory / "foreign-sentinel", "do not remove"));
	batch.reset();
	UAM_ASSERT_EQ(uam_test::ReadFile(directory / "foreign-sentinel"), std::string("do not remove"));
	UAM_ASSERT(std::filesystem::exists(parked / (Original(kPngOriginal)->Digest() + ".bin")));
}

UAM_TEST(NativeAssetStoreRefusesSharedPermissionsInsidePrivateStorage)
{
	uam_test::TempDir temp("uam-native-original-private-permissions");
	const auto storage = temp.root / std::string(assets::kStoreDirectory);
	std::filesystem::create_directory(storage);
	std::filesystem::permissions(storage, std::filesystem::perms::owner_all | std::filesystem::perms::group_read, std::filesystem::perm_options::replace);
	std::string error;
	UAM_ASSERT(!assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {Original(kPngOriginal)}, error));
	UAM_ASSERT(error.find("exclusively") != std::string::npos);
	UAM_ASSERT(std::filesystem::is_empty(storage));
}
#else
UAM_TEST(NativeAssetStoreWindowsPinsPrivateDirectoriesUntilDisposal)
{
	uam_test::TempDir temp("uam-native-original-pinned-windows");
	std::string error;
	auto batch = assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {Original(kPngOriginal)}, error);
	UAM_ASSERT(batch);
	const auto directory = Generation(temp.root, batch->Receipt());
	for (const auto& path : {directory, directory.parent_path(), directory.parent_path().parent_path()})
	{
		std::error_code rename_error;
		std::filesystem::rename(path, temp.root / "must-not-move", rename_error);
		UAM_ASSERT(rename_error);
		UAM_ASSERT(std::filesystem::is_directory(path));
	}
	batch.reset();
	UAM_ASSERT(!std::filesystem::exists(directory));
}
#endif

#endif

#if defined(__APPLE__) || defined(_WIN32)
UAM_TEST(NativeAssetStoreMatchingAttemptMarkerRequiresOwnSyncWitness)
{
	uam_test::TempDir temp("uam-native-original-unwitnessed-attempt");
	std::string error;
	auto batch = assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {Original(kPngOriginal)}, error);
	UAM_ASSERT(batch);
	const auto receipt = batch->Receipt();
	const auto reference = batch->Assets().front();
	const auto directory = Generation(temp.root, receipt);
	// A complete readable marker is the observable state after a write whose
	// file sync failed. Supplying those bytes does not give this batch a sync witness.
	const std::string marker = "uam-native-assets-v1\nattempted\n" + receipt.owner_hash + '\n' + receipt.generation + '\n';
	UAM_ASSERT(uam::io::WriteTextFile(directory / "attempted.receipt", marker));
	for (int retry = 0; retry < 2; ++retry)
	{
		UAM_ASSERT(!batch->MarkPublicationAttempted(error));
		UAM_ASSERT(!error.empty());
		UAM_ASSERT_EQ(batch->Receipt().state, assets::PublicationState::AttemptedUncertain);
		UAM_ASSERT_EQ(uam_test::ReadFile(directory / "attempted.receipt"), marker);
	}
	batch.reset();
	UAM_ASSERT_EQ(uam_test::ReadFile(directory / (reference.digest + ".bin")), OriginalBytes(kPngOriginal));
}

UAM_TEST(NativeAssetStoreMatchingPublishedMarkerCannotAcknowledgeWithoutSyncWitness)
{
	uam_test::TempDir temp("uam-native-original-unwitnessed-published");
	std::string error;
	auto batch = assets::PreparedBatch::Prepare(temp.root, kAssetOwner, {Original(kPngOriginal)}, error);
	UAM_ASSERT(batch);
	UAM_ASSERT(batch->MarkPublicationAttempted(error));
	const auto receipt = batch->Receipt();
	const auto reference = batch->Assets().front();
	const auto directory = Generation(temp.root, receipt);
	const std::string marker = "uam-native-assets-v1\npublished\n" + receipt.owner_hash + '\n' + receipt.generation + '\n';
	UAM_ASSERT(uam::io::WriteTextFile(directory / "published.receipt", marker));
	for (int retry = 0; retry < 2; ++retry)
	{
		UAM_ASSERT(!batch->MarkPublished(error));
		UAM_ASSERT(!error.empty());
		UAM_ASSERT_EQ(batch->Receipt().state, assets::PublicationState::AttemptedUncertain);
		UAM_ASSERT_EQ(uam_test::ReadFile(directory / "published.receipt"), marker);
	}
	batch.reset();
	AssertOriginal(temp.root, kAssetOwner, reference, OriginalBytes(kPngOriginal));
}


namespace
{
	MessageAttachment NativeUriMetadata()
	{
		MessageAttachment a;
		a.id = "native-uri-part"; a.name = "Recorded PDF"; a.kind = "file";
		a.mime_type = "application/pdf"; a.source = AttachmentSource::ProviderUri;
		a.source_uri = "file:///never-authorized/provider-document.pdf";
		a.origin_execution_host_id = "ssh-recorded-origin";
		a.native_part_key = "v1:exact-recorded-part";
		a.native_tool_call_id = "read-file-tool"; a.native_source_field = "toolCalls[0].result[1]";
		return a;
	}
	ChatSession NativeMetadataChat()
	{
		ChatSession chat; chat.id = "native-media-owner"; chat.title = "Recorded assets";
		chat.provider_id = "gemini-cli"; chat.created_at = "2026-10-16T12:00:00Z";
		chat.updated_at = chat.created_at;
		chat.messages.push_back(Message{MessageRole::User, "Recorded message"});
		return chat;
	}
}

UAM_TEST(NativeAssetRepositoryRetainsOpaqueUriWithoutPath)
{
	uam_test::TempDir temp("uam-native-uri-repository");
	auto chat = NativeMetadataChat();
	const auto uri = NativeUriMetadata(); chat.messages.front().attachments.push_back(uri);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const auto loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(loaded && loaded->messages.size() == 1);
	UAM_ASSERT_EQ(loaded->messages.front().attachments.size(), std::size_t{1});
	UAM_ASSERT_EQ(loaded->messages.front().attachments.front(), uri);
	UAM_ASSERT(loaded->messages.front().attachments.front().path.empty());
	UAM_ASSERT_EQ(loaded->messages.front().attachments.front().source_uri, uri.source_uri);
}

UAM_TEST(NativeAssetRepositoryRetainsOwnedOriginalReference)
{
	uam_test::TempDir temp("uam-native-owned-repository");
	auto chat = NativeMetadataChat(); std::string error;
	auto batch = assets::PreparedBatch::Prepare(temp.root, chat.id, {Original(kPngOriginal, "image/png")}, error);
	UAM_ASSERT(batch && batch->MarkPublicationAttempted(error));
	const auto ref = batch->Assets().front();
	MessageAttachment a; a.id = "owned-image"; a.name = "Recorded image"; a.kind = "image";
	a.mime_type = ref.mime_type; a.size_bytes = ref.size_bytes; a.source = AttachmentSource::OwnedNativeAsset;
	a.native_asset_key = ref.key; a.content_digest = ref.digest;
	a.native_part_key = "v1:exact-image-part"; a.origin_execution_host_id = "ssh-recorded-origin";
	chat.messages.front().attachments.push_back(a);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	UAM_ASSERT(batch->MarkPublished(error));
	const auto loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(loaded && loaded->messages.size() == 1);
	UAM_ASSERT_EQ(loaded->messages.front().attachments.size(), std::size_t{1});
	UAM_ASSERT_EQ(loaded->messages.front().attachments.front(), a);
	const auto original = assets::ReadOwned(temp.root, chat.id, loaded->messages.front().attachments.front().native_asset_key);
	UAM_ASSERT(original.ok); UAM_ASSERT_EQ(original.bytes, OriginalBytes(kPngOriginal));
}

UAM_TEST(NativeAssetRepositoryRejectsUnstagedOriginalBeforeReplacingChat)
{
	uam_test::TempDir temp("uam-native-pending-repository");
	auto chat = NativeMetadataChat(); UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const auto path = AppPaths::UamChatFilePath(temp.root, chat.id);
	const auto before = uam_test::ReadFile(path);
	MessageAttachment a; a.id = "pending-original"; a.name = "Recorded image"; a.kind = "image";
	a.source = AttachmentSource::NativeInlinePending; a.native_inline_payload = Original(kPngOriginal, "image/png");
	a.mime_type = a.native_inline_payload->MimeType(); a.size_bytes = a.native_inline_payload->DecodedSize();
	a.content_digest = a.native_inline_payload->Digest(); a.native_part_key = "v1:pending-part";
	chat.messages.front().attachments.push_back(a);
	UAM_ASSERT(!ChatRepository::SaveChat(temp.root, chat));
	UAM_ASSERT_EQ(uam_test::ReadFile(path), before);
}

UAM_TEST(NativeAssetRepositoryRejectsUnstagedPreparedSave)
{
	uam_test::TempDir temp("uam-native-pending-prepared");
	auto chat = NativeMetadataChat(); UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	MessageAttachment a; a.id = "pending-original"; a.name = "Recorded image"; a.kind = "image";
	a.source = AttachmentSource::NativeInlinePending; a.native_inline_payload = Original(kPngOriginal, "image/png");
	a.mime_type = a.native_inline_payload->MimeType(); a.size_bytes = a.native_inline_payload->DecodedSize();
	a.content_digest = a.native_inline_payload->Digest(); a.native_part_key = "v1:pending-part";
	chat.messages.front().attachments.push_back(a);
	UAM_ASSERT(!ChatRepository::PrepareChatSave(temp.root, chat));
}

UAM_TEST(NativeAssetMessageDigestIncludesSourceAndProvenance)
{
	Message message{MessageRole::User, "Recorded message"}; message.attachments.push_back(NativeUriMetadata());
	const auto digest = [](const Message& m) { std::uint64_t h = 14695981039346656037ULL; uam::chat_pages::AppendMessageDigest(h, m); return h; };
	const auto original = digest(message);
	message.attachments.front().source_uri += "?changed"; UAM_ASSERT(digest(message) != original);
	message.attachments.front() = NativeUriMetadata();
	message.attachments.front().origin_execution_host_id = "different-origin"; UAM_ASSERT(digest(message) != original);
	message.attachments.front() = NativeUriMetadata();
	message.attachments.front().native_part_key += ":different-part"; UAM_ASSERT(digest(message) != original);
}

UAM_TEST(NativeAssetRepositoryRejectsMixedSourceAuthority)
{
	uam_test::TempDir temp("uam-native-mixed-source");
	for (int variant = 0; variant < 3; ++variant)
	{
		auto chat = NativeMetadataChat();
		auto a = NativeUriMetadata();
		if (variant == 0) a.path = "/unrelated/controller/file.pdf";
		if (variant == 1) a.native_asset_key = "another-owner-asset";
		if (variant == 2) a.native_inline_payload = Original(kPngOriginal, "image/png");
		chat.messages.front().attachments.push_back(a);
		UAM_ASSERT(!ChatRepository::SaveChat(temp.root, chat));
	}
}

namespace
{
	ChatSession WriteForeignNativeReference(const std::filesystem::path& root, bool cached)
	{
		auto chat = NativeMetadataChat();
		chat.messages.push_back(Message{MessageRole::Assistant, "Valid tail"});
		UAM_ASSERT(ChatRepository::SaveChat(root, chat));
		std::string error;
		auto batch = assets::PreparedBatch::Prepare(root, "different-final-owner", {Original(kPngOriginal, "image/png")}, error);
		UAM_ASSERT(batch && batch->MarkPublicationAttempted(error));
		const auto ref = batch->Assets().front();
		MessageAttachment a; a.id = "foreign-original"; a.name = "Recorded image"; a.kind = "image";
		a.mime_type = ref.mime_type; a.size_bytes = ref.size_bytes; a.source = AttachmentSource::OwnedNativeAsset;
		a.native_asset_key = ref.key; a.content_digest = ref.digest; a.native_part_key = "v1:foreign-part";
		chat.messages.front().attachments.push_back(a);
		const auto primary = AppPaths::UamChatFilePath(root, chat.id);
		auto document = nlohmann::json::parse(uam_test::ReadFile(primary));
		document["messages"][0]["attachments"] = nlohmann::json::array({{
		    {"id", a.id}, {"name", a.name}, {"kind", a.kind}, {"mime_type", a.mime_type},
		    {"size_bytes", a.size_bytes}, {"source", "owned_native_asset"},
		    {"nativeAssetKey", a.native_asset_key}, {"contentDigest", a.content_digest},
		    {"nativePartKey", a.native_part_key}}});
		UAM_ASSERT(uam::io::WriteTextFile(primary, document.dump()));
		if (cached)
		{
			// A shape-only external/old cache is not an owner-validation receipt.
			uam::chat_pages::Builder builder(primary);
			std::uint64_t begin = 0, finish = 0; std::size_t index = 0;
			UAM_ASSERT(uam::chat_pages::Scan(primary, builder, [&](std::string_view) -> std::optional<Message>
			    { return chat.messages.at(index++); }, begin, finish));
			const auto revision = ChatRepository::ChatStorageRevision(root, chat.id);
			UAM_ASSERT(revision);
			UAM_ASSERT(builder.Finish(uam::chat_pages::IndexPath(primary), *revision,
			    std::filesystem::file_size(primary), begin, finish));
		}
		return chat;
	}
}

UAM_TEST(NativeAssetRepositoryRejectsForeignOriginalInUnloadedSave)
{
	for (bool cached : {false, true})
	{
		uam_test::TempDir temp("uam-native-foreign-unloaded");
		const auto fixture = WriteForeignNativeReference(temp.root, cached);
		const auto primary = AppPaths::UamChatFilePath(temp.root, fixture.id);
		const auto bytes = uam_test::ReadFile(primary);
		UAM_ASSERT(!ChatRepository::LoadLocalChat(temp.root, fixture.id, true));
		auto unloaded = ChatRepository::LoadLocalChat(temp.root, fixture.id, false);
		UAM_ASSERT(unloaded && !unloaded->messages_loaded);
		unloaded->title = "Metadata edit must not bless foreign originals";
		UAM_ASSERT(!ChatRepository::PrepareChatSave(temp.root, *unloaded));
		UAM_ASSERT(!ChatRepository::SaveChat(temp.root, *unloaded));
		UAM_ASSERT_EQ(uam_test::ReadFile(primary), bytes);
	}
}

UAM_TEST(NativeAssetRepositoryRejectsForeignOriginalOutsideRequestedPage)
{
	for (bool cached : {false, true})
	{
		uam_test::TempDir temp("uam-native-foreign-prefix");
		const auto fixture = WriteForeignNativeReference(temp.root, cached);
		auto unloaded = ChatRepository::LoadLocalChat(temp.root, fixture.id, false);
		UAM_ASSERT(unloaded);
		std::string warning;
		UAM_ASSERT(!ChatRepository::ReadMessagePage(temp.root, *unloaded, 1, std::nullopt, &warning));
		UAM_ASSERT(!warning.empty());
	}
}

UAM_TEST(NativeAssetRepositoryRejectsInvalidNativeQueueWithoutPartialHydration)
{
	for (const bool loaded : {false, true})
	{
		uam_test::TempDir temp("uam-native-invalid-primary-queue");
		auto chat = NativeMetadataChat();
		uam::AcpQueuedUserPromptState first; first.id = "queued-first"; first.text = "First retained input";
		first.attachments.push_back(NativeUriMetadata());
		uam::AcpQueuedUserPromptState second; second.id = "queued-second"; second.text = "Second retained input";
		chat.acp_queued_prompts = {first, second};
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		const auto primary = AppPaths::UamChatFilePath(temp.root, chat.id);
		auto document = nlohmann::json::parse(uam_test::ReadFile(primary));
		document["acp_queued_prompts"][0]["attachments"][0]["path"] = "/not-native-authority.pdf";
		UAM_ASSERT(uam::io::WriteTextFile(primary, document.dump()));
		const auto bytes = uam_test::ReadFile(primary);
		UAM_ASSERT(!ChatRepository::LoadLocalChat(temp.root, chat.id, loaded));
		UAM_ASSERT_EQ(uam_test::ReadFile(primary), bytes);
	}
}

UAM_TEST(NativeAssetSerializerRetainsPathlessMetadataWithoutOriginalPayload)
{
	auto chat = NativeMetadataChat();
	chat.messages.front().attachments.push_back(NativeUriMetadata());
	const auto output = uam::StateSerializer::SerializeSession(chat);
	UAM_ASSERT(output["messages"][0].contains("attachments"));
	const auto& projected = output["messages"][0]["attachments"][0];
	UAM_ASSERT_EQ(projected["source"].get<std::string>(), std::string("provider_uri"));
	UAM_ASSERT_EQ(projected["sourceUri"].get<std::string>(), NativeUriMetadata().source_uri);
	UAM_ASSERT_EQ(projected["nativePartKey"].get<std::string>(), NativeUriMetadata().native_part_key);
	UAM_ASSERT(projected.value("path", "").empty());
	UAM_ASSERT(!projected.contains("native_inline_payload") && !projected.contains("dataBase64"));
}

UAM_TEST(NativeAssetRepositoryRejectsUnrepresentableNativeQueueBeforeSave)
{
	for (int variant = 0; variant < 3; ++variant)
	{
		uam_test::TempDir temp("uam-native-queue-admission-bounds");
		auto chat = NativeMetadataChat();
		uam::AcpQueuedUserPromptState prompt; prompt.id = "bounded-native-queue"; prompt.text = "Retain all references";
		for (int index = 0; index < 64; ++index)
		{
			auto a = NativeUriMetadata(); a.id += std::to_string(index); a.native_part_key += std::to_string(index);
			prompt.attachments.push_back(std::move(a));
		}
		chat.acp_queued_prompts.push_back(prompt);
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		const auto loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
		UAM_ASSERT(loaded && loaded->acp_queued_prompts.size() == 1);
		UAM_ASSERT_EQ(loaded->acp_queued_prompts.front().attachments, prompt.attachments);
		const auto path = AppPaths::UamChatFilePath(temp.root, chat.id);
		const auto before = uam_test::ReadFile(path);
		if (variant == 0) chat.acp_queued_prompts.front().attachments.push_back(NativeUriMetadata());
		if (variant == 1)
		{
			for (int index = 1; index < 33; ++index)
			{
				auto extra = prompt; extra.id += std::to_string(index); extra.attachments.resize(1);
				chat.acp_queued_prompts.push_back(std::move(extra));
			}
		}
		if (variant == 2) chat.acp_queued_prompts.front().text.assign(2U * 1024U * 1024U + 1U, 'q');
		UAM_ASSERT(!ChatRepository::SaveChat(temp.root, chat));
		UAM_ASSERT(!ChatRepository::PrepareChatSave(temp.root, chat));
		UAM_ASSERT_EQ(uam_test::ReadFile(path), before);
	}
}

UAM_TEST(NativeAssetRepositoryRejectsNativeQueueBehindLegacyStopBoundaries)
{
	for (int variant = 0; variant < 4; ++variant)
	{
		for (const bool include_messages : {false, true})
		{
			uam_test::TempDir temp("uam-native-queue-recorded-bounds");
			auto chat = NativeMetadataChat();
			uam::AcpQueuedUserPromptState prompt; prompt.id = "recorded-native-queue"; prompt.text = "Keep pending input";
			prompt.attachments.push_back(NativeUriMetadata()); chat.acp_queued_prompts.push_back(prompt);
			UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
			const auto primary = AppPaths::UamChatFilePath(temp.root, chat.id);
			auto document = nlohmann::json::parse(uam_test::ReadFile(primary));
			auto& queued = document["acp_queued_prompts"];
			if (variant == 0)
			{
				for (int index = 1; index < 65; ++index) queued[0]["attachments"].push_back(queued[0]["attachments"][0]);
				queued[0]["attachments"][64]["path"] = "/invalid-native-tail.pdf";
			}
			if (variant == 1) queued[0]["attachments"].insert(queued[0]["attachments"].begin(), 7);
			if (variant == 2)
			{
				for (int index = 1; index < 33; ++index) queued.push_back(queued[0]);
			}
			if (variant == 3) queued[0]["text"] = std::string(2U * 1024U * 1024U + 1U, 'q');
			UAM_ASSERT(uam::io::WriteTextFile(primary, document.dump()));
			const auto before = uam_test::ReadFile(primary);
			UAM_ASSERT(!ChatRepository::LoadLocalChat(temp.root, chat.id, include_messages));
			UAM_ASSERT_EQ(uam_test::ReadFile(primary), before);
		}
	}
}

UAM_TEST(NativeAssetRepositoryRejectsTruncatedNativeQueueDuringBackupRecovery)
{
	uam_test::TempDir temp("uam-native-queue-backup-bounds");
	auto chat = NativeMetadataChat();
	uam::AcpQueuedUserPromptState prompt; prompt.id = "backup-native-input"; prompt.text = "Retain from backup";
	prompt.attachments.push_back(NativeUriMetadata()); chat.acp_queued_prompts.push_back(prompt);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const auto primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	auto document = nlohmann::json::parse(uam_test::ReadFile(primary));
	for (int index = 1; index < 65; ++index)
		document["acp_queued_prompts"][0]["attachments"].push_back(document["acp_queued_prompts"][0]["attachments"][0]);
	const auto backup = uam::io::MakeBackupPath(primary);
	UAM_ASSERT(uam::io::WriteTextFile(backup, document.dump()));
	std::filesystem::remove(primary);
	std::filesystem::remove(AppPaths::UamChatSummaryFilePath(temp.root, chat.id));
	UAM_ASSERT(!ChatRepository::LoadLocalChat(temp.root, chat.id, false));
	// Exercise the actual catalogue backup-recovery selector, not only the
	// single-primary loader's negative control.
	std::string warning;
	UAM_ASSERT(ChatRepository::LoadLocalChatSummaries(temp.root, &warning).empty());
	UAM_ASSERT(!warning.empty());
	UAM_ASSERT(!std::filesystem::exists(primary));
	UAM_ASSERT_EQ(uam_test::ReadFile(backup), document.dump());
}
#endif

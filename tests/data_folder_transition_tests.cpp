#include "data_folder_test_support.h"
#include "app/data_folder_transition.h"
#include "common/paths/path_utils.h"
#include "app/data_folder_settings_service.h"
#include "common/platform/data_folder_writer_lease.h"
#include "common/chat/chat_repository.h"
#include "common/runtime/json_runtime.h"

using namespace uam_test;

namespace
{
	struct TransferFixture
	{
		TempDir temp{"uam-data-folder-transaction"};
		fs::path source = fs::canonical(temp.root) / "source";
		fs::path destination = fs::canonical(temp.root) / "destination";
		fs::path control = source / ".data-folder-control";
		std::shared_ptr<uam::platform::DataRootLock> source_lease, control_lease;
		std::shared_ptr<uam::DataFolderTransition> transaction;
		std::string error;
		TransferFixture()
		{
			fs::create_directories(destination);
			source_lease = DataFolderTestProcess().TryAcquireDataRootLock(source, &error);
			control_lease = DataFolderTestProcess().TryAcquireDataRootLock(control, &error);
			UAM_ASSERT(source_lease && control_lease);
			transaction = uam::DataFolderTransition::Open(control, source, source_lease, control_lease, DataFolderTestProcess(), error);
			UAM_ASSERT(transaction != nullptr);
			for (const auto* path : {"chats/synthetic.json", "chats/synthetic.json.bak", "chat-summaries/synthetic.json", "queue-outboxes/synthetic.json", "agent-runs/synthetic.json", "usage/synthetic.json", "recovery/synthetic.json", "resource_collections.json"})
			{
				fs::create_directories((source / path).parent_path());
				UAM_ASSERT(uam::io::WriteTextFile(source / path, "{\"synthetic\":true,\"retained\":\"authority\"}"));
			}
			fs::create_directory(source / "memory");
			UAM_ASSERT(uam::io::WriteTextFile(source / "memory/knowledge.md", "disposable memory survivor"));
			UAM_ASSERT(uam::io::WriteTextFile(source / "settings.txt", "markdown_store_directory=" + uam::EncodeLineValue((source / "markdown-store").string()) + "\nunknown_setting=preserve-me\n"));
			UAM_ASSERT(uam::io::WriteTextFile(source / "settings.txt.bak", ReadFile(source / "settings.txt")));
			UAM_ASSERT(uam::io::WriteTextFile(source / "folders.txt", "fixture-folder=Retained organisation\n"));
			UAM_ASSERT(transaction->Arm(destination, "fixture/revision", error));
		}
		bool Activate(const uam::DataFolderTransition::Fault& fault = {})
		{
			return transaction->ActivateStopped(true, error, fault);
		}
		void ExpectHeldLocks()
		{
			UAM_ASSERT(!DataFolderTestProcess().TryAcquireDataRootLock(source, &error));
			UAM_ASSERT(!DataFolderTestProcess().TryAcquireDataRootLock(destination, &error));
		}
	};

	std::string FrozenInventory(const fs::path& root)
	{
		std::string error;
		const auto tree = uam::platform::DataFolderTree::Open(root, error);
		uam::platform::DataFolderTree::Inventory entries;
		UAM_ASSERT(tree && tree->Scan(entries, error));
		return nlohmann::json{{"logical", tree->Digest(entries)}, {"objects", tree->ObjectDigest(entries)}, {"receipt", ReadFile(root / ".uam-data-folder/receipt.json")}, {"intent", ReadFile(root / ".uam-data-folder/receipt-intent.json")}}.dump();
	}

	std::string ControlInventory(const fs::path& control)
	{
		nlohmann::json records = nlohmann::json::object();
		for (const auto& entry : fs::recursive_directory_iterator(control))
			if (entry.is_regular_file() && entry.path().filename() != ".uam-data-root.lock")
				records[entry.path().lexically_relative(control).generic_string()] = ReadFile(entry.path());
		return records.dump();
	}

	std::shared_ptr<uam::DataFolderSettingsService> ReopenSettings(TransferFixture& fixture)
	{
		fixture.transaction.reset();
		fixture.source_lease.reset();
		fixture.control_lease.reset();
		auto service = uam::DataFolderSettingsService::Open(fixture.source, {}, DataFolderTestProcess(), fixture.error);
		UAM_ASSERT(service);
		std::shared_ptr<uam::platform::DataRootLock> owner = DataFolderTestProcess().TryAcquireDataRootLock(service->StartupRoot(), &fixture.error);
		UAM_ASSERT(owner && service->AttachStartupOwner(owner, false, fixture.error));
		return service;
	}
} // namespace

UAM_TEST(DataFolderSettingsRejectsOriginalAndHistoricalRootOverlap)
{
	TransferFixture fixture;
	fs::create_directory(fixture.source / "empty");
	UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
	const auto next = fs::canonical(fixture.temp.root) / "next";
	fs::create_directory(next);
	UAM_ASSERT(fixture.transaction->Arm(next, "fixture/second", fixture.error));
	UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
	const auto service = ReopenSettings(fixture);
	const auto original = FrozenInventory(fixture.source), history = FrozenInventory(fixture.destination);
	const auto control = ControlInventory(fixture.control);
	const auto revision = service->Read().revision;
	for (const auto& destination : {fixture.source / "empty", fixture.source, fixture.control, fixture.destination / "empty", fixture.destination, fixture.source.parent_path()})
	{
		UAM_ASSERT(!service->Choose(destination, revision, "forbidden-overlap").committed);
		UAM_ASSERT(service->Read().revision == revision && service->Read().can_change);
		UAM_ASSERT_EQ(FrozenInventory(fixture.source), original);
		UAM_ASSERT_EQ(FrozenInventory(fixture.destination), history);
		UAM_ASSERT_EQ(ControlInventory(fixture.control), control);
	}
	fs::path selected;
	UAM_ASSERT(uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error) && selected == next);
	fixture.ExpectHeldLocks();
}

UAM_TEST(DataFolderTransferMalformedAcknowledgementPhaseRefusesWithoutThrowing)
{
	using Json = nlohmann::json;
	for (const bool root_receipt : {false, true})
	{
		TransferFixture fixture;
		UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
		const auto intent_path = root_receipt ? fixture.destination / ".uam-data-folder/receipt-intent.json" : fixture.control / "transition-intent.json";
		const auto original = Json::parse(ReadFile(intent_path));
		const auto source = FrozenInventory(fixture.source);
		for (const Json& phase : std::vector<Json>{nullptr, 17, true, Json::object(), Json::array()})
		{
			auto malformed = original;
			malformed["phase"] = phase;
			UAM_ASSERT(uam::io::WriteTextFile(intent_path, malformed.dump()));
			const auto destination = FrozenInventory(fixture.destination), control = ControlInventory(fixture.control);
			fs::path selected;
			UAM_ASSERT(!uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error));
			if (root_receipt)
				UAM_ASSERT(!uam::DataFolderTransition::CheckExternalSourceFence(fixture.destination, fixture.error, true));
			UAM_ASSERT_EQ(FrozenInventory(fixture.source), source);
			UAM_ASSERT_EQ(FrozenInventory(fixture.destination), destination);
			UAM_ASSERT_EQ(ControlInventory(fixture.control), control);
		}
		// Reacquisition must retain unknown publication as held, without a fallback or repair.
		fixture.transaction.reset();
		fixture.source_lease.reset();
		std::shared_ptr<uam::platform::DataRootLock> owner = DataFolderTestProcess().TryAcquireDataRootLock(fixture.destination, &fixture.error);
		fixture.transaction = uam::DataFolderTransition::Open(fixture.control, fixture.destination, owner, fixture.control_lease, DataFolderTestProcess(), fixture.error);
		UAM_ASSERT(fixture.transaction && fixture.transaction->Held());
		UAM_ASSERT(!fixture.transaction->RollbackBeforeActivation(fixture.error));
		fixture.ExpectHeldLocks();
	}
}

UAM_TEST(DataFolderTransferArmRejectsOriginalAndHistoricalRootOverlap)
{
	TransferFixture fixture;
	fs::create_directory(fixture.source / "empty");
	UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
	// The former current-source-only check admits this empty frozen descendant.
	UAM_ASSERT(uam::InspectDataFolderTransition(fixture.destination, fixture.source / "empty", false).state == uam::DataFolderPreflightState::RequiresStoppedValidation);
	const auto first_inventory = FrozenInventory(fixture.source), first_control = ControlInventory(fixture.control);
	UAM_ASSERT(!fixture.transaction->Arm(fixture.source / "empty", "fixture/first-rejected", fixture.error));
	UAM_ASSERT(fixture.transaction->State() == "active" && fixture.transaction->CurrentRoot() == fixture.destination);
	UAM_ASSERT_EQ(FrozenInventory(fixture.source), first_inventory);
	UAM_ASSERT_EQ(ControlInventory(fixture.control), first_control);
	const auto next = fs::canonical(fixture.temp.root) / "next";
	fs::create_directory(next);
	UAM_ASSERT(fixture.transaction->Arm(next, "fixture/second", fixture.error));
	UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
	const auto original = FrozenInventory(fixture.source), history = FrozenInventory(fixture.destination);
	const auto control = ControlInventory(fixture.control);
	for (const auto& destination : {fixture.source / "empty", fixture.source, fixture.control, fixture.destination / "empty", fixture.destination, fixture.source.parent_path()})
	{
		UAM_ASSERT(fixture.transaction->InspectDestination(destination).blocker == uam::DataFolderPreflightBlocker::OverlappingRoots);
		UAM_ASSERT(!fixture.transaction->Arm(destination, "fixture/rejected", fixture.error));
		UAM_ASSERT(fixture.transaction->State() == "active" && fixture.transaction->CurrentRoot() == next);
		UAM_ASSERT_EQ(FrozenInventory(fixture.source), original);
		UAM_ASSERT_EQ(FrozenInventory(fixture.destination), history);
		UAM_ASSERT_EQ(ControlInventory(fixture.control), control);
	}
	fixture.transaction.reset();
	fixture.source_lease.reset();
	std::shared_ptr<uam::platform::DataRootLock> owner = DataFolderTestProcess().TryAcquireDataRootLock(next, &fixture.error);
	fixture.transaction = uam::DataFolderTransition::Open(fixture.control, next, owner, fixture.control_lease, DataFolderTestProcess(), fixture.error);
	UAM_ASSERT(fixture.transaction && !fixture.transaction->Held());
	fixture.ExpectHeldLocks();
}

UAM_TEST(DataFolderTransferRejectsCurrentAndRetainedPassiveSnapshotOverlap)
{
	TransferFixture fixture;
	fs::create_directory(fixture.source / "empty");
	UAM_ASSERT(!fixture.Activate([](std::string_view phase, uam::io::AtomicWriteStage) { return phase != "beforeActivation"; }));
	UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
	const auto passive = FrozenInventory(fixture.destination);
	const auto control = ControlInventory(fixture.control);
	for (const auto& destination : {fixture.destination / "empty", fixture.destination})
	{
		UAM_ASSERT(fixture.transaction->InspectDestination(destination).blocker == uam::DataFolderPreflightBlocker::OverlappingRoots);
		UAM_ASSERT(!fixture.transaction->Arm(destination, "fixture/rejected", fixture.error));
		UAM_ASSERT(fixture.transaction->State() == "rolledBack");
		UAM_ASSERT_EQ(FrozenInventory(fixture.destination), passive);
		UAM_ASSERT_EQ(ControlInventory(fixture.control), control);
	}
	const auto next = fs::canonical(fixture.temp.root) / "next";
	fs::create_directory(next);
	UAM_ASSERT(fixture.transaction->Arm(next, "fixture/after-rollback", fixture.error));
	UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
	const auto service = ReopenSettings(fixture);
	const auto active_control = ControlInventory(fixture.control);
	const auto revision = service->Read().revision;
	for (const auto& destination : {fixture.destination / "empty", fixture.destination})
	{
		UAM_ASSERT(!service->Choose(destination, revision, "passive-overlap").committed);
		UAM_ASSERT(service->Read().revision == revision && service->Read().can_change);
		UAM_ASSERT_EQ(FrozenInventory(fixture.destination), passive);
		UAM_ASSERT_EQ(ControlInventory(fixture.control), active_control);
	}
	fs::path selected;
	UAM_ASSERT(uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error) && selected == next);
	fixture.ExpectHeldLocks();
}

UAM_TEST(DataFolderSettingsRejectsFirstSourceAndCurrentPassiveDescendants)
{
	for (const bool passive : {false, true})
	{
		TransferFixture fixture;
		fs::create_directory(fixture.source / "empty");
		if (passive)
		{
			UAM_ASSERT(!fixture.Activate([](std::string_view phase, uam::io::AtomicWriteStage) { return phase != "beforeActivation"; }));
			UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
		}
		else
			UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
		const auto service = ReopenSettings(fixture);
		const auto protected_root = passive ? fixture.destination : fixture.source;
		const auto before = FrozenInventory(protected_root), control = ControlInventory(fixture.control);
		const auto revision = service->Read().revision;
		UAM_ASSERT(service->Read().can_change);
		UAM_ASSERT(!service->Choose(protected_root / "empty", revision, "first-forbidden-descendant").committed);
		UAM_ASSERT(service->Read().revision == revision && service->Read().can_change);
		UAM_ASSERT_EQ(FrozenInventory(protected_root), before);
		UAM_ASSERT_EQ(ControlInventory(fixture.control), control);
		fixture.ExpectHeldLocks();
		// Refusal preserves ordinary selection/cancel and the exact passive owner.
		const auto separate = fs::canonical(fixture.temp.root) / "separate";
		fs::create_directory(separate);
		const auto choice = service->Choose(separate, revision, "valid-separate-choice");
		UAM_ASSERT(choice.committed);
		UAM_ASSERT(service->Cancel(choice.settings.revision, "cancel-separate-choice").committed);
		UAM_ASSERT_EQ(FrozenInventory(protected_root), before);
	}
}

#if !defined(_WIN32)
UAM_TEST(DataFolderTransferRevalidatesArmedOverlapBeforeLockMarkers)
{
	// Move the same selected inode through an ancestor alias after arming. Retained
	// roots stay pinned; no marker, receipt, copy or ledger write may follow this change.
	for (const bool passive_snapshot : {false, true})
	{
		for (const bool rearm : {false, true})
		{
			TransferFixture fixture;
			if (passive_snapshot)
			{
				UAM_ASSERT(!fixture.Activate([](std::string_view phase, uam::io::AtomicWriteStage) { return phase != "beforeActivation"; }));
				UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
			}
			else
				UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
			const auto parent = fs::canonical(fixture.temp.root) / "outside-parent";
			const auto selected = parent / "empty";
			fs::create_directories(selected);
			UAM_ASSERT(fixture.transaction->Arm(selected, "fixture/followup", fixture.error));
			const auto protected_root = passive_snapshot ? fixture.destination : fixture.source;
			const auto before = FrozenInventory(protected_root);
			const auto moved = protected_root / "moved-parent";
			fs::rename(parent, moved);
			fs::create_directory_symlink(moved, parent);
			const auto externally_changed = FrozenInventory(protected_root);
			const auto control = ControlInventory(fixture.control);
			UAM_ASSERT(fixture.transaction->InspectDestination(selected).blocker == uam::DataFolderPreflightBlocker::OverlappingRoots);
			UAM_ASSERT(!(rearm ? fixture.transaction->Arm(selected, "fixture/retry", fixture.error) : fixture.transaction->ActivateStopped(true, fixture.error)));
			UAM_ASSERT(fixture.error.find("retained data folders") != std::string::npos);
			UAM_ASSERT_EQ(FrozenInventory(protected_root), externally_changed);
			UAM_ASSERT_EQ(ControlInventory(fixture.control), control);
			UAM_ASSERT(fs::is_empty(selected)); // No root/writer marker was created.
			fs::remove(parent);
			fs::rename(moved, parent); // Undo only this test's injected filesystem change.
			UAM_ASSERT_EQ(FrozenInventory(protected_root), before);
			UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
			const auto current = fixture.transaction->CurrentRoot();
			fixture.transaction.reset();
			fixture.source_lease.reset();
			std::shared_ptr<uam::platform::DataRootLock> owner = DataFolderTestProcess().TryAcquireDataRootLock(current, &fixture.error);
			fixture.transaction = uam::DataFolderTransition::Open(fixture.control, current, owner, fixture.control_lease, DataFolderTestProcess(), fixture.error);
			UAM_ASSERT(fixture.transaction && !fixture.transaction->Held());
			UAM_ASSERT_EQ(FrozenInventory(protected_root), before);
		}
	}
}
#endif

UAM_TEST(DataFolderTransferUnknownStagingRefusesReleasedOwnerWriters)
{
	for (const auto failure : {uam::io::AtomicWriteStage::BeforeReplace, uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync})
	{
		TransferFixture fixture;
		const auto original = nlohmann::json::parse(FrozenInventory(fixture.source));
		UAM_ASSERT(!fixture.Activate([&](std::string_view phase, uam::io::AtomicWriteStage stage) { return phase != "staging" || stage != failure; }));
		const auto source = FrozenInventory(fixture.source), destination = FrozenInventory(fixture.destination), control = ControlInventory(fixture.control);
		UAM_ASSERT(nlohmann::json::parse(source)["logical"] == original["logical"]);
		UAM_ASSERT(nlohmann::json::parse(source)["objects"] == original["objects"]);
		fixture.transaction.reset();
		fixture.source_lease.reset();
		fixture.control_lease.reset();
		auto writer = uam::platform::DataFolderWriterLease::TryAcquire(fixture.source, uam::platform::DataFolderWriterLease::Access::Shared, fixture.error);
		UAM_ASSERT(writer);
		UAM_ASSERT(!uam::DataFolderTransition::CheckExternalSourceFence(fixture.source, fixture.error, true));
		fs::path selected;
		UAM_ASSERT(!uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error));
		UAM_ASSERT_EQ(FrozenInventory(fixture.source), source);
		UAM_ASSERT_EQ(FrozenInventory(fixture.destination), destination);
		UAM_ASSERT_EQ(ControlInventory(fixture.control), control);
		UAM_ASSERT(!fs::exists(fixture.destination / "chats"));
	}
}

UAM_TEST(DataFolderTransferArmedFollowupCancellationKeepsCurrentWriterAuthority)
{
	{
		TransferFixture fixture;
		UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
		UAM_ASSERT(!fs::exists(fixture.source / ".uam-data-folder"));
		UAM_ASSERT(uam::DataFolderTransition::CheckExternalSourceFence(fixture.source, fixture.error));
	}
	for (bool restored : {false, true})
	{
		TransferFixture fixture;
		if (restored)
		{
			UAM_ASSERT(!fixture.Activate([](std::string_view phase, uam::io::AtomicWriteStage) { return phase != "beforeActivation"; }));
			UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
		}
		else UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
		const auto current = restored ? fixture.source : fixture.destination;
		const auto historical = restored ? fixture.destination : fixture.source;
		const auto retained = FrozenInventory(historical);
		const auto original = nlohmann::json::parse(FrozenInventory(current));
		const auto next = fs::canonical(fixture.temp.root) / "cancelled-followup";
		fs::create_directory(next);
		UAM_ASSERT(fixture.transaction->Arm(next, "cancel/followup", fixture.error));
		UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
		{
			auto writer = uam::platform::DataFolderWriterLease::TryAcquire(current, uam::platform::DataFolderWriterLease::Access::Shared, fixture.error);
			UAM_ASSERT(writer && uam::DataFolderTransition::CheckExternalSourceFence(current, fixture.error, true));
			UAM_ASSERT(!uam::DataFolderTransition::CheckExternalSourceFence(historical, fixture.error, true));
		}
		fixture.transaction.reset();
		fixture.source_lease.reset();
		fixture.control_lease.reset();
		fixture.control_lease = DataFolderTestProcess().TryAcquireDataRootLock(fixture.control, &fixture.error);
		std::shared_ptr<uam::platform::DataRootLock> owner = DataFolderTestProcess().TryAcquireDataRootLock(current, &fixture.error);
		fixture.transaction = uam::DataFolderTransition::Open(fixture.control, current, owner, fixture.control_lease, DataFolderTestProcess(), fixture.error);
		UAM_ASSERT(fixture.transaction && !fixture.transaction->Held());
		fs::path selected;
		UAM_ASSERT(uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error) && selected == current);
		auto writer = uam::platform::DataFolderWriterLease::TryAcquire(current, uam::platform::DataFolderWriterLease::Access::Shared, fixture.error);
		UAM_ASSERT(writer && uam::DataFolderTransition::CheckExternalSourceFence(current, fixture.error, true));
		UAM_ASSERT(!uam::DataFolderTransition::CheckExternalSourceFence(historical, fixture.error, true));
		UAM_ASSERT(!DataFolderTestProcess().TryAcquireDataRootLock(historical, &fixture.error));
		UAM_ASSERT_EQ(FrozenInventory(historical), retained);
		const auto after = nlohmann::json::parse(FrozenInventory(current));
		UAM_ASSERT(after["logical"] == original["logical"] && after["objects"] == original["objects"]);
		UAM_ASSERT(!fs::exists(next / ".uam-data-root.lock") && !fs::exists(next / "chats"));
	}
}

#if !defined(_WIN32)
UAM_TEST(DataFolderTreeRejectsCaptureOpenDirectorySwap)
{
	TransferFixture fixture;
	const auto other = fs::canonical(fixture.temp.root) / "other", parked = fs::canonical(fixture.temp.root) / "parked";
	fs::create_directories(other / "recovery");
	UAM_ASSERT(uam::io::WriteTextFile(other / "recovery/foreign.json", "foreign profile survivor"));
	const auto source = FrozenInventory(fixture.source), foreign = FrozenInventory(other), control = ControlInventory(fixture.control);
	bool captured = false, opened = false;
	const auto tree = uam::platform::DataFolderTree::Open(fixture.source, fixture.error, [&](uam::platform::DataFolderTree::OpenStage stage)
	{
		if (stage == uam::platform::DataFolderTree::OpenStage::IdentityRetained)
		{
			captured = true;
			fs::rename(fixture.source, parked);
			fs::rename(other, fixture.source);
		}
		else
		{
			opened = true;
			fs::rename(fixture.source, other);
			fs::rename(parked, fixture.source);
		}
	});
	UAM_ASSERT(captured && opened && fixture.source_lease->Current());
	UAM_ASSERT(!tree);
	UAM_ASSERT_EQ(FrozenInventory(fixture.source), source);
	UAM_ASSERT_EQ(FrozenInventory(other), foreign);
	UAM_ASSERT_EQ(ControlInventory(fixture.control), control);
	UAM_ASSERT(!fs::exists(other / ".uam-data-folder") && !fs::exists(other / ".uam-data-root.lock"));
}
#endif

UAM_TEST(DataFolderRestoredStartupRequiresExactSourceReceipt)
{
	using Json = nlohmann::json;
	for (int route : {0, 1, 2}) // Passive rollback; armed cancellation from active/restored roots.
		for (const std::string mutation : {"missingReceipt", "missingAck", "malformedAck", "stale", "wrongDigest", "wrongIdentity", "wrongPhase"})
		{
			TransferFixture fixture;
			const bool active = route == 1;
			if (active)
				UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
			else
			{
				UAM_ASSERT(!fixture.Activate([](std::string_view phase, uam::io::AtomicWriteStage) { return phase != "beforeActivation"; }));
				if (route == 2)
					UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
			}
			const auto current = active ? fixture.destination : fixture.source;
			const auto historical = active ? fixture.source : fixture.destination;
			const auto stale = ReadFile(current / ".uam-data-folder/receipt.json"), stale_ack = ReadFile(current / ".uam-data-folder/receipt-intent.json");
			const auto next = fs::canonical(fixture.temp.root) / "next";
			fs::create_directory(next);
			if (route != 0)
				UAM_ASSERT(fixture.transaction->Arm(next, "receipt/cancel", fixture.error));
			UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
			const auto receipt_path = current / ".uam-data-folder/receipt.json", ack_path = current / ".uam-data-folder/receipt-intent.json";
			auto receipt = Json::parse(ReadFile(receipt_path)), ack = Json::parse(ReadFile(ack_path));
			if (mutation == "missingReceipt")
				UAM_ASSERT(fs::remove(receipt_path));
			else if (mutation == "missingAck")
				UAM_ASSERT(fs::remove(ack_path));
			else if (mutation == "malformedAck")
			{
				ack["phase"] = 17;
				UAM_ASSERT(uam::io::WriteTextFile(ack_path, ack.dump()));
			}
			else if (mutation == "stale")
			{
				UAM_ASSERT(uam::io::WriteTextFile(receipt_path, stale));
				UAM_ASSERT(uam::io::WriteTextFile(ack_path, stale_ack));
			}
			else
			{
				if (mutation == "wrongDigest") receipt["destinationDigest"] = std::string(64, 'a');
				if (mutation == "wrongIdentity") receipt["identity"] = "foreign-directory";
				if (mutation == "wrongPhase") receipt["phase"] = "active";
				ack["after"] = receipt;
				UAM_ASSERT(uam::io::WriteTextFile(receipt_path, receipt.dump()));
				UAM_ASSERT(uam::io::WriteTextFile(ack_path, ack.dump()));
			}
			const auto source = FrozenInventory(current), retained = FrozenInventory(historical), control = ControlInventory(fixture.control);
			UAM_ASSERT(!uam::DataFolderTransition::CheckExternalSourceFence(current, fixture.error, true));
			fs::path selected;
			UAM_ASSERT(!uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error));
			fixture.transaction.reset();
			fixture.source_lease.reset();
			fixture.control_lease.reset();
			fixture.control_lease = DataFolderTestProcess().TryAcquireDataRootLock(fixture.control, &fixture.error);
			std::shared_ptr<uam::platform::DataRootLock> owner = DataFolderTestProcess().TryAcquireDataRootLock(current, &fixture.error);
			UAM_ASSERT(owner && fixture.control_lease);
			fixture.transaction = uam::DataFolderTransition::Open(fixture.control, current, owner, fixture.control_lease, DataFolderTestProcess(), fixture.error);
			UAM_ASSERT(fixture.transaction && fixture.transaction->Held());
			const auto choice = fs::canonical(fixture.temp.root) / "refused";
			fs::create_directory(choice);
			UAM_ASSERT(!fixture.transaction->Arm(choice, "refused/receipt", fixture.error));
			UAM_ASSERT(!fixture.transaction->RollbackBeforeActivation(fixture.error));
			UAM_ASSERT(!uam::DataFolderTransition::CheckExternalSourceFence(current, fixture.error, true));
			UAM_ASSERT_EQ(FrozenInventory(current), source);
			UAM_ASSERT_EQ(FrozenInventory(historical), retained);
			UAM_ASSERT_EQ(ControlInventory(fixture.control), control);
			UAM_ASSERT(!fs::exists(choice / ".uam-data-root.lock"));
		}
}

UAM_TEST(DataFolderRestoredArmRejectsStalePriorReceipt)
{
	TransferFixture fixture;
	UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
	const auto stale = ReadFile(fixture.destination / ".uam-data-folder/receipt.json"), ack = ReadFile(fixture.destination / ".uam-data-folder/receipt-intent.json");
	const auto next = fs::canonical(fixture.temp.root) / "cancel";
	fs::create_directory(next);
	UAM_ASSERT(fixture.transaction->Arm(next, "stale/cancel", fixture.error));
	UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.destination / ".uam-data-folder/receipt.json", stale));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.destination / ".uam-data-folder/receipt-intent.json", ack));
	const auto source = FrozenInventory(fixture.destination), retained = FrozenInventory(fixture.source), control = ControlInventory(fixture.control);
	UAM_ASSERT(!fixture.transaction->Arm(next, "stale/refused", fixture.error));
	UAM_ASSERT(fixture.transaction->Held());
	UAM_ASSERT_EQ(FrozenInventory(fixture.destination), source);
	UAM_ASSERT_EQ(FrozenInventory(fixture.source), retained);
	UAM_ASSERT_EQ(ControlInventory(fixture.control), control);
}

UAM_TEST(DataFolderPlainCancellationRequiresReceiptFreeSource)
{
	for (bool foreign_receipt : {false, true})
	{
		TransferFixture fixture;
		UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
		fs::path selected;
		UAM_ASSERT(uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error));
		fixture.transaction.reset();
		fixture.transaction = uam::DataFolderTransition::Open(fixture.control, fixture.source, fixture.source_lease, fixture.control_lease, DataFolderTestProcess(), fixture.error);
		UAM_ASSERT(fixture.transaction && !fixture.transaction->Held());
		fs::create_directory(fixture.source / ".uam-data-folder");
		if (foreign_receipt)
			UAM_ASSERT(uam::io::WriteTextFile(fixture.source / ".uam-data-folder/receipt.json", "{\"foreign\":true}"));
		const auto source = FrozenInventory(fixture.source), control = ControlInventory(fixture.control);
		UAM_ASSERT(!uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error));
		UAM_ASSERT(!fixture.transaction->RollbackBeforeActivation(fixture.error));
		fixture.transaction.reset();
		fixture.transaction = uam::DataFolderTransition::Open(fixture.control, fixture.source, fixture.source_lease, fixture.control_lease, DataFolderTestProcess(), fixture.error);
		UAM_ASSERT(fixture.transaction && fixture.transaction->Held());
		UAM_ASSERT(!fixture.transaction->Arm(fixture.destination, "foreign/plain", fixture.error));
		UAM_ASSERT_EQ(FrozenInventory(fixture.source), source);
		UAM_ASSERT_EQ(ControlInventory(fixture.control), control);
	}
}

UAM_TEST(DataFolderTransferPreservesModificationTimeAuthority)
{
	TransferFixture fixture;
	const auto base = fs::file_time_type::clock::now() - std::chrono::hours(48);
	const fs::path primary = "chats/synthetic.json", summary = "chat-summaries/synthetic.json", queue = "queue-outboxes/synthetic.json";
	fs::last_write_time(fixture.source / primary, base + std::chrono::duration_cast<fs::file_time_type::duration>(std::chrono::nanoseconds(1234567)));
	fs::last_write_time(fixture.source / summary, base + std::chrono::duration_cast<fs::file_time_type::duration>(std::chrono::seconds(3) + std::chrono::nanoseconds(7654321)));
	const auto before = FrozenInventory(fixture.source);
	UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
	for (const auto& path : {primary, summary, queue})
	{
		UAM_ASSERT(fs::last_write_time(fixture.destination / path) == fs::last_write_time(fixture.source / path));
		UAM_ASSERT_EQ(ReadFile(fixture.destination / path), ReadFile(fixture.source / path));
	}
	const auto original = nlohmann::json::parse(before), after = nlohmann::json::parse(FrozenInventory(fixture.source));
	UAM_ASSERT(original["logical"] == after["logical"] && original["objects"] == after["objects"]);
}

UAM_TEST(DataFolderTransferRejectsIndependentRemoteDeliveryMarkers)
{
	using Json = nlohmann::json;
	for (const auto* file : {"chats/synthetic.json", "chats/synthetic.json.bak"})
		for (const auto* field : {"remote_active_turn_id", "remote_prompt_delivery_session_id", "remote_prompt_delivery_id", "remote_prompt_delivery_payload", "remote_interaction_responses", "remote_manual_compaction_pending"})
			for (const Json& value : std::vector<Json>{std::string("uncertain-delivery"), Json::array({Json{{"request_id_json", "1"}, {"response_json", "{}"}}}), 17, true, nullptr})
			{
				TransferFixture fixture;
				Json record{{"remote_process_exists", false}, {"remote_stop_cleanup_pending", false}, {"remote_restart_pending", false}, {"remote_source_exit_pending", false}, {"remote_turn_reconnect_pending", false}, {"remote_pending_requests", Json::array()}, {"acp_dispatched_queued_prompt_count", 0}, {"acp_queued_prompts", Json::array({Json{{"id", "authored"}, {"text", "retain me"}, {"prepared_for_delivery", false}}})}};
				record[field] = value;
				UAM_ASSERT(uam::io::WriteTextFile(fixture.source / file, record.dump()));
				const auto source = FrozenInventory(fixture.source);
				UAM_ASSERT(!fixture.Activate());
				UAM_ASSERT(fixture.transaction->Held());
				UAM_ASSERT_EQ(FrozenInventory(fixture.source), source);
				UAM_ASSERT(!fs::exists(fixture.destination / "chats") && !fs::exists(fixture.source / ".uam-data-folder"));
				UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
			}
}

#if !defined(_WIN32)
UAM_TEST(DataFolderRestoredReopenRefusesMetadataDirectorySwap)
{
	for (int window : {0, 1, 2}) // Receipt open; acknowledgement open; receipt removed between paired reads.
	{
		TransferFixture fixture;
		UAM_ASSERT(!fixture.Activate([](std::string_view phase, uam::io::AtomicWriteStage) { return phase != "beforeActivation"; }));
		UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
		const auto replacement = fs::canonical(fixture.temp.root) / "replacement", parked = fs::canonical(fixture.temp.root) / "parked-metadata";
		fs::create_directory(replacement);
		if (window == 2)
			fs::create_directory(parked);
		UAM_ASSERT(uam::io::WriteTextFile(replacement / "receipt-intent.json", ReadFile(fixture.source / ".uam-data-folder/receipt-intent.json")));
		const auto source = nlohmann::json::parse(FrozenInventory(fixture.source));
		const auto retained = FrozenInventory(fixture.destination), control = ControlInventory(fixture.control);
		fixture.transaction.reset();
		bool swapped = false;
		fixture.transaction = uam::DataFolderTransition::Open(fixture.control, fixture.source, fixture.source_lease, fixture.control_lease, DataFolderTestProcess(), fixture.error, [&](const fs::path& path)
		{
			if (!swapped && path == fs::path(window == 0 ? ".uam-data-folder/receipt.json" : ".uam-data-folder/receipt-intent.json"))
			{
				if (window == 2)
					fs::rename(fixture.source / ".uam-data-folder/receipt.json", parked / "receipt.json");
				else
				{
					fs::rename(fixture.source / ".uam-data-folder", parked);
					fs::rename(replacement, fixture.source / ".uam-data-folder");
				}
				swapped = true;
			}
		});
		UAM_ASSERT(swapped && fixture.transaction && fixture.transaction->Held());
		UAM_ASSERT(!fixture.transaction->Arm(fixture.destination, "swapped/metadata", fixture.error));
		UAM_ASSERT(!uam::DataFolderTransition::CheckExternalSourceFence(fixture.source, fixture.error, true));
		fs::path selected;
		UAM_ASSERT(!uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error));
		const auto after = nlohmann::json::parse(FrozenInventory(fixture.source));
		UAM_ASSERT(source["logical"] == after["logical"] && source["objects"] == after["objects"]);
		UAM_ASSERT_EQ(FrozenInventory(fixture.destination), retained);
		UAM_ASSERT_EQ(ControlInventory(fixture.control), control);
		UAM_ASSERT(!fs::exists(fixture.source / ".uam-data-folder/receipt.json") && fs::exists(parked / "receipt.json"));
	}
}
#endif

UAM_TEST(DataFolderTransferPreservesRepositorySummaryAndQueueAuthority)
{
	TransferFixture fixture;
	ChatSession chat;
	chat.id = "owner-authority";
	chat.provider_id = "gemini-cli";
	chat.native_session_id = "fixture-native-session";
	chat.created_at = "2026-10-20T12:00:00Z";
	chat.updated_at = chat.created_at;
	chat.messages.push_back(Message{MessageRole::Assistant, "Earlier retained reply"});
	uam::AcpQueuedUserPromptState prompt;
	prompt.id = "authored-input";
	prompt.text = "Original authored input";
	chat.acp_queued_prompts.push_back(prompt);
	UAM_ASSERT(ChatRepository::SaveChat(fixture.source, chat));
	chat.acp_queued_prompts.front().text = "Edited authored input";
	++chat.acp_queued_prompts.front().revision;
	chat.acp_queue_held = true;
	UAM_ASSERT(ChatRepository::SaveQueuedPromptOutbox(fixture.source, chat));
	chat.last_opened_at = "2026-10-21T12:00:00Z";
	chat.interaction_at = chat.last_opened_at;
	chat.attention_revision = "newer-overlay";
	chat.settled_at = "2026-10-21T11:00:00Z";
	chat.snoozed_until_ms = 1792600000000LL;
	UAM_ASSERT(ChatRepository::SaveLastOpenedAt(fixture.source, chat));
	const auto before = ChatRepository::LoadLocalChat(fixture.source, chat.id);
	UAM_ASSERT(before && !before->acp_queue_storage_error && before->attention_revision == chat.attention_revision);
	const auto primary_revision = ChatRepository::ChatStorageRevision(fixture.source, chat.id);
	const auto queue = ReadFile(fixture.source / "queue-outboxes/owner-authority.json");
	UAM_ASSERT(!nlohmann::json::parse(queue)["acp_queued_prompts"].front().contains("managed_turn"));
	UAM_ASSERT(!nlohmann::json::parse(ReadFile(fixture.source / "chats/owner-authority.json"))["acp_queued_prompts"].front().contains("managed_turn"));
	const auto summary = ReadFile(fixture.source / "chat-summaries/owner-authority.json");
	UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
	UAM_ASSERT(ChatRepository::ChatStorageRevision(fixture.destination, chat.id) == primary_revision);
	for (bool messages : {false, true})
	{
		const auto loaded = ChatRepository::LoadLocalChat(fixture.destination, chat.id, messages);
		UAM_ASSERT(loaded && !loaded->acp_queue_storage_error && loaded->acp_queue_held);
		UAM_ASSERT(loaded->last_opened_at == chat.last_opened_at && loaded->interaction_at == chat.interaction_at);
		UAM_ASSERT(loaded->attention_revision == chat.attention_revision && loaded->settled_at == chat.settled_at && loaded->snoozed_until_ms == chat.snoozed_until_ms);
		UAM_ASSERT(loaded->acp_queued_prompts.size() == 1 && loaded->acp_queued_prompts.front().text == "Edited authored input");
		UAM_ASSERT(loaded->acp_queued_prompts.front().revision == chat.acp_queued_prompts.front().revision && !loaded->acp_queued_prompts.front().prepared_for_delivery);
	}
	UAM_ASSERT_EQ(ReadFile(fixture.destination / "queue-outboxes/owner-authority.json"), queue);
	UAM_ASSERT_EQ(ReadFile(fixture.destination / "chat-summaries/owner-authority.json"), summary);
	UAM_ASSERT_EQ(ReadFile(fixture.source / "queue-outboxes/owner-authority.json"), queue);
	UAM_ASSERT_EQ(ReadFile(fixture.source / "chat-summaries/owner-authority.json"), summary);
}

namespace
{
	void ExpectNullManagedMarkerRefused(const fs::path& relative)
	{
		TransferFixture fixture;
		ChatSession chat;
		chat.id = "managed-marker";
		chat.provider_id = "gemini-cli";
		chat.native_session_id = "fixture-native-session";
		chat.created_at = "2026-10-20T12:00:00Z";
		chat.updated_at = chat.created_at;
		uam::AcpQueuedUserPromptState prompt;
		prompt.id = "authored-input";
		prompt.text = "Retain original authored input";
		chat.acp_queued_prompts.push_back(prompt);
		UAM_ASSERT(ChatRepository::SaveChat(fixture.source, chat));
		const auto primary = ReadFile(fixture.source / "chats/managed-marker.json");
		if (relative.parent_path() == "queue-outboxes")
		{
			chat.acp_queued_prompts.front().text = "Retain journal input";
			++chat.acp_queued_prompts.front().revision;
			UAM_ASSERT(ChatRepository::SaveQueuedPromptOutbox(fixture.source, chat));
		}
		else if (relative.extension() == ".bak")
			UAM_ASSERT(uam::io::WriteTextFile(fixture.source / relative, primary));
		auto record = nlohmann::json::parse(ReadFile(fixture.source / relative));
		UAM_ASSERT(!record["acp_queued_prompts"].front().contains("managed_turn"));
		record["acp_queued_prompts"].front()["managed_turn"] = nullptr;
		const auto unresolved = record.dump();
		UAM_ASSERT(uam::io::WriteTextFile(fixture.source / relative, unresolved));

		// Couple admission to the real owning decoder without letting it recover or
		// publish into the source under test. Backup bytes use that same chat decoder.
		const auto probe = fs::canonical(fixture.temp.root) / "owner-probe";
		fs::create_directories(probe / "chats");
		UAM_ASSERT(uam::io::WriteTextFile(probe / "chats/managed-marker.json", relative.parent_path() == "queue-outboxes" ? primary : unresolved));
		if (relative.parent_path() == "queue-outboxes")
		{
			fs::create_directory(probe / "queue-outboxes");
			UAM_ASSERT(uam::io::WriteTextFile(probe / relative, unresolved));
		}
		for (bool messages : {false, true})
		{
			const auto loaded = ChatRepository::LoadLocalChat(probe, chat.id, messages, nullptr, false);
			UAM_ASSERT(loaded && loaded->managed_followup_input_unavailable && loaded->acp_queue_held && loaded->acp_queue_storage_error);
		}
		const auto source = FrozenInventory(fixture.source), destination = FrozenInventory(fixture.destination);
		UAM_ASSERT(!fixture.Activate());
		UAM_ASSERT(fixture.transaction->Held() && fixture.transaction->CurrentRoot() == fixture.source);
		UAM_ASSERT_EQ(FrozenInventory(fixture.source), source);
		UAM_ASSERT_EQ(FrozenInventory(fixture.destination), destination);
		UAM_ASSERT(!fs::exists(fixture.source / ".uam-data-folder") && !fs::exists(fixture.destination / "chats"));
		UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
		UAM_ASSERT_EQ(FrozenInventory(fixture.source), source);
		UAM_ASSERT_EQ(ReadFile(fixture.source / relative), unresolved);
	}
}

UAM_TEST(DataFolderTransferNullManagedMarkerPrimaryRemainsHeld)
{
	ExpectNullManagedMarkerRefused("chats/managed-marker.json");
}
UAM_TEST(DataFolderTransferNullManagedMarkerBackupRemainsHeld)
{
	ExpectNullManagedMarkerRefused("chats/managed-marker.json.bak");
}
UAM_TEST(DataFolderTransferNullManagedMarkerJournalRemainsHeld)
{
	ExpectNullManagedMarkerRefused("queue-outboxes/managed-marker.json");
}

UAM_TEST(DataFolderTransferRejectsLegacyPendingInteractionResponses)
{
	using Json = nlohmann::json;
	for (const auto* file : {"chats/synthetic.json", "chats/synthetic.json.bak"})
		for (const auto& legacy : std::vector<Json>{
			Json{{"remote_interaction_response_request_id", "1"}, {"remote_interaction_response_json", "{\"result\":{}}"}},
			Json{{"remote_interaction_response_request_id", "1"}},
			Json{{"remote_interaction_response_json", "{\"result\":{}}"}},
			Json{{"remote_interaction_response_request_id", 17}, {"remote_interaction_response_json", nullptr}}})
		{
			TransferFixture fixture;
			Json record{{"execution_host_id", "fixture-remote"}, {"imported_read_only", false}, {"remote_process_exists", false}, {"remote_turn_reconnect_pending", false}, {"remote_pending_requests", Json::array()}, {"acp_queued_prompts", Json::array({Json{{"id", "authored"}, {"text", "retain input"}, {"prepared_for_delivery", false}}})}};
			record.update(legacy);
			UAM_ASSERT(!record.contains("remote_interaction_responses"));
			UAM_ASSERT(uam::io::WriteTextFile(fixture.source / file, record.dump()));
			const auto source = FrozenInventory(fixture.source);
			UAM_ASSERT(!fixture.Activate());
			UAM_ASSERT(fixture.transaction->Held());
			UAM_ASSERT_EQ(FrozenInventory(fixture.source), source);
			UAM_ASSERT(!fs::exists(fixture.source / ".uam-data-folder") && !fs::exists(fixture.destination / "chats"));
			UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
		}
	// The owner admits empty fallback fields, and ignores legacy fields when the
	// modern response array is explicitly present. Preserve those settled bytes.
	for (bool modern : {false, true})
	{
		TransferFixture fixture;
		Json record{{"remote_process_exists", false}, {"remote_turn_reconnect_pending", false}, {"remote_interaction_response_request_id", modern ? "old-request" : ""}, {"remote_interaction_response_json", modern ? "{\"historical\":true}" : ""}};
		if (modern)
			record["remote_interaction_responses"] = Json::array();
		UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "chats/synthetic.json", record.dump()));
		UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
		UAM_ASSERT_EQ(ReadFile(fixture.source / "chats/synthetic.json"), record.dump());
		UAM_ASSERT_EQ(ReadFile(fixture.destination / "chats/synthetic.json"), record.dump());
	}
}

#if !defined(_WIN32)
namespace
{
	void ExpectBootstrapPrimaryRetained(const std::string& admission)
	{
		for (bool restored : {false, true})
		for (bool replaced : {false, true})
		{
			TransferFixture fixture;
			if (restored)
			{
				UAM_ASSERT(!fixture.Activate([](std::string_view phase, uam::io::AtomicWriteStage) { return phase != "beforeActivation"; }));
				UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
			}
			else UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
			const auto current = restored ? fixture.source : fixture.destination;
			const auto source = FrozenInventory(fixture.source), destination = FrozenInventory(fixture.destination);
			const auto primary = ReadFile(fixture.control / "transition.json"), acknowledgement = ReadFile(fixture.control / "transition-intent.json");
			const auto retained = fs::canonical(fixture.temp.root) / "retained-transition.json";
			bool changed = false;
			std::string control;
			const auto observer = [&](const fs::path& path)
			{
				if (!changed && path == fs::path("transition-intent.json"))
				{
					fs::rename(fixture.control / "transition.json", retained);
					if (replaced)
						UAM_ASSERT(uam::io::WriteTextFile(fixture.control / "transition.json", "{\"foreign\":true}"));
					changed = true;
					control = ControlInventory(fixture.control);
				}
			};
			fs::path selected;
			if (admission == "external")
				UAM_ASSERT(!uam::DataFolderTransition::CheckExternalSourceFence(current, fixture.error, true, observer));
			else if (admission == "startup")
				UAM_ASSERT(!uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error, observer));
			else
			{
				fixture.transaction.reset();
				fixture.source_lease.reset();
				std::shared_ptr<uam::platform::DataRootLock> owner = DataFolderTestProcess().TryAcquireDataRootLock(current, &fixture.error);
				UAM_ASSERT(owner);
				fixture.transaction = uam::DataFolderTransition::Open(fixture.control, current, owner, fixture.control_lease, DataFolderTestProcess(), fixture.error, {}, observer);
				UAM_ASSERT(fixture.transaction && fixture.transaction->Held());
			}
			UAM_ASSERT(changed);
			UAM_ASSERT(!uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error));
			UAM_ASSERT(!uam::DataFolderTransition::CheckExternalSourceFence(current, fixture.error, true));
			UAM_ASSERT_EQ(FrozenInventory(fixture.source), source);
			UAM_ASSERT_EQ(FrozenInventory(fixture.destination), destination);
			UAM_ASSERT_EQ(ControlInventory(fixture.control), control);
			UAM_ASSERT_EQ(ReadFile(retained), primary);
			if (replaced)
				UAM_ASSERT_EQ(ReadFile(fixture.control / "transition.json"), "{\"foreign\":true}");
			else
				UAM_ASSERT(!fs::exists(fixture.control / "transition.json"));
			UAM_ASSERT_EQ(ReadFile(fixture.control / "transition-intent.json"), acknowledgement);
		}
	}
}
UAM_TEST(DataFolderExternalAdmissionRetainsBootstrapPrimary)
{
	ExpectBootstrapPrimaryRetained("external");
}
UAM_TEST(DataFolderStartupAdmissionRetainsBootstrapPrimary)
{
	ExpectBootstrapPrimaryRetained("startup");
	// The final relative default fallback is still a supported routing input.
	TransferFixture fixture;
	fs::path selected;
	UAM_ASSERT(uam::DataFolderTransition::ProbeStartupRoot(fs::relative(fixture.source), {}, selected, fixture.error));
	UAM_ASSERT(selected == fixture.source);
}
UAM_TEST(DataFolderDirectReopenRetainsBootstrapPrimary)
{
	ExpectBootstrapPrimaryRetained("reopen");
}
#endif

UAM_TEST(DataFolderTransferRefusesDuplicateOwnerJsonKeys)
{
	for (const auto* file : {"chats/synthetic.json", "chats/synthetic.json.bak"})
		for (const auto* text : {
			"{\"remote_prompt_delivery_id\":\"uncertain\",\"remote_prompt_delivery_id\":\"\"}",
			"{\"remote_interaction_responses\":[{\"request_id_json\":\"1\",\"response_json\":\"{}\"}],\"remote_interaction_responses\":[]}",
			"{\"acp_queued_prompts\":[{\"id\":\"prepared\",\"prepared_for_delivery\":true}],\"acp_queued_prompts\":[]}",
			"{\"acp_queued_prompts\":[{\"id\":\"prepared\",\"prepared_for_delivery\":true,\"prepared_for_delivery\":false}]}"})
		{
			TransferFixture fixture;
			const auto owner = ParseJson(text);
			UAM_ASSERT(owner && owner->type == JsonValue::Type::Object);
			if (const auto* delivery = owner->Find("remote_prompt_delivery_id"))
				UAM_ASSERT(delivery->string_value == "uncertain");
			if (const auto* responses = owner->Find("remote_interaction_responses"))
				UAM_ASSERT(responses->array_value.size() == 1);
			if (const auto* queued = owner->Find("acp_queued_prompts"))
				UAM_ASSERT(queued->array_value.size() == 1 && queued->array_value.front().Find("prepared_for_delivery")->bool_value);
			UAM_ASSERT(uam::io::WriteTextFile(fixture.source / file, text));
			const auto source = FrozenInventory(fixture.source);
			UAM_ASSERT(!fixture.Activate());
			UAM_ASSERT(fixture.transaction->Held());
			UAM_ASSERT_EQ(FrozenInventory(fixture.source), source);
			UAM_ASSERT(!fs::exists(fixture.source / ".uam-data-folder") && !fs::exists(fixture.destination / "chats"));
			UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
		}
}

UAM_TEST(DataFolderTransferConfirmedHandoff)
{
	TransferFixture fixture;
	const auto original = ReadFile(fixture.source / "settings.txt");
	UAM_ASSERT(!fixture.transaction->ActivateStopped(false, fixture.error));
	UAM_ASSERT(fixture.Activate());
	UAM_ASSERT(fixture.transaction->State() == "starting");
	UAM_ASSERT(fixture.transaction->ConfirmDestinationOwner(fixture.error));
	UAM_ASSERT(fixture.transaction->CurrentRoot() == fixture.destination && fixture.transaction->State() == "active");
	UAM_ASSERT_EQ(ReadFile(fixture.source / "settings.txt"), original);
	UAM_ASSERT(ReadFile(fixture.destination / "settings.txt").find(uam::EncodeLineValue(uam::paths::Utf8PathString(fixture.destination / "markdown-store"))) != std::string::npos);
	UAM_ASSERT(ReadFile(fixture.destination / "settings.txt").find("unknown_setting=preserve-me") != std::string::npos);
	for (const auto* path : {"chats/synthetic.json", "chats/synthetic.json.bak", "chat-summaries/synthetic.json", "queue-outboxes/synthetic.json", "agent-runs/synthetic.json", "usage/synthetic.json", "recovery/synthetic.json", "resource_collections.json", "folders.txt", "memory/knowledge.md"})
		UAM_ASSERT_EQ(ReadFile(fixture.destination / path), ReadFile(fixture.source / path));
	UAM_ASSERT_EQ(ReadFile(fixture.source / "settings.txt.bak"), original);
	fixture.ExpectHeldLocks();
	fs::path selected;
	UAM_ASSERT(uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error) && selected == fixture.destination);
}

UAM_TEST(DataFolderTransferDestinationLocked)
{
	TransferFixture fixture;
	auto foreign = DataFolderTestProcess().TryAcquireDataRootLock(fixture.destination, &fixture.error);
	UAM_ASSERT(foreign != nullptr && !fixture.Activate());
	UAM_ASSERT(fixture.transaction->Held() && fixture.transaction->CurrentRoot() == fixture.source);
	UAM_ASSERT(!fs::exists(fixture.destination / "chats"));
}

UAM_TEST(DataFolderTransferDestinationNotEmpty)
{
	TransferFixture fixture;
	UAM_ASSERT(uam::io::WriteTextFile(fixture.destination / "foreign", "foreign survivor"));
	UAM_ASSERT(!fixture.Activate());
	UAM_ASSERT_EQ(ReadFile(fixture.destination / "foreign"), "foreign survivor");
	UAM_ASSERT(!fs::exists(fixture.destination / "chats"));
}

UAM_TEST(DataFolderTransferSourceChangesAfterFreeze)
{
	TransferFixture fixture;
	bool changed = false;
	UAM_ASSERT(!fixture.Activate(
	    [&](std::string_view phase, uam::io::AtomicWriteStage)
	    {
		    if (phase == "copy" && !changed)
		    {
			    changed = true;
			    UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "chats/synthetic.json", "{\"newer\":true}"));
		    }
		    return true;
	    }));
	UAM_ASSERT(changed && fixture.transaction->Held());
	UAM_ASSERT_EQ(ReadFile(fixture.source / "chats/synthetic.json"), "{\"newer\":true}");
	fixture.ExpectHeldLocks();
}

UAM_TEST(DataFolderTransferLinkInInventory)
{
	TransferFixture fixture;
	fs::create_hard_link(fixture.source / "memory/knowledge.md", fixture.source / "linked-memory");
	UAM_ASSERT(!fixture.Activate());
	UAM_ASSERT(!fs::exists(fixture.destination / "memory/knowledge.md"));
	UAM_ASSERT_EQ(ReadFile(fixture.source / "memory/knowledge.md"), "disposable memory survivor");
}

UAM_TEST(DataFolderTransferCrashAfterCopyLeavesPassiveStage)
{
	TransferFixture fixture;
	UAM_ASSERT(!fixture.Activate([](std::string_view phase, uam::io::AtomicWriteStage) { return phase != "beforeActivation"; }));
	UAM_ASSERT(fixture.transaction->Held());
	UAM_ASSERT(fs::exists(fixture.destination / "queue-outboxes/synthetic.json"));
	fs::path selected;
	UAM_ASSERT(uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error) && selected == fixture.source);
	fixture.ExpectHeldLocks();
}

UAM_TEST(DataFolderTransferUnknownActivationPublication)
{
	TransferFixture fixture;
	UAM_ASSERT(!fixture.Activate([](std::string_view phase, uam::io::AtomicWriteStage stage) { return !(phase == "starting" && stage == uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync); }));
	fs::path selected;
	UAM_ASSERT(!uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error));
	UAM_ASSERT(!fixture.transaction->RollbackBeforeActivation(fixture.error));
	fixture.ExpectHeldLocks();
}

UAM_TEST(DataFolderTransferUnknownDestinationStart)
{
	TransferFixture fixture;
	UAM_ASSERT(fixture.Activate());
	UAM_ASSERT(!fixture.transaction->ConfirmDestinationOwner(fixture.error, [](std::string_view phase, uam::io::AtomicWriteStage stage) { return !(phase == "destination" && stage == uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync); }));
	UAM_ASSERT(!fixture.transaction->RollbackBeforeActivation(fixture.error));
	fs::path selected;
	UAM_ASSERT(!uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error));
	fixture.ExpectHeldLocks();
}

UAM_TEST(DataFolderTransferRetainedQueueMemoryReceipts)
{
	TransferFixture fixture;
	const nlohmann::json journal{{"dataRoot", fixture.source.string()}, {"inputId", "synthetic-input"}, {"fullWriteObserved", true}, {"delivered", "unknown"}};
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "queue-outboxes/synthetic.json", journal.dump()));
	UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
	UAM_ASSERT_EQ(ReadFile(fixture.destination / "queue-outboxes/synthetic.json"), journal.dump());
	UAM_ASSERT_EQ(ReadFile(fixture.destination / "memory/knowledge.md"), "disposable memory survivor");
	UAM_ASSERT_EQ(ReadFile(fixture.destination / "recovery/synthetic.json"), ReadFile(fixture.source / "recovery/synthetic.json"));
	UAM_ASSERT(!fs::exists(fixture.destination / ".data-folder-control"));
}

UAM_TEST(DataFolderTransferRollbackBeforeActivation)
{
	TransferFixture fixture;
	UAM_ASSERT(!fixture.Activate([](std::string_view phase, uam::io::AtomicWriteStage) { return phase != "beforeActivation"; }));
	UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
	UAM_ASSERT(fixture.transaction->State() == "rolledBack" && fixture.transaction->CurrentRoot() == fixture.source);
	UAM_ASSERT(fs::exists(fixture.destination / "chats/synthetic.json")); // Retain the passive image, never delete unknown/new work.
	UAM_ASSERT(!uam::DataFolderTransition::CheckExternalSourceFence(fixture.source, fixture.error));
	UAM_ASSERT(uam::DataFolderTransition::CheckExternalSourceFence(fixture.source, fixture.error, true));
	UAM_ASSERT(!uam::DataFolderSettingsService::Open(fixture.source, fixture.source, DataFolderTestProcess(), fixture.error));
	fs::path selected;
	UAM_ASSERT(uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error) && selected == fixture.source);
}

UAM_TEST(DataFolderTransferRollbackAfterPossibleWrites)
{
	TransferFixture fixture;
	const auto source = ReadFile(fixture.source / "chats/synthetic.json");
	UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.destination / "chats/synthetic.json", "{\"destinationNewer\":true}"));
	UAM_ASSERT(!fixture.transaction->RollbackBeforeActivation(fixture.error));
	UAM_ASSERT_EQ(ReadFile(fixture.source / "chats/synthetic.json"), source);
	UAM_ASSERT_EQ(ReadFile(fixture.destination / "chats/synthetic.json"), "{\"destinationNewer\":true}");
	fixture.ExpectHeldLocks();
}

UAM_TEST(DataFolderTransferUnsupportedExternalReference)
{
	TransferFixture fixture;
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "resource_collections.json", nlohmann::json{{"workspaceDirectory", (fixture.source / "managed-workspace").string()}}.dump()));
	UAM_ASSERT(!fixture.Activate());
	UAM_ASSERT(!fs::exists(fixture.destination / "chats"));
	UAM_ASSERT(fs::exists(fixture.source / "resource_collections.json"));
}

UAM_TEST(DataFolderSettingsConnectsPendingChoiceToStoppedActivation)
{
	TempDir temp("uam-data-folder-connected");
	const fs::path source = fs::canonical(temp.root) / "source";
	const fs::path destination = fs::canonical(temp.root) / "destination";
	fs::create_directory(destination);
	std::string error;
	auto owner = DataFolderTestProcess().TryAcquireDataRootLock(source, &error);
	auto service = uam::DataFolderSettingsService::Open(source, {}, DataFolderTestProcess(), error);
	UAM_ASSERT(service && owner && service->AttachStartupOwner(std::shared_ptr<uam::platform::DataRootLock>(owner.release()), false, error));
	UAM_ASSERT(uam::io::WriteTextFile(source / "authority", "saved original"));
	const auto choice = service->Choose(destination, service->Read().revision, "choose-disposable");
	UAM_ASSERT(choice.committed && service->ArmRestart(choice.settings.revision).committed);
	service.reset();
	owner = DataFolderTestProcess().TryAcquireDataRootLock(source, &error);
	service = uam::DataFolderSettingsService::Open(source, {}, DataFolderTestProcess(), error);
	UAM_ASSERT(service && owner && service->AttachStartupOwner(std::shared_ptr<uam::platform::DataRootLock>(owner.release()), true, error));
	UAM_ASSERT(service->StartupRoot() == destination && service->Read().pending_folder.empty());
	UAM_ASSERT_EQ(ReadFile(destination / "authority"), "saved original");
	UAM_ASSERT(!DataFolderTestProcess().TryAcquireDataRootLock(source, &error));
}

UAM_TEST(DataFolderTransferSubsequentChoiceRetainsEverySourceOwner)
{
	TransferFixture fixture;
	UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
	const fs::path next = fs::canonical(fixture.temp.root) / "next";
	fs::create_directory(next);
	UAM_ASSERT(uam::io::WriteTextFile(fixture.destination / "newer", "destination authored history"));
	UAM_ASSERT(fixture.transaction->Arm(next, "fixture/second", fixture.error));
	UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
	UAM_ASSERT_EQ(ReadFile(next / "newer"), "destination authored history");
	fixture.ExpectHeldLocks();
	UAM_ASSERT(!DataFolderTestProcess().TryAcquireDataRootLock(next, &fixture.error));
	fixture.transaction.reset();
	fixture.source_lease.reset();
	fixture.control_lease.reset();
	fixture.control_lease = DataFolderTestProcess().TryAcquireDataRootLock(fixture.control, &fixture.error);
	std::shared_ptr<uam::platform::DataRootLock> current = DataFolderTestProcess().TryAcquireDataRootLock(next, &fixture.error);
	auto reopened = uam::DataFolderTransition::Open(fixture.control, next, current, fixture.control_lease, DataFolderTestProcess(), fixture.error);
	UAM_ASSERT(reopened && !reopened->Held() && reopened->CurrentRoot() == next);
	UAM_ASSERT(!DataFolderTestProcess().TryAcquireDataRootLock(fixture.source, &fixture.error));
	UAM_ASSERT(!DataFolderTestProcess().TryAcquireDataRootLock(fixture.destination, &fixture.error));
	fs::path selected;
	UAM_ASSERT(uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error) && selected == next);
}

UAM_TEST(DataFolderTransferForeignReceiptCannotBeReplaced)
{
	TransferFixture fixture;
	bool injected = false;
	UAM_ASSERT(!fixture.Activate(
	    [&](std::string_view phase, uam::io::AtomicWriteStage stage)
	    {
		    if (!injected && phase == "beforeActivation")
		    {
			    injected = true;
			    UAM_ASSERT(uam::io::WriteTextFile(fixture.destination / ".uam-data-folder/receipt.json", "{\"foreign\":true}"));
		    }
		    return true;
	    }));
	UAM_ASSERT(injected && fixture.transaction->Held());
	UAM_ASSERT_EQ(ReadFile(fixture.destination / ".uam-data-folder/receipt.json"), "{\"foreign\":true}");
	UAM_ASSERT(!fixture.transaction->RollbackBeforeActivation(fixture.error));
	fixture.ExpectHeldLocks();
}

UAM_TEST(DataFolderTransferPassiveRecoveryPrecedesAnyWriterStartup)
{
	TransferFixture fixture;
	UAM_ASSERT(!fixture.Activate([](std::string_view phase, uam::io::AtomicWriteStage) { return phase != "beforeActivation"; }));
	fixture.transaction.reset();
	fixture.source_lease.reset();
	fixture.control_lease.reset();
	auto service = uam::DataFolderSettingsService::Open(fixture.source, {}, DataFolderTestProcess(), fixture.error);
	UAM_ASSERT(service != nullptr);
	std::shared_ptr<uam::platform::DataRootLock> owner = DataFolderTestProcess().TryAcquireDataRootLock(fixture.source, &fixture.error);
	UAM_ASSERT(!service->AttachStartupOwner(owner, false, fixture.error));
	UAM_ASSERT(service->AttachStartupOwner(owner, false, fixture.error, true));
	UAM_ASSERT(service->StartupRoot() == fixture.source);
	UAM_ASSERT(fs::exists(fixture.destination / "memory/knowledge.md"));
	UAM_ASSERT(!DataFolderTestProcess().TryAcquireDataRootLock(fixture.destination, &fixture.error));
}

UAM_TEST(DataFolderTransferPreparedDeliveryRemainsAtOriginalAuthority)
{
	TransferFixture fixture;
	const nlohmann::json journal{{"acp_dispatched_queued_prompt_count", 1}, {"acp_queued_prompts", {{{"id", "original-input"}, {"prepared_for_delivery", true}}}}};
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "queue-outboxes/synthetic.json", journal.dump()));
	UAM_ASSERT(!fixture.Activate());
	UAM_ASSERT_EQ(ReadFile(fixture.source / "queue-outboxes/synthetic.json"), journal.dump());
	UAM_ASSERT(!fs::exists(fixture.destination / "queue-outboxes"));
	fixture.ExpectHeldLocks();
}

UAM_TEST(DataFolderTransferUnpreparedAuthoredQueueRemainsByteExact)
{
	TransferFixture fixture;
	const nlohmann::json chat{{"id", "synthetic"}, {"provider_id", "codex"}, {"acp_queue_held", true}, {"acp_dispatched_queued_prompt_count", 0}, {"acp_queued_prompts", {{{"id", "original-input"}, {"revision", 3}, {"text", "authored input"}, {"prepared_for_delivery", false}}}}};
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "chats/synthetic.json", chat.dump()));
	UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
	UAM_ASSERT_EQ(ReadFile(fixture.source / "chats/synthetic.json"), chat.dump());
	UAM_ASSERT_EQ(ReadFile(fixture.destination / "chats/synthetic.json"), chat.dump());
}

UAM_TEST(DataFolderTransferUnknownActivePublicationDoesNotBecomeSuccessOnReopen)
{
	TransferFixture fixture;
	UAM_ASSERT(fixture.Activate());
	UAM_ASSERT(!fixture.transaction->ConfirmDestinationOwner(fixture.error, [](std::string_view phase, uam::io::AtomicWriteStage stage) { return !(phase == "active" && stage == uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync); }));
	fs::path selected;
	UAM_ASSERT(!uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error));
	UAM_ASSERT(!fixture.transaction->RollbackBeforeActivation(fixture.error));
	fixture.ExpectHeldLocks();
}

UAM_TEST(DataFolderTransferChangedSourceCannotClaimRollback)
{
	TransferFixture fixture;
	UAM_ASSERT(!fixture.Activate([](std::string_view phase, uam::io::AtomicWriteStage) { return phase != "beforeActivation"; }));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "memory/knowledge.md", "unowned later publication"));
	UAM_ASSERT(!fixture.transaction->RollbackBeforeActivation(fixture.error));
	UAM_ASSERT_EQ(ReadFile(fixture.source / "memory/knowledge.md"), "unowned later publication");
	UAM_ASSERT_EQ(ReadFile(fixture.destination / "memory/knowledge.md"), "disposable memory survivor");
	fixture.ExpectHeldLocks();
}

UAM_TEST(DataFolderTransferRollbackAndLaterChoiceRetainPassiveOwners)
{
	TransferFixture fixture;
	UAM_ASSERT(!fixture.Activate([](std::string_view phase, uam::io::AtomicWriteStage) { return phase != "beforeActivation"; }));
	UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
	fixture.transaction.reset();
	fixture.source_lease.reset();
	fixture.control_lease.reset();
	fixture.control_lease = DataFolderTestProcess().TryAcquireDataRootLock(fixture.control, &fixture.error);
	std::shared_ptr<uam::platform::DataRootLock> owner = DataFolderTestProcess().TryAcquireDataRootLock(fixture.source, &fixture.error);
	fixture.transaction = uam::DataFolderTransition::Open(fixture.control, fixture.source, owner, fixture.control_lease, DataFolderTestProcess(), fixture.error);
	UAM_ASSERT(fixture.transaction && !fixture.transaction->Held());
	fixture.ExpectHeldLocks();
	const fs::path next = fs::canonical(fixture.temp.root) / "later-destination";
	fs::create_directory(next);
	UAM_ASSERT(fixture.transaction->Arm(next, "fixture/later-choice", fixture.error));
	UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
	fixture.ExpectHeldLocks();
	fixture.transaction.reset();
	owner.reset();
	owner = DataFolderTestProcess().TryAcquireDataRootLock(next, &fixture.error);
	fixture.transaction = uam::DataFolderTransition::Open(fixture.control, next, owner, fixture.control_lease, DataFolderTestProcess(), fixture.error);
	UAM_ASSERT(fixture.transaction && !fixture.transaction->Held() && fixture.transaction->CurrentRoot() == next);
	fixture.ExpectHeldLocks();
}

UAM_TEST(DataFolderTransferRequiresTheArmedDurablePreference)
{
	TransferFixture fixture;
	UAM_ASSERT(fixture.transaction->MatchesPendingChoice(fixture.destination, "fixture/revision"));
	UAM_ASSERT(!fixture.transaction->MatchesPendingChoice(fixture.destination, "different-revision"));
	UAM_ASSERT(!fixture.transaction->MatchesPendingChoice(fixture.source, "fixture/revision"));
	UAM_ASSERT(!fs::exists(fixture.destination / "chats"));
}

UAM_TEST(DataFolderTransferStreamsBinaryAuthorityAcrossChunks)
{
	TransferFixture fixture;
	std::string binary(1024 * 1024, '\0');
	for (std::size_t i = 0; i < binary.size(); ++i)
		binary[i] = static_cast<char>(i % 256);
	fs::create_directory(fixture.source / "native-media");
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "native-media/synthetic.bin", binary));
	UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
	UAM_ASSERT_EQ(ReadFile(fixture.source / "native-media/synthetic.bin"), binary);
	UAM_ASSERT_EQ(ReadFile(fixture.destination / "native-media/synthetic.bin"), binary);
}

UAM_TEST(DataFolderTransferStaleLockDoesNotPreventAConfirmedRetry)
{
	TransferFixture fixture;
	UAM_ASSERT(uam::io::WriteTextFile(fixture.destination / ".uam-data-root.lock", "12345\n"));
	UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
	UAM_ASSERT_EQ(ReadFile(fixture.destination / "memory/knowledge.md"), "disposable memory survivor");
	fixture.ExpectHeldLocks();
}

UAM_TEST(DataFolderTransferByteIdenticalSourceReplacementStillHolds)
{
	TransferFixture fixture;
	bool replaced = false;
	UAM_ASSERT(!fixture.Activate(
	    [&](std::string_view phase, uam::io::AtomicWriteStage)
	    {
		    if (phase == "beforeActivation")
		    {
			    replaced = true;
			    UAM_ASSERT(uam::io::AtomicWriteFile(fixture.source / "memory/knowledge.md", "disposable memory survivor"));
		    }
		    return true;
	    }));
	UAM_ASSERT(replaced && fixture.transaction->Held());
	UAM_ASSERT_EQ(ReadFile(fixture.source / "memory/knowledge.md"), ReadFile(fixture.destination / "memory/knowledge.md"));
	fixture.ExpectHeldLocks();
}

UAM_TEST(DataFolderTransferRetainedSourcePublicationMustRemainExact)
{
	TransferFixture fixture;
	UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
	fixture.transaction.reset();
	fixture.source_lease.reset();
	fixture.control_lease.reset();
	UAM_ASSERT(uam::io::AtomicWriteFile(fixture.source / "memory/knowledge.md", "disposable memory survivor"));
	fixture.control_lease = DataFolderTestProcess().TryAcquireDataRootLock(fixture.control, &fixture.error);
	std::shared_ptr<uam::platform::DataRootLock> current = DataFolderTestProcess().TryAcquireDataRootLock(fixture.destination, &fixture.error);
	fixture.transaction = uam::DataFolderTransition::Open(fixture.control, fixture.destination, current, fixture.control_lease, DataFolderTestProcess(), fixture.error);
	UAM_ASSERT(fixture.transaction && fixture.transaction->Held() && fixture.transaction->CurrentRoot() == fixture.destination);
	fixture.ExpectHeldLocks();
}

#if !defined(_WIN32)
UAM_TEST(DataFolderTransferLinkedBootstrapCannotRouteAProfile)
{
	TransferFixture fixture;
	UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
	const auto retained_control = fs::canonical(fixture.temp.root) / "retained-control";
	fs::rename(fixture.control, retained_control);
	fs::create_directory_symlink(retained_control, fixture.control);
	fs::path selected;
	UAM_ASSERT(!uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error));
	UAM_ASSERT(fs::exists(retained_control / "transition.json"));
	fixture.ExpectHeldLocks();
}
#endif

UAM_TEST(DataFolderTransferEmbeddedSettingsAliasesRequireTheirOwner)
{
	TransferFixture fixture;
	std::string alias = fixture.source.string();
	for (auto& byte : alias)
		if (byte >= 'a' && byte <= 'z')
			byte -= 'a' - 'A';
	const auto flags = nlohmann::json{{"home", alias + "/memory"}}.dump();
	const auto settings = "provider_extra_flags=" + uam::EncodeLineValue(flags) + "\n";
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "settings.txt", settings));
	UAM_ASSERT(!fixture.Activate());
	UAM_ASSERT_EQ(ReadFile(fixture.source / "settings.txt"), settings);
	UAM_ASSERT(!fs::exists(fixture.destination / "settings.txt"));
}

UAM_TEST(DataFolderTransferWorkspaceFolderReferenceRequiresItsOwner)
{
	TransferFixture fixture;
	const auto folder = "folders_format_version=1\n[folder]\nid=fixture\ntitle=Workspace\ndirectory=" + uam::EncodeLineValue((fixture.source / "memory").string()) + "\nexecution_host_id=local\nfolders_complete=1\n";
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "folders.txt", folder));
	UAM_ASSERT(!fixture.Activate());
	UAM_ASSERT_EQ(ReadFile(fixture.source / "folders.txt"), folder);
	UAM_ASSERT(!fs::exists(fixture.destination / "folders.txt"));
}

UAM_TEST(DataFolderTransferExternalWorkspaceStaysExternal)
{
	TransferFixture fixture;
	const auto workspace = fs::canonical(fixture.temp.root) / "workspace";
	fs::create_directory(workspace);
	UAM_ASSERT(uam::io::WriteTextFile(workspace / "outside.txt", "external survivor"));
	const auto folder = "folders_format_version=1\n[folder]\nid=fixture\ntitle=Workspace\ndirectory=" + uam::EncodeLineValue(workspace.string()) + "\nexecution_host_id=local\nfolders_complete=1\n";
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "folders.txt", folder));
	UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
	UAM_ASSERT_EQ(ReadFile(fixture.destination / "folders.txt"), folder);
	UAM_ASSERT_EQ(ReadFile(workspace / "outside.txt"), "external survivor");
	UAM_ASSERT(!fs::exists(fixture.destination / "outside.txt"));
}

UAM_TEST(DataFolderTransferRemoteWorkspaceReferenceIsNotLocalCopyAuthority)
{
	TransferFixture fixture;
	const auto folder = "folders_format_version=1\n[folder]\nid=fixture\ntitle=Remote\ndirectory=" + uam::EncodeLineValue(fixture.source.string()) + "\nexecution_host_id=fixture-remote\nfolders_complete=1\n";
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "folders.txt", folder));
	UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
	UAM_ASSERT_EQ(ReadFile(fixture.destination / "folders.txt"), folder);
}

UAM_TEST(DataFolderTransferImplicitWorkspaceCannotBindToRetainedSource)
{
	TransferFixture fixture;
	struct RestoreDirectory
	{
		fs::path original = fs::current_path();
		~RestoreDirectory()
		{
			fs::current_path(original);
		}
	} restore;
	fs::current_path(fixture.source);
	UAM_ASSERT(!fixture.Activate());
	UAM_ASSERT(!fs::exists(fixture.destination / "chats"));
	fixture.ExpectHeldLocks();
}

UAM_TEST(DataFolderTransferIndependentWriterRefusesBeforeCopy)
{
	TransferFixture fixture;
	using Writers = uam::platform::DataFolderWriterLease;
	const auto writer = Writers::TryAcquire(fixture.source, Writers::Access::Shared, fixture.error);
	UAM_ASSERT(writer && writer->Current());
	UAM_ASSERT(!fixture.Activate());
	UAM_ASSERT(fixture.transaction->Held() && fixture.transaction->CurrentRoot() == fixture.source);
	UAM_ASSERT(!fs::exists(fixture.destination / "chats"));
	UAM_ASSERT_EQ(ReadFile(fixture.source / "memory/knowledge.md"), "disposable memory survivor");
}

UAM_TEST(DataFolderTransferWriterFreezeFollowsAuthority)
{
	TransferFixture fixture;
	using Writers = uam::platform::DataFolderWriterLease;
	UAM_ASSERT(fixture.Activate());
	UAM_ASSERT(!Writers::TryAcquire(fixture.source, Writers::Access::Shared, fixture.error));
	UAM_ASSERT(!Writers::TryAcquire(fixture.destination, Writers::Access::Shared, fixture.error));
	UAM_ASSERT(fixture.transaction->ConfirmDestinationOwner(fixture.error));
	UAM_ASSERT(!Writers::TryAcquire(fixture.source, Writers::Access::Shared, fixture.error));
	UAM_ASSERT(Writers::TryAcquire(fixture.destination, Writers::Access::Shared, fixture.error));
	UAM_ASSERT(uam::DataFolderTransition::CheckExternalSourceFence(fixture.destination, fixture.error, true));
	UAM_ASSERT(!uam::DataFolderTransition::CheckExternalSourceFence(fixture.source, fixture.error, true));
}

UAM_TEST(DataFolderTransferRecoveredRollbackRetainsWriterFreeze)
{
	TransferFixture fixture;
	using Writers = uam::platform::DataFolderWriterLease;
	UAM_ASSERT(!fixture.Activate([](std::string_view phase, uam::io::AtomicWriteStage) { return phase != "beforeActivation"; }));
	fixture.transaction.reset();
	fixture.transaction = uam::DataFolderTransition::Open(fixture.control, fixture.source, fixture.source_lease, fixture.control_lease, DataFolderTestProcess(), fixture.error);
	UAM_ASSERT(fixture.transaction && fixture.transaction->Held());
	UAM_ASSERT(!Writers::TryAcquire(fixture.source, Writers::Access::Shared, fixture.error));
	UAM_ASSERT(!Writers::TryAcquire(fixture.destination, Writers::Access::Shared, fixture.error));
	UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
	UAM_ASSERT(Writers::TryAcquire(fixture.source, Writers::Access::Shared, fixture.error));
	UAM_ASSERT(!Writers::TryAcquire(fixture.destination, Writers::Access::Shared, fixture.error));
	fixture.transaction.reset();
	fixture.transaction = uam::DataFolderTransition::Open(fixture.control, fixture.source, fixture.source_lease, fixture.control_lease, DataFolderTestProcess(), fixture.error);
	UAM_ASSERT(fixture.transaction && !fixture.transaction->Held());
	UAM_ASSERT(!Writers::TryAcquire(fixture.destination, Writers::Access::Shared, fixture.error));
}

UAM_TEST(DataFolderWriterLeaseRejectsForeignMarkerAndReplacedName)
{
	TempDir temp("uam-data-folder-writer-marker");
	using Writers = uam::platform::DataFolderWriterLease;
	std::string error;
	const auto root = fs::canonical(temp.root);
	const auto marker = root / Writers::kFileName;
	UAM_ASSERT(uam::io::WriteTextFile(marker, "foreign survivor"));
	UAM_ASSERT(!Writers::TryAcquire(root, Writers::Access::Exclusive, error));
	UAM_ASSERT_EQ(ReadFile(marker), "foreign survivor");
	fs::remove(marker);
	auto writer = Writers::TryAcquire(root, Writers::Access::Shared, error);
	UAM_ASSERT(writer && writer->Current());
	UAM_ASSERT(Writers::TryAcquire(root, Writers::Access::Shared, error));
	UAM_ASSERT(!Writers::TryAcquire(root, Writers::Access::Exclusive, error));
#if !defined(_WIN32)
	fs::rename(marker, root / "retained-marker");
	UAM_ASSERT(uam::io::WriteTextFile(marker, ""));
	UAM_ASSERT(!writer->Current());
#endif
}

UAM_TEST(DataFolderTransferUnknownRootReceiptNeverAuthorizesRollback)
{
	for (const auto stage : {uam::io::AtomicWriteStage::BeforeReplace, uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync})
	{
		TransferFixture fixture;
		unsigned source_publications = 0;
		UAM_ASSERT(!fixture.Activate(
		    [&](std::string_view phase, uam::io::AtomicWriteStage observed)
		    {
			    // Let fenced/passive receipts commit, then interrupt the source prepared receipt.
			    if (phase == "source" && observed == uam::io::AtomicWriteStage::BeforeReplace)
				    ++source_publications;
			    return !(phase == "source" && source_publications == 2 && observed == stage);
		    }));
		UAM_ASSERT(source_publications == 2 && fixture.transaction->Held());
		UAM_ASSERT(!fixture.transaction->RollbackBeforeActivation(fixture.error));
		fixture.ExpectHeldLocks();
		fixture.transaction.reset();
		fixture.transaction = uam::DataFolderTransition::Open(fixture.control, fixture.source, fixture.source_lease, fixture.control_lease, DataFolderTestProcess(), fixture.error);
		UAM_ASSERT(fixture.transaction && fixture.transaction->Held());
		UAM_ASSERT(!fixture.transaction->RollbackBeforeActivation(fixture.error));
	}
}

UAM_TEST(DataFolderTransferPreparedRecoveryBackupRemainsAtSource)
{
	TransferFixture fixture;
	const auto backup = fixture.source / "chats/synthetic.json.bak";
	const std::string prepared = R"({"id":"synthetic","acp_queued_prompts":[{"id":"input","text":"retain intent","prepared_for_delivery":true}],"acp_dispatched_queued_prompt_count":0})";
	UAM_ASSERT(uam::io::WriteTextFile(backup, prepared));
	UAM_ASSERT(!fixture.Activate());
	UAM_ASSERT(fixture.transaction->Held());
	UAM_ASSERT_EQ(ReadFile(backup), prepared);
	UAM_ASSERT(!fs::exists(fixture.destination / "chats"));
}

UAM_TEST(DataFolderTransferForeignBootstrapDataCannotBeExcluded)
{
	TransferFixture fixture;
	UAM_ASSERT(uam::io::WriteTextFile(fixture.control / "foreign-note.txt", "retain foreign survivor"));
	UAM_ASSERT(!fixture.Activate());
	UAM_ASSERT(fixture.transaction->Held());
	UAM_ASSERT_EQ(ReadFile(fixture.control / "foreign-note.txt"), "retain foreign survivor");
	UAM_ASSERT(!fs::exists(fixture.destination / "chats"));
}

UAM_TEST(DataFolderTransferOpaqueJsonCommandCannotRetainSourceBinding)
{
	TransferFixture fixture;
	const auto record = nlohmann::json{{"command", "inspect --store \"" + fixture.source.string() + "/configuration\""}}.dump();
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "resource_collections.json", record));
	UAM_ASSERT(!fixture.Activate());
	UAM_ASSERT(fixture.transaction->Held());
	UAM_ASSERT_EQ(ReadFile(fixture.source / "resource_collections.json"), record);
	UAM_ASSERT(!fs::exists(fixture.destination / "chats"));
}

UAM_TEST(DataFolderTransferAbsoluteExecutableCannotRetainSourceBinding)
{
	for (bool settings : {false, true})
	{
		TransferFixture fixture;
		const auto command = "/usr/bin/tool --store \"" + fixture.source.string() + "/configuration\"";
		const auto relative = settings ? "settings.txt" : "resource_collections.json";
		const auto bytes = settings ? "unsupported_command=" + uam::EncodeLineValue(command) + "\n" : nlohmann::json{{"command", command}}.dump();
		UAM_ASSERT(uam::io::WriteTextFile(fixture.source / relative, bytes));
		const auto source = FrozenInventory(fixture.source);
		UAM_ASSERT(!fixture.Activate());
		UAM_ASSERT(fixture.transaction->Held());
		UAM_ASSERT_EQ(FrozenInventory(fixture.source), source);
		UAM_ASSERT(!fs::exists(fixture.destination / "chats"));
	}
}

UAM_TEST(DataFolderTransferUnknownFinalPublicationRefusesIndependentWriters)
{
	for (bool rollback : {false, true})
	{
		TransferFixture fixture;
		const auto fault = [](std::string_view phase, uam::io::AtomicWriteStage stage) { return !((phase == "active" || phase == "rolledBack") && stage == uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync); };
		if (rollback)
		{
			UAM_ASSERT(!fixture.Activate([](std::string_view phase, uam::io::AtomicWriteStage) { return phase != "beforeActivation"; }));
			UAM_ASSERT(!fixture.transaction->RollbackBeforeActivation(fixture.error, fault));
		}
		else
		{
			UAM_ASSERT(fixture.Activate());
			UAM_ASSERT(!fixture.transaction->ConfirmDestinationOwner(fixture.error, fault));
		}
		const auto source = FrozenInventory(fixture.source), destination = FrozenInventory(fixture.destination);
		fixture.transaction.reset();
		fixture.source_lease.reset();
		fixture.control_lease.reset();
		const auto root = rollback ? fixture.source : fixture.destination;
		auto writer = uam::platform::DataFolderWriterLease::TryAcquire(root, uam::platform::DataFolderWriterLease::Access::Shared, fixture.error);
		UAM_ASSERT(writer); // The failed desktop owner has exited; the writer freeze alone is insufficient.
		UAM_ASSERT(!uam::DataFolderTransition::CheckExternalSourceFence(root, fixture.error, true));
		fs::path selected;
		UAM_ASSERT(!uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error));
		UAM_ASSERT_EQ(FrozenInventory(fixture.source), source);
		UAM_ASSERT_EQ(FrozenInventory(fixture.destination), destination);
	}
}

UAM_TEST(DataFolderTransferDestinationMarkerRaceCannotMutateSource)
{
	for (bool hard_link : {false, true})
	{
#if defined(_WIN32)
		if (!hard_link) continue; // Windows symbolic-link privileges require separate platform verification.
#endif
		TransferFixture fixture;
		const auto source = FrozenInventory(fixture.source), control = ControlInventory(fixture.control);
		bool injected = false;
		UAM_ASSERT(!fixture.Activate([&](std::string_view phase, uam::io::AtomicWriteStage)
		{
			if (phase == "beforeDestinationLock")
			{
				injected = true;
				const auto marker = fixture.destination / ".uam-data-root.lock";
				if (hard_link) fs::create_hard_link(fixture.source / "memory/knowledge.md", marker);
				else fs::create_symlink(fixture.source / "memory/knowledge.md", marker);
			}
			return true;
		}));
		UAM_ASSERT(injected && fixture.transaction->Held());
		UAM_ASSERT_EQ(ReadFile(fixture.source / "memory/knowledge.md"), "disposable memory survivor");
		if (hard_link)
		{
			// The fixture attacker changed source link-count/ctime. Remove only its own
			// disposable link; source bytes and logical inventory must still be intact.
			UAM_ASSERT(fs::remove(fixture.destination / ".uam-data-root.lock"));
			UAM_ASSERT(nlohmann::json::parse(FrozenInventory(fixture.source))["logical"] == nlohmann::json::parse(source)["logical"]);
		}
		else UAM_ASSERT_EQ(FrozenInventory(fixture.source), source);
		UAM_ASSERT_EQ(ControlInventory(fixture.control), control);
		UAM_ASSERT(!fs::exists(fixture.destination / "chats"));
	}
}

UAM_TEST(DataFolderRootLockIsNonDestructiveAndNameBound)
{
	TempDir temp("uam-root-lock-identity");
	const auto root = fs::canonical(temp.root);
	const auto marker = root / ".uam-data-root.lock";
	UAM_ASSERT(uam::io::WriteTextFile(marker, "12345\n"));
	std::string error;
	auto lease = DataFolderTestProcess().TryAcquireDataRootLock(root, &error);
	UAM_ASSERT(lease && lease->Current());
#if !defined(_WIN32)
	UAM_ASSERT_EQ(ReadFile(marker), "12345\n"); // Historical PID text is neither overwritten nor authority.
#endif
	UAM_ASSERT(!DataFolderTestProcess().TryAcquireDataRootLock(root, &error));
	std::error_code ec;
	fs::rename(marker, root / "retained-marker", ec);
#if defined(_WIN32)
	UAM_ASSERT(ec && lease->Current()); // Exclusive Windows ownership denies delete/rename sharing.
#else
	UAM_ASSERT(!ec);
	UAM_ASSERT(uam::io::WriteTextFile(marker, ""));
	UAM_ASSERT(!lease->Current());
#endif
	lease.reset();
#if defined(_WIN32)
	UAM_ASSERT_EQ(ReadFile(marker), "12345\n"); // The exclusive lease blocks reads until released; the refused rename kept the text.
#endif
}

UAM_TEST(DataFolderInventoryRefusesOversizedDirectoryDuringListing)
{
	TempDir temp("uam-inventory-entry-limit");
	const auto root = fs::canonical(temp.root);
	for (unsigned i = 0; i <= 100000; ++i)
	{
		std::ofstream file(root / ("entry-" + std::to_string(i)), std::ios::binary);
		UAM_ASSERT(file.good());
	}
	std::string error;
	const auto tree = uam::platform::DataFolderTree::Open(root, error);
	uam::platform::DataFolderTree::Inventory entries;
	UAM_ASSERT(tree && !tree->Scan(entries, error));
	// Refusal occurs while listing, before sorting or reading/hash-opening any file.
	UAM_ASSERT(entries.empty());
}

UAM_TEST(DataFolderRootLockRejectsForeignReservedBytesWithoutMutation)
{
	TempDir temp("uam-root-lock-foreign");
	const auto root = fs::canonical(temp.root), marker = root / ".uam-data-root.lock";
	UAM_ASSERT(uam::io::WriteTextFile(marker, "foreign small survivor"));
	std::string error;
	UAM_ASSERT(!DataFolderTestProcess().TryAcquireDataRootLock(root, &error));
	UAM_ASSERT_EQ(ReadFile(marker), "foreign small survivor");
}

#if !defined(_WIN32)
UAM_TEST(DataFolderTransferRevokedRootLockCannotAcknowledgePublication)
{
	TransferFixture fixture;
	const auto source = FrozenInventory(fixture.source);
	UAM_ASSERT(!fixture.Activate([&](std::string_view phase, uam::io::AtomicWriteStage stage)
	{
		if (phase == "staging" && stage == uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync)
		{
			fs::rename(fixture.source / ".uam-data-root.lock", fixture.temp.root / "retained-source-marker");
			UAM_ASSERT(uam::io::WriteTextFile(fixture.source / ".uam-data-root.lock", ""));
		}
		return true;
	}));
	UAM_ASSERT(fixture.transaction->Held());
	const auto before = nlohmann::json::parse(source), after = nlohmann::json::parse(FrozenInventory(fixture.source));
	UAM_ASSERT(after["logical"] == before["logical"] && after["objects"] == before["objects"]);
	// Staging is now preceded by a durable source fence. Revocation may retain
	// that fence, but cannot acknowledge staging or permit an independent writer.
	const auto receipt = nlohmann::json::parse(ReadFile(fixture.source / ".uam-data-folder/receipt.json"));
	UAM_ASSERT(receipt["phase"] == "fenced");
	UAM_ASSERT(!uam::DataFolderTransition::CheckExternalSourceFence(fixture.source, fixture.error, true));
	UAM_ASSERT(!fs::exists(fixture.destination / "chats"));
	fs::path selected;
	UAM_ASSERT(!uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error));
}
#endif

UAM_TEST(DataFolderTransferEarlyFenceFailureCannotReopenOrCancel)
{
	for (bool followup : {false, true})
		for (const auto role : {"source", "destination"})
			for (const auto failure : {uam::io::AtomicWriteStage::BeforeReplace, uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync})
			{
				TransferFixture fixture;
				fs::path current = fixture.source;
				if (followup)
				{
					UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
					current = fixture.destination;
					const auto next = fs::canonical(fixture.temp.root) / "early-fence-followup";
					fs::create_directory(next);
					UAM_ASSERT(fixture.transaction->Arm(next, "early/fence", fixture.error));
				}
				const auto before = nlohmann::json::parse(FrozenInventory(current));
				const auto control = ControlInventory(fixture.control);
				UAM_ASSERT(!fixture.Activate([&](std::string_view phase, uam::io::AtomicWriteStage stage) { return phase != role || stage != failure; }));
				const auto after = nlohmann::json::parse(FrozenInventory(current));
				UAM_ASSERT(after["logical"] == before["logical"] && after["objects"] == before["objects"]);
				UAM_ASSERT_EQ(ControlInventory(fixture.control), control); // No staging publication was attempted.
				fixture.transaction.reset();
				fixture.source_lease.reset();
				fixture.control_lease.reset();
				fs::path selected;
				UAM_ASSERT(!uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error));
				fixture.control_lease = DataFolderTestProcess().TryAcquireDataRootLock(fixture.control, &fixture.error);
				std::shared_ptr<uam::platform::DataRootLock> owner = DataFolderTestProcess().TryAcquireDataRootLock(current, &fixture.error);
				fixture.transaction = uam::DataFolderTransition::Open(fixture.control, current, owner, fixture.control_lease, DataFolderTestProcess(), fixture.error);
				UAM_ASSERT(fixture.transaction && fixture.transaction->Held());
				UAM_ASSERT(!fixture.transaction->RollbackBeforeActivation(fixture.error));
				UAM_ASSERT(!uam::DataFolderTransition::CheckExternalSourceFence(current, fixture.error, true));
				UAM_ASSERT_EQ(ControlInventory(fixture.control), control);
			}
}

UAM_TEST(DataFolderTransferUnknownArmedCancellationHoldsWriterAuthority)
{
	for (bool restored : {false, true})
		for (const auto phase_to_fail : {"source", "rolledBack"})
			for (const auto failure : {uam::io::AtomicWriteStage::BeforeReplace, uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync})
			{
				TransferFixture fixture;
				if (restored)
				{
					UAM_ASSERT(!fixture.Activate([](std::string_view phase, uam::io::AtomicWriteStage) { return phase != "beforeActivation"; }));
					UAM_ASSERT(fixture.transaction->RollbackBeforeActivation(fixture.error));
				}
				else UAM_ASSERT(fixture.Activate() && fixture.transaction->ConfirmDestinationOwner(fixture.error));
				const auto current = restored ? fixture.source : fixture.destination, historical = restored ? fixture.destination : fixture.source;
				const auto before = nlohmann::json::parse(FrozenInventory(current));
				const auto retained = FrozenInventory(historical);
				const auto next = fs::canonical(fixture.temp.root) / "uncertain-cancel";
				fs::create_directory(next);
				UAM_ASSERT(fixture.transaction->Arm(next, "uncertain/cancel", fixture.error));
				UAM_ASSERT(!fixture.transaction->RollbackBeforeActivation(fixture.error, [&](std::string_view phase, uam::io::AtomicWriteStage stage) { return phase != phase_to_fail || stage != failure; }));
				const auto source = FrozenInventory(current), control = ControlInventory(fixture.control);
				fixture.transaction.reset();
				fixture.source_lease.reset();
				fixture.control_lease.reset();
				auto writer = uam::platform::DataFolderWriterLease::TryAcquire(current, uam::platform::DataFolderWriterLease::Access::Shared, fixture.error);
				UAM_ASSERT(writer && !uam::DataFolderTransition::CheckExternalSourceFence(current, fixture.error, true));
				fs::path selected;
				UAM_ASSERT(!uam::DataFolderTransition::ProbeStartupRoot(fixture.source, {}, selected, fixture.error));
				const auto after = nlohmann::json::parse(source);
				UAM_ASSERT(after["logical"] == before["logical"] && after["objects"] == before["objects"]);
				UAM_ASSERT_EQ(FrozenInventory(current), source);
				UAM_ASSERT_EQ(FrozenInventory(historical), retained);
				UAM_ASSERT_EQ(ControlInventory(fixture.control), control);
			}
}

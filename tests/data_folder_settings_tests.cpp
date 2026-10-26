#include "data_folder_test_support.h"
#include "app/data_folder_settings_service.h"
#include "common/platform/retained_directory_identity.h"

using namespace uam_test;

namespace
{
	struct DataFolderFixture
	{
		TempDir temp{"uam-data-folder-settings"};
		fs::path source = temp.root / "source";
		fs::path destination = temp.root / "destination";
		std::shared_ptr<uam::DataFolderSettingsService> service;
		std::shared_ptr<uam::platform::DataRootLock> source_lease;

		DataFolderFixture()
		{
			fs::create_directories(destination);
			Reopen();
		}
		void Reopen()
		{
			service.reset();
			source_lease.reset();
			std::string error;
			service = uam::DataFolderSettingsService::Open(source, {}, DataFolderTestProcess(), error);
			UAM_ASSERT(service != nullptr);
			source_lease = DataFolderTestProcess().TryAcquireDataRootLock(source, &error);
			UAM_ASSERT(source_lease != nullptr);
			service->BindSourceLease(source_lease);
		}
	};
} // namespace

UAM_TEST(DataFolderSettingsPersistsPendingChoiceWithoutSwitchingProfiles)
{
	DataFolderFixture fixture;
	const std::vector<fs::path> records = {"chats/chat.json", "chat-summaries/chat.json", "queue-outboxes/chat.json", "memory/knowledge.md", "agent-runs/run.json", "recovery/receipt.json", "settings.txt"};
	for (const fs::path& relative : records)
	{
		fs::create_directories((fixture.source / relative).parent_path());
		UAM_ASSERT(uam::io::WriteTextFile(fixture.source / relative, "disposable authority survivor"));
	}
	const auto initial = fixture.service->Read();
	UAM_ASSERT(initial.can_change && !initial.external_override && initial.pending_folder.empty());
	const auto saved = fixture.service->Choose(fixture.destination, initial.revision);
	UAM_ASSERT(saved.committed && saved.settings.state == "awaitingMigration");
	UAM_ASSERT(saved.settings.pending_folder == fs::canonical(fixture.destination));
	UAM_ASSERT(fixture.service->StartupRoot() == fixture.source && fs::is_empty(fixture.destination));
	fixture.Reopen();
	const auto restarted = fixture.service->Read();
	UAM_ASSERT(restarted.state == "awaitingMigration" && restarted.pending_folder == fs::canonical(fixture.destination));
	UAM_ASSERT(restarted.current_folder == fixture.source);
	UAM_ASSERT(!fixture.service->Cancel(saved.settings.revision).committed); // Prior app-instance token is stale.
	UAM_ASSERT(fixture.service->Cancel(restarted.revision).committed);
	fixture.Reopen();
	UAM_ASSERT(fixture.service->Read().pending_folder.empty());
	for (const fs::path& relative : records)
	{
		UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.source / relative), "disposable authority survivor");
	}
	UAM_ASSERT(fs::is_empty(fixture.destination));
}

UAM_TEST(DataFolderSettingsExternalOverrideBypassesDefaultPreferenceAndLocks)
{
	TempDir temp("uam-data-folder-override");
	const fs::path default_root = temp.root / "not-a-directory";
	const fs::path override_root = temp.root / "explicit-profile";
	UAM_ASSERT(uam::io::WriteTextFile(default_root, "default path must not be opened"));
	std::string error;
	const auto service = uam::DataFolderSettingsService::Open(default_root, override_root, DataFolderTestProcess(), error);
	UAM_ASSERT(service != nullptr && error.empty());
	UAM_ASSERT(service->StartupRoot() == override_root && service->Read().external_override);
	UAM_ASSERT(!service->Read().can_change);
	UAM_ASSERT(!service->Choose(temp.root / "destination", service->Read().revision).committed);
	UAM_ASSERT(!fs::exists(override_root));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(default_root), "default path must not be opened");
}

UAM_TEST(DataFolderSettingsRequiresControlAndSourceOwnership)
{
	DataFolderFixture fixture;
	std::string error;
	UAM_ASSERT(!uam::DataFolderSettingsService::Open(fixture.source, {}, DataFolderTestProcess(), error));
	const auto token = fixture.service->Read().revision;
	fixture.source_lease.reset();
	fixture.service->BindSourceLease({});
	UAM_ASSERT(!fixture.service->Read().can_change);
	UAM_ASSERT(!fixture.service->Choose(fixture.destination, token).committed);
	UAM_ASSERT(!fs::exists(fixture.source / ".data-folder-control/selection.json"));
	UAM_ASSERT(fs::is_empty(fixture.destination));
}

UAM_TEST(DataFolderSettingsInterruptedPublicationRetainsSourceAndHoldAcrossRestart)
{
	DataFolderFixture fixture;
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "queue-journal-survivor", "do not replay or remove"));
	const auto failed = fixture.service->Choose(fixture.destination, fixture.service->Read().revision, "interrupted-choice", [](uam::io::AtomicWriteStage stage) { return stage != uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync; });
	UAM_ASSERT(!failed.committed && failed.settings.state == "publicationUnknown" && !failed.settings.can_change);
	UAM_ASSERT(fs::exists(fixture.source / ".data-folder-control/intent.json"));
	UAM_ASSERT(!fixture.service->Cancel(failed.settings.revision).committed);
	fixture.Reopen();
	UAM_ASSERT(fixture.service->Read().state == "publicationUnknown" && !fixture.service->Read().can_change);
	UAM_ASSERT(fixture.service->StartupRoot() == fixture.source && fs::is_empty(fixture.destination));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(fixture.source / "queue-journal-survivor"), "do not replay or remove");
}

UAM_TEST(DataFolderSettingsStaleRequestsAndForeignPrimaryCannotPublish)
{
	DataFolderFixture fixture;
	const auto initial = fixture.service->Read();
	UAM_ASSERT(!fixture.service->Choose(fixture.destination, "foreign-scope").committed);
	const auto saved = fixture.service->Choose(fixture.destination, initial.revision);
	UAM_ASSERT(saved.committed);
	UAM_ASSERT(!fixture.service->Cancel(initial.revision).committed);
	const fs::path primary = fixture.source / ".data-folder-control/selection.json";
	UAM_ASSERT(uam::io::WriteTextFile(primary, "foreign replacement"));
	const auto rejected = fixture.service->Cancel(saved.settings.revision);
	UAM_ASSERT(!rejected.committed && rejected.settings.state == "publicationUnknown");
	UAM_ASSERT_EQ(uam::io::ReadTextFile(primary), "foreign replacement");
	UAM_ASSERT(fs::is_empty(fixture.destination));
}

UAM_TEST(DataFolderSettingsPrePublicationFailureRetainsIntentWithoutCreatingDestinationData)
{
	DataFolderFixture fixture;
	const auto failed = fixture.service->Choose(fixture.destination, fixture.service->Read().revision, "before-replace", [](uam::io::AtomicWriteStage stage) { return stage != uam::io::AtomicWriteStage::BeforeReplace; });
	UAM_ASSERT(!failed.committed && failed.settings.state == "publicationUnknown");
	UAM_ASSERT(!fs::exists(fixture.source / ".data-folder-control/selection.json"));
	fixture.Reopen();
	UAM_ASSERT(fixture.service->Read().state == "publicationUnknown");
	UAM_ASSERT(fixture.service->Read().pending_folder == fs::canonical(fixture.destination));
	UAM_ASSERT(fixture.service->StartupRoot() == fixture.source && fs::is_empty(fixture.destination));
}

UAM_TEST(DataFolderSettingsMalformedAndActivationRecordsFailClosed)
{
	TempDir temp("uam-data-folder-invalid-record");
	const fs::path source = temp.root / "source";
	fs::create_directories(source / ".data-folder-control");
	const fs::path primary = source / ".data-folder-control/selection.json";
	std::string error;
	UAM_ASSERT(uam::io::WriteTextFile(primary, "invalid"));
	UAM_ASSERT(!uam::DataFolderSettingsService::Open(source, {}, DataFolderTestProcess(), error));
	const fs::path other = temp.root / "other-profile";
	fs::create_directory(other);
	const nlohmann::json unsupported = {{"version", 1}, {"source", uam::paths::Utf8PathString(other)}, {"destination", ""}, {"revision", "foreign"}, {"operationId", "foreign"}, {"phase", "stable"}};
	UAM_ASSERT(uam::io::WriteTextFile(primary, unsupported.dump()));
	UAM_ASSERT(!uam::DataFolderSettingsService::Open(source, {}, DataFolderTestProcess(), error));
	UAM_ASSERT(fs::is_empty(other));
}

UAM_TEST(DataFolderSettingsMissingAcknowledgementKeepsPendingChoiceHeld)
{
	DataFolderFixture fixture;
	UAM_ASSERT(fixture.service->Choose(fixture.destination, fixture.service->Read().revision).committed);
	fs::remove(fixture.source / ".data-folder-control/intent.json");
	fixture.Reopen();
	const auto state = fixture.service->Read();
	UAM_ASSERT(state.state == "publicationUnknown" && !state.can_change);
	UAM_ASSERT(state.pending_folder == fs::canonical(fixture.destination));
	UAM_ASSERT(!fixture.service->Cancel(state.revision).committed);
	UAM_ASSERT(fixture.service->StartupRoot() == fixture.source && fs::is_empty(fixture.destination));
}

UAM_TEST(DataFolderDirectoryIdentityAllowsContentsButRejectsReplacement)
{
	TempDir temp("uam-data-folder-directory-identity");
	const fs::path directory = temp.root / "directory";
	fs::create_directory(directory);
	const auto identity = uam::platform::RetainedDirectoryIdentity::Capture(directory);
	UAM_ASSERT(identity != nullptr && identity->Current());
	UAM_ASSERT(uam::io::WriteTextFile(directory / "entry", "disposable"));
	UAM_ASSERT(identity->Current());
	fs::rename(directory, temp.root / "retained");
	fs::create_directory(directory);
	UAM_ASSERT(!identity->Current());
	UAM_ASSERT(fs::is_empty(directory));
}

UAM_TEST(DataFolderSettingsStartupCapturesOverrideBeforeEffectiveExport)
{
	const fs::path root = fs::path(__FILE__).parent_path().parent_path();
	const auto source = ReadFile(root / "src/app/application.cpp");
	const auto initialize = source.find("bool Application::InitializeState()");
	const auto original = source.find("GetTrimmedPath(\"UAM_DATA_DIR\")", initialize);
	const auto effective = source.find("SetString(\"UAM_DATA_DIR\"", original);
	const auto captured = source.find("DataFolderSettingsService::Open(", original);
	UAM_ASSERT(initialize != std::string::npos && original != std::string::npos && captured != std::string::npos && effective != std::string::npos);
	UAM_ASSERT(original < effective);
	UAM_ASSERT(source.find("default_root, data_root_env,", captured) != std::string::npos);
	// Existing second-instance shell actions must queue and reach the data-root-lock gate before bootstrap locking.
	const auto forwarded = source.find("ShellActionService::QueueLaunchRequest(", initialize);
	const auto root_lock = source.find("m_dataRootLock =", forwarded);
	UAM_ASSERT(forwarded != std::string::npos && root_lock != std::string::npos && forwarded < root_lock && root_lock < captured);
}

#if !defined(_WIN32)
UAM_TEST(DataFolderSettingsReplacementCannotInheritControlOwnership)
{
	DataFolderFixture fixture;
	const auto revision = fixture.service->Read().revision;
	const fs::path old = fixture.temp.root / "retained-source";
	fs::rename(fixture.source, old);
	fs::create_directory(fixture.source);
	const auto result = fixture.service->Choose(fixture.destination, revision);
	UAM_ASSERT(!result.committed && result.settings.state == "publicationUnknown");
	UAM_ASSERT(fs::is_empty(fixture.source) && fs::is_empty(fixture.destination));
	UAM_ASSERT(!fs::exists(old / ".data-folder-control/selection.json"));
}
#endif

UAM_TEST(DataFolderSettingsTransferRecoveryCannotClearUnknownPreference)
{
	DataFolderFixture fixture;
	const auto result = fixture.service->Choose(fixture.destination, fixture.service->Read().revision, "unconfirmed-choice", [](uam::io::AtomicWriteStage stage) { return stage != uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync; });
	UAM_ASSERT(!result.committed && result.settings.state == "publicationUnknown");
	UAM_ASSERT(!fixture.service->RollbackPendingTransfer(result.settings.revision).committed);
	UAM_ASSERT(fixture.service->Read().state == "publicationUnknown");
	UAM_ASSERT(fs::is_empty(fixture.destination));
}

UAM_TEST(DataFolderSettingsRelativeDefaultCanStageExplicitChoice)
{
	TempDir temp("uam-data-folder-relative-default");
	struct RestoreDirectory
	{
		fs::path original = fs::current_path();
		~RestoreDirectory()
		{
			fs::current_path(original);
		}
	} restore;
	fs::current_path(fs::canonical(temp.root));
	std::string error;
	auto owner = DataFolderTestProcess().TryAcquireDataRootLock("data", &error);
	auto service = uam::DataFolderSettingsService::Open("data", {}, DataFolderTestProcess(), error);
	UAM_ASSERT(owner && service && service->AttachStartupOwner(std::shared_ptr<uam::platform::DataRootLock>(owner.release()), false, error));
	UAM_ASSERT(service->StartupRoot() == fs::path("data"));
	const auto destination = fs::canonical(temp.root) / "chosen";
	fs::create_directory(destination);
	UAM_ASSERT(service->Choose(destination, service->Read().revision, "relative-default-choice").committed);
	UAM_ASSERT(service->StartupRoot() == fs::path("data"));
	UAM_ASSERT(service->Read().pending_folder == destination);
}

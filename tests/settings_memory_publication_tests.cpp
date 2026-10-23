#include "test_harness.h"
#include "app/persistence_coordinator.h"
#include "common/config/settings_store.h"
#include <stdexcept>

namespace
{
	using Stage = uam::io::AtomicWriteStage;
	using Outcome = SettingsMemoryWriteOutcome;
	struct PublicationFixture
	{
		uam_test::TempDir temp{"uam-settings-publication"};
		uam::AppState app;
		PersistenceCoordinator coordinator;
		PublicationFixture()
		{
			app.data_root = temp.root;
			app.settings.memory_idle_delay_seconds = 60;
			UAM_ASSERT(coordinator.SaveSettings(app));
		}
		std::filesystem::path Path() const { return temp.root / "settings.txt"; }
		AppSettings Primary() const
		{
			AppSettings value;
			const auto result = SettingsStore::LoadPrimary(Path(), value);
			UAM_ASSERT(result.loaded && !result.recovered_from_backup && !result.unrecovered_error);
			return value;
		}
		void Unknown(bool throws)
		{
			app.settings.memory_idle_delay_seconds = 177;
			int visits = 0;
			bool threw = false;
			try
			{
				UAM_ASSERT(!coordinator.SaveSettings(app, nullptr, [&](Stage stage)
				{
					if (stage != Stage::AfterReplaceBeforeDirectorySync) return true;
					++visits;
					if (throws) throw std::runtime_error("observed private stage fault");
					return false;
				}));
			}
			catch (const std::runtime_error& error)
			{
				threw = true;
				UAM_ASSERT_EQ(std::string(error.what()), std::string("observed private stage fault"));
			}
			UAM_ASSERT_EQ(visits, 1);
			UAM_ASSERT_EQ(threw, throws);
			UAM_ASSERT(app.memory_settings_uncertain);
			UAM_ASSERT_EQ(Primary().memory_idle_delay_seconds, 177);
		}
	};
}

UAM_TEST(SettingsPublicationScopeIsLazyDistinctAndStable)
{
	uam::AppState first, second;
	PersistenceCoordinator coordinator;
	UAM_ASSERT(first.settings_publication_scope.empty() && first.settings_publication_revision == 0);
	UAM_ASSERT(coordinator.EnsureSettingsPublicationScope(first));
	const auto scope = first.settings_publication_scope;
	UAM_ASSERT(!scope.empty() && coordinator.EnsureSettingsPublicationScope(first));
	UAM_ASSERT_EQ(first.settings_publication_scope, scope);
	UAM_ASSERT_EQ(first.settings_publication_revision, std::uint64_t{0});
	UAM_ASSERT(coordinator.EnsureSettingsPublicationScope(second));
	UAM_ASSERT(second.settings_publication_scope != scope);
}

UAM_TEST(SettingsPublicationEverySaveLoadAttemptRetiresPriorRevision)
{
	PublicationFixture fixture;
	const auto scope = fixture.app.settings_publication_scope;
	std::uint64_t revision = fixture.app.settings_publication_revision;
	SettingsMemoryWriteReceipt receipt;
	UAM_ASSERT(!fixture.coordinator.SaveSettings(fixture.app, &receipt, [](Stage stage) { return stage != Stage::BeforeReplace; }));
	UAM_ASSERT_EQ(receipt.outcome, Outcome::RejectedBeforeReplace);
	UAM_ASSERT_EQ(fixture.app.settings_publication_revision, ++revision);
	UAM_ASSERT(fixture.coordinator.LoadSettings(fixture.app));
	UAM_ASSERT_EQ(fixture.app.settings_publication_revision, ++revision);
	UAM_ASSERT(uam::io::WriteTextFile(fixture.Path(), "invalid settings"));
	UAM_ASSERT(uam::io::WriteTextFile(uam::io::MakeBackupPath(fixture.Path()), "invalid backup"));
	UAM_ASSERT(!fixture.coordinator.LoadSettings(fixture.app));
	UAM_ASSERT_EQ(fixture.app.settings_publication_revision, ++revision);
	UAM_ASSERT_EQ(fixture.app.settings_publication_scope, scope);
}

UAM_TEST(SettingsPublicationExhaustionRefusesBeforeWriteOrLoad)
{
	PublicationFixture fixture;
	const auto bytes = uam_test::ReadFile(fixture.Path());
	fixture.app.settings_publication_revision = PersistenceCoordinator::kMaxSettingsPublicationRevision;
	fixture.app.settings.memory_idle_delay_seconds = 199;
	UAM_ASSERT(!fixture.coordinator.SaveSettings(fixture.app));
	UAM_ASSERT(!fixture.coordinator.LoadSettings(fixture.app));
	UAM_ASSERT_EQ(fixture.app.settings_publication_revision, PersistenceCoordinator::kMaxSettingsPublicationRevision);
	UAM_ASSERT_EQ(fixture.app.settings.memory_idle_delay_seconds, 199);
	UAM_ASSERT_EQ(uam_test::ReadFile(fixture.Path()), bytes);
}

UAM_TEST(SettingsPublicationGenericUnknownFalseAndThrowRetireAndLatch)
{
	for (const bool throws : {false, true})
	{
		PublicationFixture fixture;
		const auto revision = fixture.app.settings_publication_revision;
		fixture.Unknown(throws);
		UAM_ASSERT_EQ(fixture.app.settings_publication_revision, revision + 1);
	}
}

UAM_TEST(SettingsPublicationUnknownRecoveryPreservesUnrelatedSaveIntent)
{
	PublicationFixture fixture;
	fixture.Unknown(false);
	fixture.app.settings.memory_idle_delay_seconds = 60; // Simulated old speculative rollback.
	fixture.app.settings.provider_extra_flags = "--private-unrelated-setting";
	const auto revision = fixture.app.settings_publication_revision;
	UAM_ASSERT(fixture.coordinator.SaveSettings(fixture.app));
	UAM_ASSERT(!fixture.app.memory_settings_uncertain);
	UAM_ASSERT_EQ(fixture.app.settings_publication_revision, revision + 1);
	UAM_ASSERT_EQ(fixture.app.settings.memory_idle_delay_seconds, 177);
	UAM_ASSERT_EQ(fixture.app.settings.provider_extra_flags, std::string("--private-unrelated-setting"));
	UAM_ASSERT_EQ(fixture.Primary().memory_idle_delay_seconds, 177);
	UAM_ASSERT_EQ(fixture.Primary().provider_extra_flags, std::string("--private-unrelated-setting"));
}

UAM_TEST(SettingsPublicationUnknownMissingOrInvalidPrimaryNeverUsesBackupToWrite)
{
	for (const bool missing : {false, true})
	{
		PublicationFixture fixture;
		fixture.Unknown(false);
		UAM_ASSERT(SettingsStore::Save(uam::io::MakeBackupPath(fixture.Path()), AppSettings{}));
		if (missing) std::filesystem::remove(fixture.Path());
		else UAM_ASSERT(uam::io::WriteTextFile(fixture.Path(), "invalid primary"));
		const auto backup = uam_test::ReadFile(uam::io::MakeBackupPath(fixture.Path()));
		fixture.app.settings.memory_idle_delay_seconds = 99;
		fixture.app.settings.provider_extra_flags = "--unsaved-unrelated";
		const auto revision = fixture.app.settings_publication_revision;
		UAM_ASSERT(!fixture.coordinator.SaveSettings(fixture.app));
		UAM_ASSERT(fixture.app.memory_settings_uncertain);
		UAM_ASSERT_EQ(fixture.app.settings_publication_revision, revision + 1);
		UAM_ASSERT_EQ(fixture.app.settings.memory_idle_delay_seconds, 99);
		UAM_ASSERT_EQ(fixture.app.settings.provider_extra_flags, std::string("--unsaved-unrelated"));
		UAM_ASSERT_EQ(uam_test::ReadFile(uam::io::MakeBackupPath(fixture.Path())), backup);
		UAM_ASSERT_EQ(std::filesystem::exists(fixture.Path()), !missing);
		if (!missing) UAM_ASSERT_EQ(uam_test::ReadFile(fixture.Path()), std::string("invalid primary"));
	}
}

UAM_TEST(SettingsPublicationResolveBeforeNewMemoryIntentDoesNotEraseRequest)
{
	PublicationFixture fixture;
	fixture.Unknown(true);
	fixture.app.settings.memory_idle_delay_seconds = 60;
	const auto revision = fixture.app.settings_publication_revision;
	UAM_ASSERT(fixture.coordinator.ResolveMemorySettingsUncertainty(fixture.app));
	UAM_ASSERT_EQ(fixture.app.settings_publication_revision, revision);
	UAM_ASSERT_EQ(fixture.app.settings.memory_idle_delay_seconds, 177);
	fixture.app.settings.memory_idle_delay_seconds = 188;
	SettingsMemoryWriteReceipt receipt;
	UAM_ASSERT(fixture.coordinator.SaveSettings(fixture.app, &receipt));
	UAM_ASSERT_EQ(receipt.outcome, Outcome::Committed);
	UAM_ASSERT(receipt.canonical_values_ready);
	UAM_ASSERT_EQ(receipt.canonical.memory_idle_delay_seconds, 188);
	UAM_ASSERT_EQ(fixture.Primary().memory_idle_delay_seconds, 188);
}

UAM_TEST(SettingsPublicationRejectedStageThrowPropagatesWithoutUnknownLatch)
{
	PublicationFixture fixture;
	const auto revision = fixture.app.settings_publication_revision;
	const auto bytes = uam_test::ReadFile(fixture.Path());
	bool threw = false;
	try
	{
		fixture.coordinator.SaveSettings(fixture.app, nullptr, [](Stage stage)
		{
			if (stage == Stage::BeforeReplace) throw std::runtime_error("private pre-replace fault");
			return true;
		});
	}
	catch (const std::runtime_error& error)
	{
		threw = true;
		UAM_ASSERT_EQ(std::string(error.what()), std::string("private pre-replace fault"));
	}
	UAM_ASSERT(threw && !fixture.app.memory_settings_uncertain);
	UAM_ASSERT_EQ(fixture.app.settings_publication_revision, revision + 1);
	UAM_ASSERT_EQ(uam_test::ReadFile(fixture.Path()), bytes);
}
UAM_TEST(SettingsPublicationRecoveredUnknownSurvivesRejectedSaveAndWholeSnapshotRollback)
{
	for (const bool throws : {false, true})
	{
		PublicationFixture fixture;
		fixture.Unknown(false); // Genuine post-replace uncertainty; primary contains 177.
		fixture.app.settings.memory_idle_delay_seconds = 60;
		const AppSettings previous = fixture.app.settings; // Existing legacy caller pattern.
		fixture.app.settings.provider_extra_flags = "--rejected-unrelated-setting";
		const auto revision = fixture.app.settings_publication_revision;
		int visits = 0;
		bool threw = false;
		try
		{
			UAM_ASSERT(!fixture.coordinator.SaveSettings(fixture.app, nullptr, [&](Stage stage)
			{
				if (stage != Stage::BeforeReplace) return true;
				++visits;
				if (throws) throw std::runtime_error("private recovered pre-replace fault");
				return false;
			}));
		}
		catch (const std::runtime_error& error)
		{
			threw = true;
			UAM_ASSERT_EQ(std::string(error.what()), std::string("private recovered pre-replace fault"));
		}
		UAM_ASSERT_EQ(visits, 1);
		UAM_ASSERT_EQ(threw, throws);
		UAM_ASSERT_EQ(fixture.app.settings_publication_revision, revision + 1);
		UAM_ASSERT_EQ(fixture.app.settings.memory_idle_delay_seconds, 177);
		UAM_ASSERT_EQ(fixture.Primary().memory_idle_delay_seconds, 177);
		UAM_ASSERT(fixture.app.memory_settings_uncertain);

		fixture.app.settings = previous; // Full rollback may restore the stale memory value.
		UAM_ASSERT_EQ(fixture.app.settings.memory_idle_delay_seconds, 60);
		fixture.app.settings.provider_extra_flags = "--retry-unrelated-setting";
		UAM_ASSERT(fixture.coordinator.SaveSettings(fixture.app));
		UAM_ASSERT_EQ(fixture.app.settings_publication_revision, revision + 2);
		UAM_ASSERT_EQ(fixture.app.settings.memory_idle_delay_seconds, 177);
		UAM_ASSERT_EQ(fixture.Primary().memory_idle_delay_seconds, 177);
		UAM_ASSERT_EQ(fixture.Primary().provider_extra_flags, std::string("--retry-unrelated-setting"));
		UAM_ASSERT(!fixture.app.memory_settings_uncertain);
	}
}

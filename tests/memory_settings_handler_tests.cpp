#include "cef_handler_fixture.h"
#include "app/persistence_coordinator.h"
#include <stdexcept>

using namespace uam_test;

namespace
{
	class MemorySettingsCallback final : public UamQueryHandler::Callback
	{
	public:
		int successes = 0;
		int failures = 0;
		bool throw_success = false;
		nlohmann::json body;
		void Success(const CefString& value) override
		{
			++successes;
			body = nlohmann::json::parse(value.ToString());
			if (throw_success) throw std::runtime_error("private memory response observation failure");
		}
		void Success(const void*, size_t) override { ++successes; }
		void Failure(int, const CefString&) override { ++failures; }
	private:
		IMPLEMENT_REFCOUNTING(MemorySettingsCallback);
	};

	struct MemorySettingsHandlerFixture
	{
		TempDir temp{"uam-memory-settings-handler"};
		uam::AppState app;
		std::unique_ptr<UamQueryHandler> handler;
		std::uint64_t request_serial = 0;
		explicit MemorySettingsHandlerFixture(bool seed = true)
		{
			app.data_root = temp.root;
			app.settings.memory_idle_delay_seconds = 60;
			if (seed) UAM_ASSERT(PersistenceCoordinator().SaveSettings(app));
			handler = UamQueryHandlerTestAccess::Create(app, "file:///private-memory-fixture/index.html");
		}
		CefRefPtr<MemorySettingsCallback> Dispatch(std::string_view action, const nlohmann::json& payload = nlohmann::json::object())
		{
			CefRefPtr<MemorySettingsCallback> callback = new MemorySettingsCallback;
			UAM_ASSERT(UamQueryHandlerTestAccess::MemorySettingsAction(*handler, action, payload, callback));
			return callback;
		}
		nlohmann::json Fence()
		{
			const auto response = Dispatch("getMemorySettings");
			UAM_ASSERT_EQ(response->successes, 1);
			UAM_ASSERT_EQ(response->failures, 0);
			return {{"expectedSettingsScope", response->body.at("scope")}, {"expectedSettingsRevision", response->body.at("revision")},
			    {"memoryRequestId", "private-memory-request-" + std::to_string(++request_serial)}};
		}
		AppSettings Primary()
		{
			AppSettings settings;
			const auto read = SettingsStore::LoadPrimary(AppPaths::SettingsFilePath(app.data_root), settings);
			UAM_ASSERT(read.loaded && !read.recovered_from_backup && !read.unrecovered_error);
			return settings;
		}
	};
}

UAM_TEST(MemorySettingsHandlerGetPrimaryOnlyAdvancesFenceWithoutSaving)
{
	MemorySettingsHandlerFixture fixture;
	const auto path = AppPaths::SettingsFilePath(fixture.app.data_root);
	const auto before = ReadFile(path);
	const auto revision = fixture.app.settings_publication_revision;
	const auto first = fixture.Dispatch("getMemorySettings");
	const auto second = fixture.Dispatch("getMemorySettings");
	UAM_ASSERT(first->body.at("primaryValidated").get<bool>());
	UAM_ASSERT_EQ(first->body.at("values").size(), std::size_t{10});
	UAM_ASSERT_EQ(first->body.at("scope"), second->body.at("scope"));
	UAM_ASSERT_EQ(second->body.at("revision").get<std::uint64_t>(), revision + 2);
	UAM_ASSERT_EQ(ReadFile(path), before);
	UAM_ASSERT(fixture.app.acp_sessions.empty());
}

UAM_TEST(MemorySettingsHandlerGetMissingPrimaryNeverAttestsBackupOrDefaults)
{
	MemorySettingsHandlerFixture fixture(false);
	const auto path = AppPaths::SettingsFilePath(fixture.app.data_root);
	UAM_ASSERT(SettingsStore::Save(std::filesystem::path(path.string() + ".bak"), fixture.app.settings));
	const auto read = fixture.Dispatch("getMemorySettings");
	UAM_ASSERT_EQ(read->successes, 1);
	UAM_ASSERT(!read->body.at("primaryValidated").get<bool>());
	UAM_ASSERT(!read->body.contains("values"));
	UAM_ASSERT(!std::filesystem::exists(path));
}

UAM_TEST(MemorySettingsHandlerRejectsStaleFenceBeforeMutationOrSave)
{
	MemorySettingsHandlerFixture fixture;
	auto payload = fixture.Fence();
	fixture.Dispatch("getMemorySettings"); // Independent barrier retires the captured write.
	const auto revision = fixture.app.settings_publication_revision;
	const auto before = ReadFile(AppPaths::SettingsFilePath(fixture.app.data_root));
	int stages = 0;
	UamQueryHandlerTestAccess::MemorySettingsFaults(*fixture.handler, [&](uam::io::AtomicWriteStage) { ++stages; return true; });
	payload["idleDelaySeconds"] = 177;
	payload["memoryRequestId"] = "stale-exact-request";
	const auto response = fixture.Dispatch("setMemorySettings", payload);
	UAM_ASSERT_EQ(response->body.at("outcome"), nlohmann::json("rejected"));
	UAM_ASSERT_EQ(response->body.at("memoryRequestId"), nlohmann::json("stale-exact-request"));
	UAM_ASSERT(response->body.at("primaryValidated").get<bool>());
	UAM_ASSERT_EQ(response->body.at("values").at("memoryIdleDelaySeconds"), nlohmann::json(60));
	UAM_ASSERT_EQ(fixture.app.settings.memory_idle_delay_seconds, 60);
	UAM_ASSERT_EQ(fixture.app.settings_publication_revision, revision);
	UAM_ASSERT_EQ(stages, 0);
	UAM_ASSERT_EQ(ReadFile(AppPaths::SettingsFilePath(fixture.app.data_root)), before);
}

UAM_TEST(MemorySettingsHandlerMalformedLaterFieldCannotPartiallyMutate)
{
	MemorySettingsHandlerFixture fixture;
	auto payload = fixture.Fence();
	payload["enabledDefault"] = false;
	payload["permissionReviewerModelId"] = nlohmann::json::array();
	const auto revision = fixture.app.settings_publication_revision;
	const auto before = ReadFile(AppPaths::SettingsFilePath(fixture.app.data_root));
	const auto response = fixture.Dispatch("setMemorySettings", payload);
	UAM_ASSERT_EQ(response->failures, 1);
	UAM_ASSERT_EQ(response->successes, 0);
	UAM_ASSERT(fixture.app.settings.memory_enabled_default);
	UAM_ASSERT_EQ(fixture.app.settings_publication_revision, revision);
	UAM_ASSERT_EQ(ReadFile(AppPaths::SettingsFilePath(fixture.app.data_root)), before);
}

UAM_TEST(MemorySettingsHandlerCommittedCanonicalSurvivesPostSaveException)
{
	MemorySettingsHandlerFixture fixture;
	auto payload = fixture.Fence();
	payload["idleDelaySeconds"] = -100;
	payload["memoryRequestId"] = "canonical-commit";
	UamQueryHandlerTestAccess::MemorySettingsFaults(*fixture.handler, {}, [] { throw std::runtime_error("private notification fault"); });
	const auto response = fixture.Dispatch("setMemorySettings", payload);
	UAM_ASSERT_EQ(response->successes, 1);
	UAM_ASSERT_EQ(response->failures, 0);
	UAM_ASSERT_EQ(response->body.at("outcome"), nlohmann::json("committed"));
	UAM_ASSERT(response->body.at("primaryValidated").get<bool>());
	const auto primary = fixture.Primary();
	UAM_ASSERT(primary.memory_idle_delay_seconds != -100);
	UAM_ASSERT_EQ(response->body.at("values").at("memoryIdleDelaySeconds"), nlohmann::json(primary.memory_idle_delay_seconds));
	UAM_ASSERT_EQ(fixture.app.settings.memory_idle_delay_seconds, primary.memory_idle_delay_seconds);
	UAM_ASSERT(!fixture.app.memory_settings_uncertain);
}

UAM_TEST(MemorySettingsHandlerBeforeReplaceRejectionPreservesPrimary)
{
	MemorySettingsHandlerFixture fixture;
	auto payload = fixture.Fence();
	payload["idleDelaySeconds"] = 177;
	const auto before = ReadFile(AppPaths::SettingsFilePath(fixture.app.data_root));
	UamQueryHandlerTestAccess::MemorySettingsFaults(*fixture.handler, [](uam::io::AtomicWriteStage stage) { return stage != uam::io::AtomicWriteStage::BeforeReplace; });
	const auto response = fixture.Dispatch("setMemorySettings", payload);
	UAM_ASSERT_EQ(response->body.at("outcome"), nlohmann::json("rejected"));
	UAM_ASSERT_EQ(response->body.at("values").at("memoryIdleDelaySeconds"), nlohmann::json(60));
	UAM_ASSERT_EQ(fixture.app.settings.memory_idle_delay_seconds, 60);
	UAM_ASSERT_EQ(ReadFile(AppPaths::SettingsFilePath(fixture.app.data_root)), before);
}

UAM_TEST(MemorySettingsHandlerUnknownThenRejectedUsesValidatedPrimaryNotOptimisticPredecessor)
{
	MemorySettingsHandlerFixture fixture;
	auto first = fixture.Fence();
	first["idleDelaySeconds"] = 177;
	UamQueryHandlerTestAccess::MemorySettingsFaults(*fixture.handler, [](uam::io::AtomicWriteStage stage) { return stage != uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync; });
	const auto unknown = fixture.Dispatch("setMemorySettings", first);
	UAM_ASSERT_EQ(unknown->body.at("outcome"), nlohmann::json("unknown"));
	UAM_ASSERT(!unknown->body.at("primaryValidated").get<bool>() && !unknown->body.contains("values"));
	UAM_ASSERT(fixture.app.memory_settings_uncertain);
	UAM_ASSERT_EQ(fixture.Primary().memory_idle_delay_seconds, 177); // Observation, not retroactive commit proof.
	auto second = fixture.Fence();
	second["idleDelaySeconds"] = 188;
	UamQueryHandlerTestAccess::MemorySettingsFaults(*fixture.handler, [](uam::io::AtomicWriteStage stage) { return stage != uam::io::AtomicWriteStage::BeforeReplace; });
	const auto rejected = fixture.Dispatch("setMemorySettings", second);
	UAM_ASSERT_EQ(rejected->body.at("outcome"), nlohmann::json("rejected"));
	UAM_ASSERT(rejected->body.at("primaryValidated").get<bool>());
	UAM_ASSERT_EQ(rejected->body.at("values").at("memoryIdleDelaySeconds"), nlohmann::json(177));
	UAM_ASSERT_EQ(fixture.app.settings.memory_idle_delay_seconds, 177);
}

UAM_TEST(MemorySettingsHandlerLostSuccessCannotUndoCommittedPrimary)
{
	MemorySettingsHandlerFixture fixture;
	auto payload = fixture.Fence();
	payload["idleDelaySeconds"] = 177;
	CefRefPtr<MemorySettingsCallback> callback = new MemorySettingsCallback;
	callback->throw_success = true;
	bool threw = false;
	try { UamQueryHandlerTestAccess::MemorySettingsAction(*fixture.handler, "setMemorySettings", payload, callback); }
	catch (const std::runtime_error&) { threw = true; }
	UAM_ASSERT(!threw);
	UAM_ASSERT_EQ(callback->body.at("outcome"), nlohmann::json("committed"));
	UAM_ASSERT_EQ(callback->successes, 1);
	UAM_ASSERT_EQ(callback->failures, 0);
	UAM_ASSERT_EQ(fixture.Primary().memory_idle_delay_seconds, 177);
	UAM_ASSERT_EQ(fixture.app.settings.memory_idle_delay_seconds, 177);
	UAM_ASSERT(!fixture.app.memory_settings_uncertain);
}

UAM_TEST(MemorySettingsHandlerLegacyNoncommittedWriteNeverUsesSuccessAcknowledgment)
{
	for (const bool after_replace : {false, true})
	{
		MemorySettingsHandlerFixture fixture;
		int visits = 0;
		UamQueryHandlerTestAccess::MemorySettingsFaults(*fixture.handler, [&](uam::io::AtomicWriteStage stage)
		{
			const auto failure = after_replace ? uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync : uam::io::AtomicWriteStage::BeforeReplace;
			if (stage != failure) return true;
			++visits;
			return false;
		});
		// Actual legacy iOS wire shape: no fence and no memoryRequestId.
		const auto callback = fixture.Dispatch("setMemorySettings", {{"idleDelaySeconds", 177}});
		UAM_ASSERT_EQ(visits, 1);
		UAM_ASSERT_EQ(callback->successes, 0);
		UAM_ASSERT_EQ(callback->failures, 1);
		UAM_ASSERT_EQ(fixture.app.memory_settings_uncertain, after_replace);
		UAM_ASSERT_EQ(fixture.Primary().memory_idle_delay_seconds, after_replace ? 177 : 60);
		UAM_ASSERT_EQ(fixture.app.settings.memory_idle_delay_seconds, after_replace ? 177 : 60);
	}
}

UAM_TEST(MemorySettingsHandlerPartialFenceCannotMutateThroughLegacyRoute)
{
	MemorySettingsHandlerFixture fixture;
	const auto complete = fixture.Fence();
	const auto revision = fixture.app.settings_publication_revision;
	const auto before = ReadFile(AppPaths::SettingsFilePath(fixture.app.data_root));
	int stages = 0;
	UamQueryHandlerTestAccess::MemorySettingsFaults(*fixture.handler, [&](uam::io::AtomicWriteStage) { ++stages; return true; });
	for (int mask = 1; mask < 7; ++mask)
	{
		nlohmann::json payload = {{"idleDelaySeconds", 177}};
		int bit = 1;
		for (const char* key : {"expectedSettingsScope", "expectedSettingsRevision", "memoryRequestId"})
		{
			if (mask & bit) payload[key] = complete.at(key);
			bit <<= 1;
		}
		const auto response = fixture.Dispatch("setMemorySettings", payload);
		UAM_ASSERT_EQ(response->successes, 0);
		UAM_ASSERT_EQ(response->failures, 1);
		UAM_ASSERT_EQ(fixture.app.settings.memory_idle_delay_seconds, 60);
		UAM_ASSERT_EQ(fixture.app.settings_publication_revision, revision);
		UAM_ASSERT_EQ(ReadFile(AppPaths::SettingsFilePath(fixture.app.data_root)), before);
	}
	UAM_ASSERT_EQ(stages, 0);
}

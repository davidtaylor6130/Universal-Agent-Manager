#include "test_harness.h"
#include "common/runtime/acp/acp_model_json.h"

#include <limits>

using namespace uam_test;

UAM_TEST(ModelMetadataPreservesExplicitCapacityAndUnknownValues)
{
	const std::optional<uam::AcpModelState> known = uam::acp_models::ParseAcpModelState({{"id", "fixture-model"}, {"contextWindowTokens", 200000}, {"maxOutputTokens", 0}});
	UAM_ASSERT(known.has_value());
	UAM_ASSERT_EQ(known->context_window_tokens.value_or(-1), std::int64_t{200000});
	UAM_ASSERT_EQ(known->max_output_tokens.value_or(-1), std::int64_t{0});
	const std::optional<uam::AcpModelState> unknown = uam::acp_models::ParseAcpModelState({{"id", "fixture-model"}});
	UAM_ASSERT(unknown.has_value());
	UAM_ASSERT(!unknown->context_window_tokens.has_value());
	UAM_ASSERT(!unknown->max_output_tokens.has_value());
	UAM_ASSERT_EQ(uam::acp_models::ModelTokenLimit({{"limit", 9007199254740991LL}}, {"limit"}).value_or(-1), std::int64_t{9007199254740991LL});
	UAM_ASSERT(!uam::acp_models::ModelTokenLimit({{"normalized", nullptr}, {"alias", 200000}}, {"normalized", "alias"}).has_value());
	for (const nlohmann::json& invalid : std::vector<nlohmann::json>{nullptr, -1, 1.5, "200000", true, std::uint64_t{9007199254740992ULL}, std::numeric_limits<std::uint64_t>::max()})
	{
		const std::optional<uam::AcpModelState> parsed = uam::acp_models::ParseAcpModelState({{"id", "fixture-model"}, {"contextWindowTokens", invalid}, {"maxOutputTokens", invalid}});
		UAM_ASSERT(parsed.has_value());
		UAM_ASSERT(!parsed->context_window_tokens.has_value());
		UAM_ASSERT(!parsed->max_output_tokens.has_value());
	}
}

UAM_TEST(CodexModelMetadataUsesReportedCapacityWithoutMaximumGuess)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-model-metadata-cache");
	ScopedEnvVar disable_refresh("UAM_DISABLE_OPENCODE_ZEN_REFRESH", "1");
	ScopedEnvVar config_home("XDG_CONFIG_HOME", temp.root.string());
	ScopedEnvVar custom_config("OPENCODE_CONFIG", (temp.root / "missing.json").string());
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile()};
	app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
	app.provider_model_catalog->Initialize(app.data_root, app.provider_profiles);
	ChatSession chat;
	chat.provider_id = "codex-cli";
	chat.workspace_directory = temp.root.string();
	uam::AcpSessionState session;
	session.provider_id = "codex-cli";
	session.model_catalog_scope_key = app.provider_model_catalog->CaptureScopeKey(session.provider_id, chat.workspace_directory);
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(session.provider_id);
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "model/list", "1", {{"data", {{{"id", "fixture-codex"}, {"context_window", 272000}, {"max_context_window", 1000000}}, {{"id", "unknown-codex"}, {"max_context_window", 1000000}}}}}));
	UAM_ASSERT_EQ(session.available_models.size(), std::size_t{2});
	UAM_ASSERT_EQ(session.available_models[0].context_window_tokens.value_or(-1), std::int64_t{272000});
	UAM_ASSERT(!session.available_models[0].max_output_tokens.has_value());
	UAM_ASSERT(!session.available_models[1].context_window_tokens.has_value());
	const nlohmann::json cached = app.provider_model_catalog->GetCachedProviderModels("codex-cli", chat.workspace_directory);
	UAM_ASSERT_EQ(cached.size(), std::size_t{2});
	UAM_ASSERT_EQ(cached[0].value("contextWindowTokens", -1LL), 272000LL);
	uam::ProviderModelCatalogService reloaded;
	reloaded.Initialize(app.data_root, app.provider_profiles);
	const nlohmann::json restored = reloaded.GetCachedProviderModels("codex-cli", chat.workspace_directory);
	UAM_ASSERT_EQ(restored, cached);
	const std::optional<uam::AcpModelState> parsed = uam::acp_models::ParseAcpModelState(restored[0]);
	UAM_ASSERT(parsed.has_value());
	UAM_ASSERT_EQ(parsed->context_window_tokens.value_or(-1), std::int64_t{272000});
#endif
}

UAM_TEST(ModelMetadataSurvivesNativeFrontendProjection)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "metadata-chat";
	chat.provider_id = "gemini-cli";
	app.chats.push_back(chat);
	std::unique_ptr<uam::AcpSessionState> session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->provider_id = chat.provider_id;
	const std::optional<uam::AcpModelState> model = uam::acp_models::ParseAcpModelState({{"id", "fixture-model"}, {"contextWindowTokens", 200000}, {"maxOutputTokens", 8192}});
	UAM_ASSERT(model.has_value());
	session->available_models.push_back(*model);
	session->available_models.push_back(uam::AcpModelState{"unknown-model", "Unknown", ""});
	app.acp_sessions.push_back(std::move(session));
	const nlohmann::json models = uam::StateSerializer::Serialize(app)["chats"][0]["acpSession"]["availableModels"];
	UAM_ASSERT_EQ(models[0].value("contextWindowTokens", -1LL), 200000LL);
	UAM_ASSERT_EQ(models[0].value("maxOutputTokens", -1LL), 8192LL);
	UAM_ASSERT(models[1]["contextWindowTokens"].is_null());
	UAM_ASSERT(models[1]["maxOutputTokens"].is_null());
}

UAM_TEST(OpenCodeConfiguredModelMetadataKeepsExplicitLimitsAndFirstDefinition)
{
	TempDir temp("uam-opencode-model-metadata");
	ScopedEnvVar config_home("XDG_CONFIG_HOME", temp.root.string());
	ScopedEnvVar config_override("OPENCODE_CONFIG", (temp.root / "override.json").string());
	ScopedEnvVar disable_refresh("UAM_DISABLE_OPENCODE_ZEN_REFRESH", "1");
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "opencode" / "opencode.json", R"({"provider":{"fixture":{"models":{"known":{"limit":{"context":32768,"output":4096}},"unknown":{},"invalid":{"limit":{"context":-1,"output":"4096"}}}}}})"));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "override.json", R"({"provider":{"fixture":{"models":{"known":{"limit":{"context":999,"output":999}}}}}})"));
	uam::ProviderModelCatalogService catalog;
	catalog.Initialize(temp.root / "data");
	const nlohmann::json models = catalog.GetConfiguredOpenCodeModels();
	UAM_ASSERT_EQ(models.size(), std::size_t{3});
	for (const nlohmann::json& model : models)
	{
		if (model.value("id", "") == "fixture/known")
		{
			UAM_ASSERT_EQ(model.value("contextWindowTokens", -1LL), 32768LL);
			UAM_ASSERT_EQ(model.value("maxOutputTokens", -1LL), 4096LL);
		}
		else
		{
			UAM_ASSERT(!model.contains("contextWindowTokens"));
			UAM_ASSERT(!model.contains("maxOutputTokens"));
		}
	}
}

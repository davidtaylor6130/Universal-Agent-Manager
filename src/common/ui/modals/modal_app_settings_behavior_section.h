#pragma once

#include "common/runtime/local_engine_runtime_service.h"
#include "common/rag/rag_app_helpers.h"

/// <summary>
/// General / behavior preference toggles.
/// </summary>
inline void DrawAppSettingsBehaviorControls(AppSettings& draft_settings)
{
	ImGui::Checkbox("Confirm before deleting chat", &draft_settings.confirm_delete_chat);
	ImGui::Dummy(ImVec2(0.0f, ScaleUiLength(ui::kSpace4)));
	ImGui::Checkbox("Confirm before deleting folder", &draft_settings.confirm_delete_folder);
	ImGui::Dummy(ImVec2(0.0f, ScaleUiLength(ui::kSpace4)));
	ImGui::Checkbox("Remember last selected chat", &draft_settings.remember_last_chat);
	ImGui::Dummy(ImVec2(0.0f, ScaleUiLength(ui::kSpace12)));
	ImGui::TextColored(ui::kTextSecondary, "Session");
	ImGui::Dummy(ImVec2(0.0f, ScaleUiLength(ui::kSpace4)));
	draft_settings.cli_idle_timeout_seconds = std::clamp(draft_settings.cli_idle_timeout_seconds, 30, 3600);
	ImGui::SetNextItemWidth(ScaleUiLength(140.0f));
	ImGui::InputInt("CLI idle timeout (sec)", &draft_settings.cli_idle_timeout_seconds);
	draft_settings.cli_idle_timeout_seconds = std::clamp(draft_settings.cli_idle_timeout_seconds, 30, 3600);
	ImGui::TextColored(ui::kTextMuted, "Range: 30 – 3600 seconds.");
}

/// <summary>
/// Provider selection info and local runtime model configuration.
/// </summary>
inline void DrawAppSettingsProviderControls(AppState& app, AppSettings& draft_settings)
{
	const ProviderProfile* active_profile = ProviderProfileStore::FindById(app.provider_profiles, app.settings.active_provider_id);
	std::string active_label = "(none)";

	if (active_profile != nullptr)
	{
		active_label = (active_profile->title.empty() ? active_profile->id : active_profile->title)
		             + std::string(ProviderRuntime::UsesCliOutput(*active_profile) ? "  [CLI]" : "  [Structured]");
	}

	ImGui::TextColored(ui::kTextMuted, "Active provider (last selected)");
	ImGui::TextColored(ui::kTextPrimary, "%s", active_label.c_str());
	ImGui::TextColored(ui::kTextMuted, "The active provider is selected when creating a new chat.");

#if UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
	ImGui::Dummy(ImVec2(0.0f, ScaleUiLength(ui::kSpace12)));
	ImGui::TextColored(ui::kTextSecondary, "Ollama Engine");
	ImGui::Dummy(ImVec2(0.0f, ScaleUiLength(ui::kSpace4)));
	ImGui::TextColored(ui::kTextMuted, "Models folder directory");
	std::string browse_error;
	DrawPathInputWithBrowseButton("##models_folder_directory", draft_settings.models_folder_directory, "app_settings_models_folder_picker", PathBrowseTarget::Directory, -1.0f, nullptr, nullptr, &browse_error);

	if (!browse_error.empty())
	{
		app.status_line = browse_error;
	}

	ImGui::TextColored(ui::kTextMuted, "Leave empty to auto-detect.");

	ImGui::Dummy(ImVec2(0.0f, ScaleUiLength(ui::kSpace8)));
	static std::vector<std::string> s_runtime_models_cache;
	static double s_last_model_refresh = -1.0;
	const double now_s = ImGui::GetTime();

	if (s_last_model_refresh < 0.0 || (now_s - s_last_model_refresh) > 1.0)
	{
		const fs::path model_folder = ResolveRagModelFolder(app, &draft_settings);
		s_runtime_models_cache = app.runtime_model_service.ListModels(model_folder);
		s_last_model_refresh = now_s;
	}

	ImGui::TextColored(ui::kTextMuted, "Runtime model");
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputText("##runtime_model_id", &draft_settings.selected_model_id);
	const std::string runtime_preview = s_runtime_models_cache.empty()
	    ? std::string("(no models found)")
	    : (draft_settings.selected_model_id.empty() ? std::string("(auto)") : draft_settings.selected_model_id);

	if (ImGui::BeginCombo("Runtime model", runtime_preview.c_str()))
	{
		if (s_runtime_models_cache.empty())
		{
			ImGui::BeginDisabled();
			ImGui::Selectable("No local runtime models found", false);
			ImGui::EndDisabled();
		}
		else
		{
			if (ImGui::Selectable("(auto)", draft_settings.selected_model_id.empty()))
				draft_settings.selected_model_id.clear();

			for (const std::string& model : s_runtime_models_cache)
			{
				if (ImGui::Selectable(model.c_str(), draft_settings.selected_model_id == model))
					draft_settings.selected_model_id = model;
			}
		}

		ImGui::EndCombo();
	}
#else
	(void)app;
	(void)draft_settings;
#endif
}

/// <summary>
/// Vector retrieval / embedding model configuration.
/// Only available when UAM_ENABLE_ENGINE_RAG is set.
/// </summary>
inline void DrawAppSettingsVectorRetrievalControls(AppState& app, AppSettings& draft_settings)
{
#if UAM_ENABLE_ENGINE_RAG
	const bool vector_none = (draft_settings.vector_db_backend == "none");

	if (ImGui::RadioButton("Ollama Engine Vector DB", !vector_none))
		draft_settings.vector_db_backend = "ollama-engine";

	ImGui::SameLine();

	if (ImGui::RadioButton("Disabled", vector_none))
		draft_settings.vector_db_backend = "none";

	ImGui::Dummy(ImVec2(0.0f, ScaleUiLength(ui::kSpace8)));
	ImGui::TextColored(ui::kTextMuted, "Vector model");
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputText("##vector_model_id", &draft_settings.selected_vector_model_id);

	static std::vector<std::string> s_vector_models_cache;
	static double s_last_vector_refresh = -1.0;
	const double now_s = ImGui::GetTime();

	if (s_last_vector_refresh < 0.0 || (now_s - s_last_vector_refresh) > 1.0)
	{
		const fs::path model_folder = ResolveRagModelFolder(app, &draft_settings);
		app.rag_index_service.SetModelFolder(model_folder);
		s_vector_models_cache = app.rag_index_service.ListModels();
		s_last_vector_refresh = now_s;
	}

	const std::string vector_preview = s_vector_models_cache.empty()
	    ? std::string("(no models found)")
	    : (draft_settings.selected_vector_model_id.empty() ? std::string("(auto)") : draft_settings.selected_vector_model_id);

	if (ImGui::BeginCombo("Vector model", vector_preview.c_str()))
	{
		if (s_vector_models_cache.empty())
		{
			ImGui::BeginDisabled();
			ImGui::Selectable("No local vector models found", false);
			ImGui::EndDisabled();
		}
		else
		{
			if (ImGui::Selectable("(auto)", draft_settings.selected_vector_model_id.empty()))
				draft_settings.selected_vector_model_id.clear();

			for (const std::string& model : s_vector_models_cache)
			{
				if (ImGui::Selectable(model.c_str(), draft_settings.selected_vector_model_id == model))
					draft_settings.selected_vector_model_id = model;
			}
		}

		ImGui::EndCombo();
	}

	ImGui::Dummy(ImVec2(0.0f, ScaleUiLength(ui::kSpace8)));
	ImGui::TextColored(ui::kTextMuted, "Database name override");
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputText("##vector_database_name_override", &draft_settings.vector_database_name_override);
#else
	draft_settings.vector_db_backend = "none";
	draft_settings.selected_vector_model_id.clear();
	ImGui::TextColored(ui::kTextMuted, "Vector retrieval is not enabled in this build.");
	(void)app;
#endif
}

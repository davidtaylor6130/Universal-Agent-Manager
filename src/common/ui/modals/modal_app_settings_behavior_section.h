#pragma once

#include "common/runtime/local_engine_runtime_service.h"
#include "common/rag/rag_app_helpers.h"

/// <summary>
/// Draws behavior toggles in the app settings modal.
/// </summary>
inline void DrawAppSettingsBehaviorSection(AppState& app, AppSettings& draft_settings)
{
	ImGui::TextColored(ui::kTextSecondary, "Behavior");
	ImGui::Checkbox("Confirm before deleting chat", &draft_settings.confirm_delete_chat);
	ImGui::Checkbox("Confirm before deleting folder", &draft_settings.confirm_delete_folder);
	ImGui::Checkbox("Remember last selected chat", &draft_settings.remember_last_chat);
	draft_settings.cli_idle_timeout_seconds = std::clamp(draft_settings.cli_idle_timeout_seconds, 30, 3600);
	ImGui::SetNextItemWidth(140.0f);
	ImGui::InputInt("CLI idle timeout (sec)", &draft_settings.cli_idle_timeout_seconds);
	draft_settings.cli_idle_timeout_seconds = std::clamp(draft_settings.cli_idle_timeout_seconds, 30, 3600);

	ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
	ImGui::TextColored(ui::kTextSecondary, "Provider");
	const ProviderProfile* active_profile = ProviderProfileStore::FindById(app.provider_profiles, app.settings.active_provider_id);
	std::string active_label = "(none)";

	if (active_profile != nullptr)
	{
		active_label = (active_profile->title.empty() ? active_profile->id : active_profile->title) + std::string(ProviderRuntime::UsesCliOutput(*active_profile) ? " [CLI]" : " [Structured]");
	}

	ImGui::TextColored(ui::kTextMuted, "Choose provider when creating a new chat.");
	ImGui::TextColored(ui::kTextMuted, "Last selected: %s", active_label.c_str());

	ImGui::TextColored(ui::kTextMuted, "Models folder directory (Ollama Engine)");
	std::string browse_error;
	DrawPathInputWithBrowseButton("##models_folder_directory", draft_settings.models_folder_directory, "app_settings_models_folder_picker", PathBrowseTarget::Directory, -1.0f, nullptr, nullptr, &browse_error);

	if (!browse_error.empty())
	{
		app.status_line = browse_error;
	}

	ImGui::TextColored(ui::kTextMuted, "Leave empty to auto-detect models folder.");

	ImGui::TextColored(ui::kTextMuted, "Model id (runtime)");
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputText("##runtime_model_id", &draft_settings.selected_model_id);
	static std::vector<std::string> runtime_models_cache;
	static std::vector<std::string> vector_models_cache;
	static double last_model_refresh_s = -1.0;
	const double now_s = ImGui::GetTime();

	if (last_model_refresh_s < 0.0 || (now_s - last_model_refresh_s) > 1.0)
	{
		const fs::path model_folder = ResolveRagModelFolder(app, &draft_settings);
		app.rag_index_service.SetModelFolder(model_folder);
		runtime_models_cache = app.runtime_model_service.ListModels(model_folder);
		vector_models_cache = app.rag_index_service.ListModels();
		last_model_refresh_s = now_s;
	}

	const std::vector<std::string>& runtime_models = runtime_models_cache;
	const std::string runtime_model_preview = runtime_models.empty() ? std::string("(no models found)") : (draft_settings.selected_model_id.empty() ? std::string("(auto)") : draft_settings.selected_model_id);

	if (ImGui::BeginCombo("Runtime model", runtime_model_preview.c_str()))
	{
		if (runtime_models.empty())
		{
			ImGui::BeginDisabled();
			ImGui::Selectable("No local runtime models found", false);
			ImGui::EndDisabled();
		}
		else
		{
			if (ImGui::Selectable("(auto)", draft_settings.selected_model_id.empty()))
			{
				draft_settings.selected_model_id.clear();
			}

			for (const std::string& model : runtime_models)
			{
				const bool selected = (draft_settings.selected_model_id == model);

				if (ImGui::Selectable(model.c_str(), selected))
				{
					draft_settings.selected_model_id = model;
				}
			}
		}

		ImGui::EndCombo();
	}

	ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
	ImGui::TextColored(ui::kTextSecondary, "Vector Retrieval");
#if UAM_ENABLE_ENGINE_RAG
	const bool vector_none = (draft_settings.vector_db_backend == "none");

	if (ImGui::RadioButton("Ollama Engine Vector DB", !vector_none))
	{
		draft_settings.vector_db_backend = "ollama-engine";
	}

	ImGui::SameLine();

	if (ImGui::RadioButton("Disabled", vector_none))
	{
		draft_settings.vector_db_backend = "none";
	}

	ImGui::TextColored(ui::kTextMuted, "Vector model id");
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputText("##vector_model_id", &draft_settings.selected_vector_model_id);
	const std::vector<std::string>& vector_models = vector_models_cache;
	const std::string vector_model_preview = vector_models.empty() ? std::string("(no models found)") : (draft_settings.selected_vector_model_id.empty() ? std::string("(auto)") : draft_settings.selected_vector_model_id);

	if (ImGui::BeginCombo("Vector model", vector_model_preview.c_str()))
	{
		if (vector_models.empty())
		{
			ImGui::BeginDisabled();
			ImGui::Selectable("No local vector models found", false);
			ImGui::EndDisabled();
		}
		else
		{
			if (ImGui::Selectable("(auto)", draft_settings.selected_vector_model_id.empty()))
			{
				draft_settings.selected_vector_model_id.clear();
			}

			for (const std::string& model : vector_models)
			{
				const bool selected = (draft_settings.selected_vector_model_id == model);

				if (ImGui::Selectable(model.c_str(), selected))
				{
					draft_settings.selected_vector_model_id = model;
				}
			}
		}

		ImGui::EndCombo();
	}

	ImGui::TextColored(ui::kTextMuted, "Vector database name override");
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputText("##vector_database_name_override", &draft_settings.vector_database_name_override);
#else
	draft_settings.vector_db_backend = "none";
	draft_settings.selected_vector_model_id.clear();
	ImGui::TextColored(ui::kTextMuted, "Vector retrieval is disabled in this build.");
#endif
}

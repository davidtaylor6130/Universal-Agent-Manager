#include "template_runtime_service.h"

#include "app/application_core_helpers.h"

#include "common/markdown_template_catalog.h"
#include "common/platform/platform_services.h"

namespace fs = std::filesystem;
using uam::AppState;

std::string TemplateRuntimeService::BuildShellCommandWithWorkingDirectory(const fs::path& working_directory,
	                                                                       const std::string& command) const
{
	return PlatformServicesFactory::Instance().process_service.BuildShellCommandWithWorkingDirectory(working_directory, command);
}

bool TemplateRuntimeService::EnsureWorkspaceProviderLayout(const AppState& app, const ChatSession& chat, std::string* error_out) const
{
	std::error_code ec;
	const fs::path workspace_root = ResolveWorkspaceRootPath(app, chat);
	fs::create_directories(workspace_root, ec);

	if (ec)
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to create workspace root '" + workspace_root.string() + "': " + ec.message();
		}

		return false;
	}

	const fs::path provider_root = WorkspacePromptProfileRootPath(app, chat);
	fs::create_directories(provider_root, ec);

	if (ec)
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to create local prompt profile directory: " + ec.message();
		}

		return false;
	}

	fs::create_directories(provider_root / "Lessons", ec);

	if (ec)
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to create prompt profile Lessons directory: " + ec.message();
		}

		return false;
	}

	fs::create_directories(provider_root / "Failures", ec);

	if (ec)
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to create prompt profile Failures directory: " + ec.message();
		}

		return false;
	}

	fs::create_directories(provider_root / "auto-test", ec);

	if (ec)
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to create prompt profile auto-test directory: " + ec.message();
		}

		return false;
	}

	return true;
}

void TemplateRuntimeService::MarkTemplateCatalogDirty(AppState& app) const
{
	app.template_catalog_dirty = true;
}

bool TemplateRuntimeService::RefreshTemplateCatalog(AppState& app, const bool force) const
{
	if (!force && !app.template_catalog_dirty)
	{
		return true;
	}

	std::string error;
	const fs::path global_root = ResolvePromptProfileRootPath(app.settings);

	if (!MarkdownTemplateCatalog::EnsureCatalogPath(global_root, &error))
	{
		app.template_catalog.clear();
		app.template_catalog_dirty = false;

		if (!error.empty())
		{
			app.status_line = error;
		}

		return false;
	}

	app.template_catalog = MarkdownTemplateCatalog::List(global_root);
	app.template_catalog_dirty = false;
	return true;
}

const TemplateCatalogEntry* TemplateRuntimeService::FindTemplateEntryById(const AppState& app, const std::string& template_id) const
{
	if (template_id.empty())
	{
		return nullptr;
	}

	for (const TemplateCatalogEntry& entry : app.template_catalog)
	{
		if (entry.id == template_id)
		{
			return &entry;
		}
	}

	return nullptr;
}

std::string TemplateRuntimeService::TemplateLabelOrFallback(const AppState& app, const std::string& template_id) const
{
	const TemplateCatalogEntry* entry = FindTemplateEntryById(app, template_id);

	if (entry != nullptr)
	{
		return entry->display_name;
	}

	return template_id.empty() ? "None" : ("Missing: " + template_id);
}

TemplateRuntimeService& GetTemplateRuntimeService()
{
	static TemplateRuntimeService service;
	return service;
}

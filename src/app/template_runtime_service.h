#ifndef UAM_APP_TEMPLATE_RUNTIME_SERVICE_H
#define UAM_APP_TEMPLATE_RUNTIME_SERVICE_H


#include "common/state/app_state.h"

#include <filesystem>
#include <string>

class TemplateRuntimeService
{
  public:
	std::string BuildShellCommandWithWorkingDirectory(const std::filesystem::path& working_directory,
	                                                  const std::string& command) const;
	bool EnsureWorkspaceProviderLayout(const uam::AppState& app,
	                                  const ChatSession& chat,
	                                  std::string* error_out = nullptr) const;
	void MarkTemplateCatalogDirty(uam::AppState& app) const;
	bool RefreshTemplateCatalog(uam::AppState& app, bool force = false) const;
	const TemplateCatalogEntry* FindTemplateEntryById(const uam::AppState& app,
	                                                const std::string& template_id) const;
	std::string TemplateLabelOrFallback(const uam::AppState& app,
	                                    const std::string& template_id) const;
};

TemplateRuntimeService& GetTemplateRuntimeService();

#endif // UAM_APP_TEMPLATE_RUNTIME_SERVICE_H

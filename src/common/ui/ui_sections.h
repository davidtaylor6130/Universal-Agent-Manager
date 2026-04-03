#pragma once

#include "app/chat_domain_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_profile_migration_service.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_local_service.h"
#include "app/runtime_orchestration_services.h"
#include "app/template_runtime_service.h"
#include "common/app_paths.h"
#include "common/chat_branching.h"
#include "common/chat_folder_store.h"
#include "common/provider_runtime.h"

/// <summary>
/// Chat actions and mutation helpers used by sidebar and detail views.
/// </summary>
#include "common/ui/ui_chat_actions.h"

/// <summary>
/// Shared Dear ImGui styling, theme, and top-level menu helpers.
/// </summary>
#include "common/ui/ui_theme_helpers.h"

/// <summary>
/// Sidebar tree rendering, folder controls, and context popups.
/// </summary>
#include "common/ui/ui_sidebar.h"

/// <summary>
/// Conversation pane rendering, terminal surface, and agent controls.
/// </summary>
#include "common/ui/ui_chat_detail.h"

/// <summary>
/// Modal windows for settings, templates, and confirmations.
/// </summary>
#include "common/ui/ui_modals.h"

/// <summary>
/// Global keyboard shortcut handlers.
/// </summary>
#include "common/ui/ui_shortcuts.h"

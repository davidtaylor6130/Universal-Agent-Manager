#pragma once

#include "common/provider/runtime/provider_build_config.h"

/// <summary>
/// Window size/state helper functions.
/// </summary>
#include "common/ui/modals/modal_window_state.h"

/// <summary>
/// About dialog modal.
/// </summary>
#include "common/ui/modals/modal_about.h"

/// <summary>
/// Chat/folder delete confirmation modals.
/// </summary>
#include "common/ui/modals/modal_delete_chat.h"
#include "common/ui/modals/modal_delete_folder.h"

/// <summary>
/// Move chat missing session warning modal.
/// </summary>
#include "common/ui/modals/modal_move_chat_missing_session.h"

/// <summary>
/// Folder settings modal.
/// </summary>
#include "common/ui/modals/modal_folder_settings.h"

/// <summary>
/// Chat rename modal.
/// </summary>
#include "common/ui/modals/modal_rename_chat.h"

/// <summary>
/// Template change warning modal.
/// </summary>
#include "common/ui/modals/modal_template_change_warning.h"

/// <summary>
/// Template manager modal and section components.
/// </summary>
#include "common/ui/modals/modal_template_manager.h"

/// <summary>
/// VCS output modal.
/// </summary>
#include "common/ui/modals/modal_vcs_output.h"

/// <summary>
/// Runtime model picker modal.
/// </summary>
#include "common/ui/modals/modal_runtime_model_selection.h"

#if UAM_ENABLE_ENGINE_RAG
/// <summary>
/// Project-wide RAG console modal.
/// </summary>
#include "common/ui/modals/modal_rag_console.h"
#endif

/// <summary>
/// App settings modal and section components.
/// </summary>
#include "common/ui/modals/modal_app_settings.h"

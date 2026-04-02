#pragma once

/// <summary>
/// Cross-platform terminal lifecycle management and shared libvterm plumbing.
/// </summary>
#include "common/runtime/terminal_common.h"

/// <summary>
/// Platform startup dispatch for PTY/ConPTY process attachment.
/// </summary>
#include "common/platform/terminal_startup_dispatch.h"

/// <summary>
/// Terminal rendering/polling integration and Gemini request orchestration.
/// </summary>
#include "common/runtime/terminal_polling.h"

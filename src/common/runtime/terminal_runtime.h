#pragma once

/// <summary>
/// Cross-platform terminal lifecycle management and shared libvterm plumbing.
/// </summary>
#include "common/runtime/terminal_common.h"

/// <summary>
/// Platform startup is handled via IPlatformTerminalRuntime implementations.
/// </summary>

/// <summary>
/// Terminal rendering/polling integration and Gemini request orchestration.
/// </summary>
#include "common/runtime/terminal_polling.h"

#pragma once

/// <summary>
/// Cross-platform terminal lifecycle management and shared libvterm plumbing.
/// </summary>
#include "common/runtime/terminal_common.h"

/// <summary>
/// Unix/macOS PTY startup and process attachment.
/// </summary>
#include "common/platform/terminal_unix.h"

/// <summary>
/// Windows ConPTY startup and process attachment.
/// </summary>
#include "common/platform/terminal_windows.h"

/// <summary>
/// Terminal rendering/polling integration and Gemini request orchestration.
/// </summary>
#include "common/runtime/terminal_polling.h"

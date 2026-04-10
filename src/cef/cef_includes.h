#pragma once

// Platform-safe CEF include wrapper.
// The CEF binary distribution places headers in an include/ subdirectory.
// CMake sets up the include path so we can use <cef_*> directly.

#include "include/cef_app.h"
#include "include/cef_path_util.h"
#include "include/cef_browser.h"
#include "include/cef_browser_process_handler.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_display_handler.h"
#include "include/cef_frame.h"
#include "include/cef_keyboard_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_scheme.h"
#include "include/cef_v8.h"
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_message_router.h"

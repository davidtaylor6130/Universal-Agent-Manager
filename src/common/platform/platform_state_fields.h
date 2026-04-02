#pragma once

#include "common/app_models.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <wincontypes.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#else
#error "platform_state_fields.h is only supported on Windows and macOS."
#endif

namespace uam::platform
{

	struct CliTerminalPlatformFields
	{
#if defined(_WIN32)
		HANDLE pipe_input = INVALID_HANDLE_VALUE;
		HANDLE pipe_output = INVALID_HANDLE_VALUE;
		PROCESS_INFORMATION process_info = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, 0, 0};
		HPCON pseudo_console = nullptr;
		LPPROC_THREAD_ATTRIBUTE_LIST attr_list = nullptr;
#elif defined(__APPLE__)
		int master_fd = -1;
		pid_t child_pid = -1;
#else
#error "CliTerminalPlatformFields is only supported on Windows and macOS."
#endif
	};

	inline bool CliTerminalHasWritableInput(const CliTerminalPlatformFields& fields)
	{
#if defined(_WIN32)
		return fields.pipe_input != INVALID_HANDLE_VALUE;
#elif defined(__APPLE__)
		return fields.master_fd >= 0;
#else
#error "CliTerminalHasWritableInput is only supported on Windows and macOS."
#endif
	}

	struct OpenCodeBridgePlatformFields
	{
#if defined(_WIN32)
		HANDLE process_handle = INVALID_HANDLE_VALUE;
		HANDLE process_thread = INVALID_HANDLE_VALUE;
		DWORD process_id = 0;
#elif defined(__APPLE__)
		pid_t process_id = -1;
#else
#error "OpenCodeBridgePlatformFields is only supported on Windows and macOS."
#endif
	};

	struct AsyncNativeChatLoadTask
	{
		bool running = false;
		std::shared_ptr<std::atomic<bool>> completed;
		std::shared_ptr<std::vector<ChatSession>> chats;
		std::shared_ptr<std::string> error;
	};

} // namespace uam::platform

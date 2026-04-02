#pragma once

#include "common/app_models.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <wincontypes.h>
#else
#include <sys/types.h>
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
#else
		int master_fd = -1;
		pid_t child_pid = -1;
#endif
	};

	inline bool CliTerminalHasWritableInput(const CliTerminalPlatformFields& fields)
	{
#if defined(_WIN32)
		return fields.pipe_input != INVALID_HANDLE_VALUE;
#else
		return fields.master_fd >= 0;
#endif
	}

	struct OpenCodeBridgePlatformFields
	{
#if defined(_WIN32)
		HANDLE process_handle = INVALID_HANDLE_VALUE;
		HANDLE process_thread = INVALID_HANDLE_VALUE;
		DWORD process_id = 0;
#else
		pid_t process_id = -1;
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

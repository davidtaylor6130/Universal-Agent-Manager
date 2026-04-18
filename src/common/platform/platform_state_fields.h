#pragma once

#include "common/models/app_models.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
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
		HANDLE job_object = nullptr;
#elif defined(__APPLE__)
		int master_fd = -1;
		pid_t child_pid = -1;
#else
#error "CliTerminalPlatformFields is only supported on Windows and macOS."
#endif
	};

	struct StdioProcessPlatformFields
	{
#if defined(_WIN32)
		HANDLE stdin_write = INVALID_HANDLE_VALUE;
		HANDLE stdout_read = INVALID_HANDLE_VALUE;
		HANDLE stderr_read = INVALID_HANDLE_VALUE;
		PROCESS_INFORMATION process_info = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, 0, 0};
		HANDLE job_object = nullptr;
#elif defined(__APPLE__)
		int stdin_write_fd = -1;
		int stdout_read_fd = -1;
		int stderr_read_fd = -1;
		pid_t child_pid = -1;
#else
#error "StdioProcessPlatformFields is only supported on Windows and macOS."
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

		struct AsyncNativeChatLoadTask
		{
			bool running = false;
			std::string provider_id_snapshot;
			std::string chats_dir_snapshot;
			struct State
			{
				std::atomic<bool> completed{false};
				std::vector<ChatSession> chats;
				std::string snapshot_digest;
				std::string error;
			};
			std::shared_ptr<State> state;
			std::unique_ptr<std::jthread> worker;
		};

} // namespace uam::platform

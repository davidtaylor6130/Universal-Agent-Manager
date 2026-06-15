#pragma once

#include "common/models/app_models.h"

#include <atomic>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
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

	inline void ResetAsyncNativeChatLoadTask(AsyncNativeChatLoadTask& task)
	{
		if (task.worker != nullptr)
		{
			task.worker->request_stop();
			task.worker.reset();
		}

		task.running = false;
		task.provider_id_snapshot.clear();
		task.chats_dir_snapshot.clear();
		task.state.reset();
	}

	template <typename LoadChats, typename BuildDigest> inline bool StartAsyncNativeChatLoadTask(AsyncNativeChatLoadTask& task, std::string provider_id, const std::filesystem::path& chats_dir, LoadChats load_chats, BuildDigest build_digest)
	{
		if (task.running)
		{
			return false;
		}

		ResetAsyncNativeChatLoadTask(task);
		task.running = true;
		task.provider_id_snapshot = std::move(provider_id);
		task.chats_dir_snapshot = chats_dir.string();
		task.state = std::make_shared<AsyncNativeChatLoadTask::State>();
		std::shared_ptr<AsyncNativeChatLoadTask::State> state = task.state;

		task.worker = std::make_unique<std::jthread>(
		    [state, load_chats = std::move(load_chats), build_digest = std::move(build_digest)](std::stop_token stop_token) mutable
		    {
			    try
			    {
				    state->chats = load_chats(stop_token);
				    state->snapshot_digest = build_digest(state->chats);
			    }
			    catch (const std::exception& ex)
			    {
				    state->error = ex.what();
			    }
			    catch (...)
			    {
				    // Keep non-standard exceptions from escaping the background worker.
				    state->error = "Unknown native chat load failure.";
			    }

			    state->completed.store(true, std::memory_order_release);
		    });
		return true;
	}

	inline bool TryConsumeAsyncNativeChatLoadTask(AsyncNativeChatLoadTask& task, std::vector<ChatSession>& chats_out, std::string* digest_out, std::string& error_out)
	{
		if (!task.running)
		{
			return false;
		}

		if (task.state == nullptr)
		{
			ResetAsyncNativeChatLoadTask(task);
			chats_out.clear();
			if (digest_out != nullptr)
			{
				digest_out->clear();
			}
			error_out.clear();
			return true;
		}

		if (!task.state->completed.load(std::memory_order_acquire))
		{
			return false;
		}

		chats_out = std::move(task.state->chats);
		if (digest_out != nullptr)
		{
			*digest_out = std::move(task.state->snapshot_digest);
		}
		error_out = std::move(task.state->error);
		ResetAsyncNativeChatLoadTask(task);
		return true;
	}

	inline void ResetAsyncNativeChatLoadTasks(std::unordered_map<std::string, AsyncNativeChatLoadTask>& tasks)
	{
		for (auto& entry : tasks)
		{
			ResetAsyncNativeChatLoadTask(entry.second);
		}

		tasks.clear();
	}

} // namespace uam::platform

#pragma once

#include "common/models/app_models.h"
#include "common/platform/async_byte_writer.h"
#include "common/runtime/native_refresh_lifetime.h"

#include <atomic>
#include <array>
#include <cstdint>
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
#if defined(__MINGW32__)
using HPCON = HANDLE;
#endif
#elif defined(__APPLE__) || defined(__linux__)
#include <sys/types.h>
#else
#error "platform_state_fields.h is only supported on Windows, macOS, and Linux."
#endif

namespace uam::platform
{

	struct CliTerminalPlatformFields
	{
		std::shared_ptr<AsyncByteWriter> input_writer;
#if defined(_WIN32)
		HANDLE pipe_input = INVALID_HANDLE_VALUE;
		HANDLE pipe_output = INVALID_HANDLE_VALUE;
		PROCESS_INFORMATION process_info = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, 0, 0};
		HPCON pseudo_console = nullptr;
		std::unique_ptr<std::jthread> pseudo_console_closer;
		LPPROC_THREAD_ATTRIBUTE_LIST attr_list = nullptr;
		HANDLE job_object = nullptr;
#elif defined(__APPLE__) || defined(__linux__)
		int master_fd = -1;
		pid_t child_pid = -1;
		pid_t watchdog_pid = -1;
#else
#error "CliTerminalPlatformFields is only supported on Windows, macOS, and Linux."
#endif
	};

	struct StdioProcessPlatformFields
	{
		std::shared_ptr<AsyncByteWriter> stdin_writer;
#if defined(_WIN32)
		HANDLE stdin_write = INVALID_HANDLE_VALUE;
		HANDLE stdout_read = INVALID_HANDLE_VALUE;
		HANDLE stderr_read = INVALID_HANDLE_VALUE;
		PROCESS_INFORMATION process_info = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, 0, 0};
		HANDLE job_object = nullptr;
#elif defined(__APPLE__) || defined(__linux__)
		int stdin_write_fd = -1;
		int stdout_read_fd = -1;
		int stderr_read_fd = -1;
		pid_t child_pid = -1;
		pid_t watchdog_pid = -1;
		// macOS reaps before output drainage finishes; retain status with process ownership.
		int reaped_exit_code = -1;
#if defined(__linux__)
		// Deferred reap retains observable completion after callers reset their fields.
		std::shared_ptr<std::atomic<bool>> deferred_reap_complete;
#endif
#else
#error "StdioProcessPlatformFields is only supported on Windows, macOS, and Linux."
#endif
	};

	inline bool CliTerminalHasWritableInput(const CliTerminalPlatformFields& fields)
	{
#if defined(_WIN32)
		return fields.pipe_input != INVALID_HANDLE_VALUE;
#elif defined(__APPLE__) || defined(__linux__)
		return fields.master_fd >= 0;
#else
#error "CliTerminalHasWritableInput is only supported on Windows, macOS, and Linux."
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
			// The same dedicated parser-pool owner survives parse and media adoption.
			std::shared_ptr<void> retained_processing_payload;
			std::uint64_t admission_generation = 0;
			std::filesystem::path owner_root;
			bool worker_retirement_owned = false;
			// Retirement observation only, never native execution authority.
			// Explicit standalone readers remain valid until a real terminal has
			// consumed this root/provider/history-directory key.
			bool terminal_owner_observed = false;
			struct TerminalOwnerObservation
			{
				// Comparison-only identity, never dereferenced or logged.
				std::uintptr_t row_identity = 0;
				std::string terminal_id, chat_id, chat_created_at, provider_id;
				double startup_time_s = 0.0;
				std::weak_ptr<const std::uint8_t> chat_incarnation;
				std::array<std::string, 10> workspace;
			};
			std::vector<TerminalOwnerObservation> terminal_owners;
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

	template <typename LoadChats, typename BuildDigest> inline bool StartAsyncNativeChatLoadTask(AsyncNativeChatLoadTask& task, std::string provider_id, const std::filesystem::path& chats_dir, LoadChats load_chats, BuildDigest build_digest, bool worker_retirement = false)
	{
		if (task.running)
		{
			return false;
		}

		ResetAsyncNativeChatLoadTask(task);
		auto state_owner = worker_retirement ? uam::native_history::TryCreateWorkerOwnedNativeRefresh<AsyncNativeChatLoadTask::State, uam::native_history::NativeRefreshWorkerPool::TerminalParser>() : std::make_shared<AsyncNativeChatLoadTask::State>();
		if (!state_owner) return false; // Backpressure before parsing or retaining a payload.
		task.running = true;
		task.provider_id_snapshot = std::move(provider_id);
		task.chats_dir_snapshot = chats_dir.string();
		task.state = std::move(state_owner);
		task.state->worker_retirement_owned = worker_retirement;
		std::shared_ptr<AsyncNativeChatLoadTask::State> state = task.state;

		try
		{
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
		}
		catch (...)
		{
			ResetAsyncNativeChatLoadTask(task);
			return false;
		}
		return true;
	}

	inline bool TryConsumeAsyncNativeChatLoadTask(AsyncNativeChatLoadTask& task, std::vector<ChatSession>& chats_out, std::string* digest_out, std::string& error_out, std::shared_ptr<AsyncNativeChatLoadTask::State>* owner_out = nullptr)
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

		if (owner_out != nullptr) *owner_out = task.state;
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

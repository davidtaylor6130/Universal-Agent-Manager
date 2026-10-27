#pragma once

#include "test_harness.h"
#include <mutex>
#if !defined(_WIN32)
#include <fcntl.h>
#endif

namespace uam_test
{
	// An existing AsyncByteWriter sink, with no child, runner, SSH or provider process.
	// Captured bytes prove only the controller's write attempt, never native acceptance.
	struct InjectedAcpInput
	{
		struct State { std::mutex mutex; std::string bytes; };
		std::shared_ptr<State> state = std::make_shared<State>();
		std::shared_ptr<uam::platform::AsyncByteWriter> writer;

		void Attach(uam::AcpSessionState& session)
		{
#if defined(_WIN32)
			session.stdin_write = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
			UAM_ASSERT(session.stdin_write != INVALID_HANDLE_VALUE);
#else
			session.stdin_write_fd = open("/dev/null", O_WRONLY);
			UAM_ASSERT(session.stdin_write_fd >= 0);
#endif
			writer = std::make_shared<uam::platform::AsyncByteWriter>([captured = state](const char* bytes, std::size_t size, std::string&)
			{
				std::lock_guard lock(captured->mutex);
				captured->bytes.append(bytes, size);
				return static_cast<std::ptrdiff_t>(size);
			}, [] {});
			session.stdin_writer = writer;
			session.running = session.initialized = session.session_ready = true;
		}
		std::string Bytes() const
		{
			std::lock_guard lock(state->mutex);
			return state->bytes;
		}
	};
}

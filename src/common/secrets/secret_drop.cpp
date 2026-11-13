#include "common/secrets/secret_drop.h"

#include "common/secrets/secret_drop_internal.h"
#include "common/secrets/secret_redactor.h"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

// POSIX: the drop is a FIFO, so the value only ever passes through a kernel pipe buffer.
namespace uam::secrets
{
	namespace
	{
		using namespace drop_internal;

		bool PrivateDirectory(const std::filesystem::path& directory, std::string& error)
		{
			if (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
			{
				error = "The private secret folder could not be created.";
				return false;
			}
			struct stat status{};
			if (lstat(directory.c_str(), &status) != 0 || S_ISLNK(status.st_mode) || !S_ISDIR(status.st_mode) || status.st_uid != geteuid())
			{
				error = "The private secret folder must be a real folder owned by this user.";
				return false;
			}
			if ((status.st_mode & 07777) != 0700 && chmod(directory.c_str(), 0700) != 0)
			{
				error = "The private secret folder permissions could not be restricted.";
				return false;
			}
			return true;
		}

		bool WriteAll(int descriptor, const std::string& value)
		{
			std::size_t written = 0;
			while (written < value.size())
			{
				const ssize_t result = write(descriptor, value.data() + written, value.size() - written);
				if (result < 0 && errno == EINTR) continue;
				if (result <= 0) return false;
				written += static_cast<std::size_t>(result);
			}
			return true;
		}

		void ServeOnce(const std::string& path, std::string& value, std::chrono::steady_clock::time_point deadline, std::stop_token stop)
		{
			// A reader that closes early raises SIGPIPE on this thread only; blocked, write reports EPIPE instead.
			sigset_t pipe_signal;
			sigemptyset(&pipe_signal);
			sigaddset(&pipe_signal, SIGPIPE);
			(void)pthread_sigmask(SIG_BLOCK, &pipe_signal, nullptr);
			int descriptor = -1;
			while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
			{
				descriptor = open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
				if (descriptor >= 0 || (errno != ENXIO && errno != EINTR)) break; // ENXIO: no reader yet; anything else: the drop is gone.
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
			// A reader that started waiting during the last poll interval is served rather than left blocked.
			if (descriptor < 0) descriptor = open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
			// Unlink before writing: the reader already holds the pipe, and nobody else can open it afterwards.
			(void)unlink(path.c_str());
			if (descriptor >= 0)
			{
				const int flags = fcntl(descriptor, F_GETFL);
				struct stat status{};
				if (flags >= 0 && fcntl(descriptor, F_SETFL, flags & ~O_NONBLOCK) == 0 && fstat(descriptor, &status) == 0 && S_ISFIFO(status.st_mode))
					(void)WriteAll(descriptor, value);
				(void)close(descriptor);
			}
			Wipe(value);
		}
	}

	DropResult Drop(const std::filesystem::path& directory, std::string_view value, std::chrono::seconds ttl)
	{
		DropResult result;
		if (value.empty() || value.size() > kMaxSecretBytes)
		{
			result.error = "The secret is empty or larger than 8 KB.";
			return result;
		}
		if (!PrivateDirectory(directory, result.error)) return result;
		const std::string path = (directory / RandomDropName()).string();
		struct stat status{};
		if (mkfifo(path.c_str(), 0600) != 0 || lstat(path.c_str(), &status) != 0 || !S_ISFIFO(status.st_mode) ||
		    status.st_uid != geteuid() || (status.st_mode & 07777) != 0600)
		{
			(void)unlink(path.c_str());
			result.error = "The private secret handoff could not be created.";
			return result;
		}
		try
		{
			StartWriter([path, secret = std::string(value), deadline = std::chrono::steady_clock::now() + ttl](std::stop_token stop) mutable
			{
				ServeOnce(path, secret, deadline, stop);
			});
		}
		catch (const std::system_error&)
		{
			(void)unlink(path.c_str());
			result.error = "The private secret handoff could not start.";
			return result;
		}
		result.ok = true;
		result.path = path;
		return result;
	}

	std::size_t SweepStale(const std::filesystem::path& directory)
	{
		return SweepDropDirectory(directory);
	}

	void StopAllDrops()
	{
		StopAllWriters();
	}
	LocalDropKind LocalDrop()
	{
		// The desktop runs on macOS; Linux builds this file only for the SSH helper, which never shows a card.
		return {kDropTtl, true, "macos"};
	}
}

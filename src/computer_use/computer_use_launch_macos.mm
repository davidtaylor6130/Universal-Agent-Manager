#include "computer_use/computer_use_launch_macos.h"
#include "computer_use/computer_use_install_macos.h"

#include "computer_use/computer_use_mcp_config.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

extern char** environ;

namespace uam::computer_use
{
	namespace
	{
		constexpr std::string_view kSocketArgument = "--uam-computer-use-socket";
		constexpr auto kTimeout = std::chrono::seconds(10);

		std::string SocketArgument(const std::vector<std::string>& arguments)
		{
			for (std::size_t i = 0; i + 1 < arguments.size(); ++i)
				if (arguments[i] == kSocketArgument)
					return arguments[i + 1];
			return {};
		}

		bool SetNonBlocking(int fd)
		{
			const int flags = fcntl(fd, F_GETFL, 0);
			return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
		}

		bool SameUser(int fd)
		{
			uid_t uid = 0;
			gid_t gid = 0;
			return getpeereid(fd, &uid, &gid) == 0 && uid == geteuid();
		}

		class SocketDirectory
		{
		public:
			~SocketDirectory()
			{
				if (!m_socket.empty()) unlink(m_socket.c_str());
				if (!m_directory.empty()) rmdir(m_directory.c_str());
			}

			std::string m_directory;
			std::string m_socket;
		};

		bool WaitForExit(pid_t pid)
		{
			const auto deadline = std::chrono::steady_clock::now() + kTimeout;
			int status = 0;
			while (std::chrono::steady_clock::now() < deadline)
			{
				const pid_t result = waitpid(pid, &status, WNOHANG);
				if (result == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
				if (result < 0 && errno != EINTR) return false;
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			kill(pid, SIGKILL);
			(void)waitpid(pid, &status, 0);
			return false;
		}

		bool Relay(int socket_fd)
		{
			if (!SetNonBlocking(STDIN_FILENO) || !SetNonBlocking(STDOUT_FILENO) || !SetNonBlocking(socket_fd)) return false;
			signal(SIGPIPE, SIG_IGN);
			std::string to_socket, to_stdout;
			bool stdin_closed = false, socket_closed = false;
			std::array<char, 64 * 1024> buffer{};

			while (true)
			{
				if (!stdin_closed && to_socket.empty())
				{
					const ssize_t count = read(STDIN_FILENO, buffer.data(), buffer.size());
					if (count > 0) to_socket.assign(buffer.data(), static_cast<std::size_t>(count));
					else if (count == 0) { shutdown(socket_fd, SHUT_WR); stdin_closed = true; }
					else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
				}

				pollfd fds[] = {
				    {stdin_closed || !to_socket.empty() ? -1 : STDIN_FILENO, POLLIN, 0},
				    {socket_closed || (!to_stdout.empty() && to_socket.empty()) ? -1 : socket_fd, static_cast<short>((to_stdout.empty() ? POLLIN : 0) | (!to_socket.empty() ? POLLOUT : 0)), 0},
				    {to_stdout.empty() ? -1 : STDOUT_FILENO, POLLOUT, 0},
				};
				const int ready = poll(fds, 3, 10000);
				if (ready < 0) { if (errno == EINTR) continue; return false; }
				if (ready == 0) continue;

				if ((fds[2].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) return false;
				if (to_stdout.empty() && (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
				{
					const ssize_t count = read(socket_fd, buffer.data(), buffer.size());
					if (count > 0) to_stdout.append(buffer.data(), static_cast<std::size_t>(count));
					else if (count == 0) socket_closed = true;
					else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
				}
				if ((fds[1].revents & POLLOUT) != 0 && !to_socket.empty())
				{
					const ssize_t count = write(socket_fd, to_socket.data(), to_socket.size());
					if (count > 0) to_socket.erase(0, static_cast<std::size_t>(count));
					else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
				}
				if ((fds[2].revents & POLLOUT) != 0 && !to_stdout.empty())
				{
					const ssize_t count = write(STDOUT_FILENO, to_stdout.data(), to_stdout.size());
					if (count > 0) to_stdout.erase(0, static_cast<std::size_t>(count));
					else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
				}
				if (socket_closed && to_stdout.empty()) break;
			}
			return to_stdout.empty();
		}
	} // namespace

	bool AttachIndependentComputerUseSocket(const std::vector<std::string>& arguments, std::string* error)
	{
		const std::string path = SocketArgument(arguments);
		if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path))
		{
			if (error) *error = "Missing or invalid Computer Use socket path.";
			return false;
		}
		struct stat socket_stat{};
		if (lstat(path.c_str(), &socket_stat) != 0 || !S_ISSOCK(socket_stat.st_mode) || socket_stat.st_uid != geteuid() || (socket_stat.st_mode & 0777) != 0600)
		{
			if (error) *error = "Invalid Computer Use socket.";
			return false;
		}
		const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0) { if (error) *error = std::strerror(errno); return false; }
		sockaddr_un address{};
		address.sun_family = AF_UNIX;
		std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
		if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || !SameUser(fd))
		{
			if (error) *error = "Could not connect to the Computer Use parent.";
			close(fd);
			return false;
		}
		if (dup2(fd, STDIN_FILENO) < 0 || dup2(fd, STDOUT_FILENO) < 0)
		{
			if (error) *error = "Could not attach Computer Use stdio.";
			close(fd);
			return false;
		}
		if (fd > STDERR_FILENO) close(fd);
		return true;
	}

	int RunIndependentComputerUse(const std::vector<std::string>& arguments)
	{
		SocketDirectory temporary;
		std::array<char, 32> directory_template{};
		std::strncpy(directory_template.data(), "/tmp/uam-cu-XXXXXX", directory_template.size() - 1);
		char* directory = mkdtemp(directory_template.data());
		if (!directory) return 1;
		temporary.m_directory = directory;
		temporary.m_socket = temporary.m_directory + "/ipc";
		const int listener = socket(AF_UNIX, SOCK_STREAM, 0);
		if (listener < 0) return 1;
		chmod(temporary.m_directory.c_str(), 0700);
		sockaddr_un address{};
		address.sun_family = AF_UNIX;
		std::strncpy(address.sun_path, temporary.m_socket.c_str(), sizeof(address.sun_path) - 1);
		if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || chmod(temporary.m_socket.c_str(), 0600) != 0 || listen(listener, 1) != 0)
		{
			close(listener);
			return 1;
		}

		const std::filesystem::path executable = McpExecutablePath();
		std::filesystem::path app = executable.parent_path().parent_path().parent_path();
		if (app.filename() != "UAM Computer Use.app" || !std::filesystem::is_directory(app))
		{
			close(listener);
			return 1;
		}
		std::string install_error;
		app = PrepareStandaloneComputerUse(app, &install_error);
		if (app.empty())
		{
			std::fprintf(stderr, "%s\n", install_error.c_str());
			close(listener);
			return 1;
		}
		std::vector<std::string> launch = {"/usr/bin/open", "-n", "-g", "-a", app.string()};
		if (const char* data_root = std::getenv("UAM_DATA_DIR"); data_root != nullptr && *data_root != '\0')
		{
			launch.push_back("--env");
			launch.push_back(std::string("UAM_DATA_DIR=") + data_root);
		}
		launch.push_back("--args");
		for (std::size_t i = 1; i < arguments.size(); ++i) launch.push_back(arguments[i]);
		launch.push_back(std::string(kSocketArgument));
		launch.push_back(temporary.m_socket);
		std::vector<char*> argv;
		for (auto& item : launch) argv.push_back(item.data());
		argv.push_back(nullptr);
		pid_t pid = 0;
		if (posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0 || !WaitForExit(pid))
		{
			close(listener);
			return 1;
		}
		SetNonBlocking(listener);
		const auto deadline = std::chrono::steady_clock::now() + kTimeout;
		int client = -1;
		while (std::chrono::steady_clock::now() < deadline)
		{
			pollfd poll_fd{listener, POLLIN, 0};
			const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
			if (poll(&poll_fd, 1, static_cast<int>(remaining.count())) <= 0) continue;
			client = accept(listener, nullptr, nullptr);
			if (client >= 0) break;
			if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) break;
		}
		close(listener);
		if (client < 0 || !SameUser(client)) { if (client >= 0) close(client); return 1; }
		const bool relayed = Relay(client);
		close(client);
		return relayed ? 0 : 1;
	}
} // namespace uam::computer_use

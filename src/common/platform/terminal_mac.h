#pragma once

#if defined(__APPLE__)
#include <cstdlib>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <util.h>

static bool StartCliTerminalMac(AppState& app, CliTerminalState& terminal, const ChatSession& chat)
{
	const fs::path workspace_root = ResolveWorkspaceRootPath(app, chat);
	int master_fd = -1;
	int slave_fd = -1;

	struct winsize ws
	{
	};

	ws.ws_row = static_cast<unsigned short>(terminal.rows);
	ws.ws_col = static_cast<unsigned short>(terminal.cols);

	if (openpty(&master_fd, &slave_fd, nullptr, nullptr, &ws) != 0)
	{
		terminal.last_error = "openpty failed.";
		return false;
	}

	const pid_t pid = fork();

	if (pid < 0)
	{
		terminal.last_error = "fork failed.";
		close(master_fd);
		close(slave_fd);
		return false;
	}

	if (pid == 0)
	{
		setsid();
		ioctl(slave_fd, TIOCSCTTY, 0);
		dup2(slave_fd, STDIN_FILENO);
		dup2(slave_fd, STDOUT_FILENO);
		dup2(slave_fd, STDERR_FILENO);
		close(master_fd);
		close(slave_fd);

		if (!workspace_root.empty())
		{
			std::error_code ec;
			fs::create_directories(workspace_root, ec);
			chdir(workspace_root.c_str());
		}

		setenv("TERM", "xterm-256color", 1);
		const std::vector<std::string> argv_vec = BuildProviderInteractiveArgv(app, chat);
		std::vector<char*> argv_ptrs;
		argv_ptrs.reserve(argv_vec.size() + 1);

		for (const std::string& arg : argv_vec)
		{
			argv_ptrs.push_back(const_cast<char*>(arg.c_str()));
		}

		argv_ptrs.push_back(nullptr);
		execvp(argv_ptrs[0], argv_ptrs.data());
		_exit(127);
	}

	close(slave_fd);
	terminal.master_fd = master_fd;
	terminal.child_pid = pid;
	const int flags = fcntl(terminal.master_fd, F_GETFL, 0);

	if (flags >= 0)
	{
		(void)fcntl(terminal.master_fd, F_SETFL, flags | O_NONBLOCK);
	}

	return true;
}

#else
#error "terminal_mac.h is only supported on macOS builds."
#endif

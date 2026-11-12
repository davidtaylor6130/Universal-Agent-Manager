#include "companion_network_platform.h"
#include <algorithm>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#pragma comment(lib, "iphlpapi.lib")
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#endif
#include <vector>

namespace uam::platform
{
	bool ProbeCompanionTcpListener(const std::string& ipv4, int port, std::chrono::milliseconds timeout)
	{
		if (port < 1 || port > 65535 || timeout.count() <= 0 || ipv4.find('\0') != std::string::npos) return false;
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(static_cast<unsigned short>(port));
#if defined(_WIN32)
		WSADATA data{};
		if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
		struct StartupGuard { ~StartupGuard() { WSACleanup(); } } startup;
#endif
		if (inet_pton(AF_INET, ipv4.c_str(), &address.sin_addr) != 1) return false;
		const auto socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#if defined(_WIN32)
		if (socket_fd == INVALID_SOCKET) return false;
		struct SocketGuard { SOCKET fd; ~SocketGuard() { closesocket(fd); } } guard{socket_fd};
		u_long nonblocking = 1;
		if (ioctlsocket(socket_fd, FIONBIO, &nonblocking) != 0) return false;
#else
		if (socket_fd < 0) return false;
		struct SocketGuard { int fd; ~SocketGuard() { close(fd); } } guard{socket_fd};
		if (socket_fd >= FD_SETSIZE) return false;
		const int flags = fcntl(socket_fd, F_GETFL, 0);
		if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) != 0) return false;
#endif
		if (connect(socket_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) return true;
#if defined(_WIN32)
		if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
#else
		if (errno != EINPROGRESS) return false;
#endif
		fd_set writable;
		FD_ZERO(&writable);
		FD_SET(socket_fd, &writable);
		const auto bounded = std::min(timeout, std::chrono::milliseconds(200));
		timeval wait{};
		wait.tv_usec = static_cast<decltype(wait.tv_usec)>(bounded.count() * 1000);
		if (select(static_cast<int>(socket_fd + 1), nullptr, &writable, nullptr, &wait) <= 0) return false;
		int error = 0;
#if defined(_WIN32)
		int length = sizeof(error);
		return getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == 0 && error == 0;
#else
		socklen_t length = sizeof(error);
		return getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
#endif
	}

	std::optional<bool> LoopbackClientDescendsFromThisProcess(int client_port, int server_port, std::int64_t excluded_pid)
	{
		if (client_port < 1 || client_port > 65535 || server_port < 1 || server_port > 65535) return std::nullopt;
		std::unordered_map<std::int64_t, std::int64_t> parents;
#if defined(__APPLE__)
		const int estimate = proc_listallpids(nullptr, 0);
		if (estimate <= 0) return std::nullopt;
		std::vector<pid_t> pids(static_cast<std::size_t>(estimate) + 64);
		const int listed = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
		if (listed <= 0) return std::nullopt;
		pids.resize(std::min(pids.size(), static_cast<std::size_t>(listed)));
		for (const pid_t pid : pids)
		{
			proc_bsdinfo info{};
			// Other users' processes are unreadable and cannot be this process's children anyway.
			if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == static_cast<int>(sizeof(info))) parents[pid] = info.pbi_ppid;
		}
		const auto owns_client = [&](std::int64_t pid)
		{
			const int bytes = proc_pidinfo(static_cast<pid_t>(pid), PROC_PIDLISTFDS, 0, nullptr, 0);
			if (bytes <= 0) return false;
			std::vector<proc_fdinfo> fds(static_cast<std::size_t>(bytes) / sizeof(proc_fdinfo) + 16);
			const int filled = proc_pidinfo(static_cast<pid_t>(pid), PROC_PIDLISTFDS, 0, fds.data(), static_cast<int>(fds.size() * sizeof(proc_fdinfo)));
			if (filled <= 0) return false;
			fds.resize(std::min(fds.size(), static_cast<std::size_t>(filled) / sizeof(proc_fdinfo)));
			for (const proc_fdinfo& fd : fds)
			{
				if (fd.proc_fdtype != PROX_FDTYPE_SOCKET) continue;
				socket_fdinfo socket_info{};
				if (proc_pidfdinfo(static_cast<pid_t>(pid), fd.proc_fd, PROC_PIDFDSOCKETINFO, &socket_info, sizeof(socket_info)) != static_cast<int>(sizeof(socket_info)) ||
				    socket_info.psi.soi_kind != SOCKINFO_TCP) continue;
				const in_sockinfo& endpoint = socket_info.psi.soi_proto.pri_tcp.tcpsi_ini;
				if (ntohs(static_cast<std::uint16_t>(endpoint.insi_lport)) == client_port && ntohs(static_cast<std::uint16_t>(endpoint.insi_fport)) == server_port) return true;
			}
			return false;
		};
		// Phone traffic arrives through the proxy; settle that common case with one process's sockets.
		if (excluded_pid > 0 && owns_client(excluded_pid)) return false;
		const std::int64_t self = getpid();
		for (const auto& [pid, parent] : parents)
		{
			(void)parent;
			if (DescendsFrom(pid, self, excluded_pid, parents) && owns_client(pid)) return true;
		}
		return false;
#elif defined(_WIN32)
		std::vector<char> buffer;
		ULONG size = 0;
		DWORD status = ERROR_INSUFFICIENT_BUFFER;
		for (int attempt = 0; attempt < 3 && status == ERROR_INSUFFICIENT_BUFFER; ++attempt)
		{
			buffer.resize(size + 4096);
			size = static_cast<ULONG>(buffer.size());
			status = GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
		}
		if (status != NO_ERROR) return std::nullopt;
		const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
		std::int64_t owner = 0;
		for (DWORD index = 0; index < table->dwNumEntries; ++index)
		{
			const MIB_TCPROW_OWNER_PID& row = table->table[index];
			if (ntohs(static_cast<u_short>(row.dwLocalPort)) == client_port && ntohs(static_cast<u_short>(row.dwRemotePort)) == server_port) owner = row.dwOwningPid;
		}
		if (owner == 0) return std::nullopt;
		const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;
		PROCESSENTRY32 entry{};
		entry.dwSize = sizeof(entry);
		for (BOOL more = Process32First(snapshot, &entry); more; more = Process32Next(snapshot, &entry)) parents[entry.th32ProcessID] = entry.th32ParentProcessID;
		CloseHandle(snapshot);
		return DescendsFrom(owner, static_cast<std::int64_t>(GetCurrentProcessId()), excluded_pid, parents);
#else
		(void)excluded_pid;
		return std::nullopt;
#endif
	}
}

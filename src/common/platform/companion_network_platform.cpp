#include "companion_network_platform.h"
#include <algorithm>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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
}

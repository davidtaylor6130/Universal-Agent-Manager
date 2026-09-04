#include "remote/runner_service_posix.h"

#include "remote/runner_protocol.h"
#include "remote/runner_state.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace uam::remote
{
	namespace
	{
		class Socket
		{
		  public:
			explicit Socket(int descriptor = -1) : m_descriptor(descriptor) {}
			~Socket()
			{
				if (m_descriptor >= 0) (void)close(m_descriptor);
			}
			Socket(const Socket&) = delete;
			Socket& operator=(const Socket&) = delete;
			int Get() const { return m_descriptor; }

		  private:
			int m_descriptor;
		};

		class SocketPathCleanup
		{
		  public:
			explicit SocketPathCleanup(std::filesystem::path path) : m_path(std::move(path)) {}
			~SocketPathCleanup() { (void)unlink(m_path.c_str()); }

		  private:
			std::filesystem::path m_path;
		};

		class ServiceLock
		{
		  public:
			explicit ServiceLock(int descriptor = -1) : m_descriptor(descriptor) {}
			~ServiceLock()
			{
				if (m_descriptor >= 0) (void)close(m_descriptor);
			}
			ServiceLock(const ServiceLock&) = delete;
			ServiceLock& operator=(const ServiceLock&) = delete;
			ServiceLock(ServiceLock&& other) noexcept
			    : m_descriptor(std::exchange(other.m_descriptor, -1))
			{
			}
			int Get() const { return m_descriptor; }

		  private:
			int m_descriptor;
		};

		struct ClientThread
		{
			std::shared_ptr<std::atomic<bool>> done;
			std::jthread thread;
		};

		bool SocketAddress(const std::filesystem::path& path, sockaddr_un& address,
		                   std::string& error)
		{
			const std::string value = path.string();
			if (!path.is_absolute() || value.empty() || value.size() >= sizeof(address.sun_path))
			{
				error = "Runner socket path must be an absolute path shorter than " +
				        std::to_string(sizeof(address.sun_path)) + " bytes.";
				return false;
			}
			address = {};
			address.sun_family = AF_UNIX;
			std::memcpy(address.sun_path, value.c_str(), value.size() + 1);
			return true;
		}

		bool SetNoSigPipe(int descriptor)
		{
#if defined(__APPLE__)
			const int enabled = 1;
			return setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
			                  sizeof(enabled)) == 0;
#else
			(void)descriptor;
			return true;
#endif
		}

		bool ReadExact(int descriptor, char* bytes, std::size_t size)
		{
			std::size_t offset = 0;
			while (offset < size)
			{
				const ssize_t read_size = read(descriptor, bytes + offset, size - offset);
				if (read_size > 0)
				{
					offset += static_cast<std::size_t>(read_size);
					continue;
				}
				if (read_size < 0 && errno == EINTR) continue;
				return false;
			}
			return true;
		}

		bool WriteExact(int descriptor, const char* bytes, std::size_t size)
		{
			std::size_t offset = 0;
			while (offset < size)
			{
				const ssize_t written = write(descriptor, bytes + offset, size - offset);
				if (written > 0)
				{
					offset += static_cast<std::size_t>(written);
					continue;
				}
				if (written < 0 && errno == EINTR) continue;
				return false;
			}
			return true;
		}

		FrameReadResult ReadSocketFrame(int descriptor, nlohmann::json& message,
		                                std::string& error)
		{
			unsigned char header[4] = {};
			ssize_t first_read = read(descriptor, header, sizeof(header));
			if (first_read == 0) return FrameReadResult::EndOfStream;
			if (first_read < 0 && errno == EINTR)
				return ReadSocketFrame(descriptor, message, error);
			if (first_read < 0 ||
			    (first_read < static_cast<ssize_t>(sizeof(header)) &&
			     !ReadExact(descriptor, reinterpret_cast<char*>(header) + first_read,
			                sizeof(header) - static_cast<std::size_t>(first_read))))
			{
				error = "Runner socket frame header is truncated.";
				return FrameReadResult::Error;
			}
			const std::uint32_t size = (static_cast<std::uint32_t>(header[0]) << 24U) |
			                           (static_cast<std::uint32_t>(header[1]) << 16U) |
			                           (static_cast<std::uint32_t>(header[2]) << 8U) |
			                           static_cast<std::uint32_t>(header[3]);
			if (size == 0 || size > kMaxRunnerFrameBytes)
			{
				error = "Runner socket frame size is invalid.";
				return FrameReadResult::Error;
			}
			std::string body(size, '\0');
			if (!ReadExact(descriptor, body.data(), body.size()))
			{
				error = "Runner socket frame body is truncated.";
				return FrameReadResult::Error;
			}
			message = nlohmann::json::parse(body, nullptr, false);
			if (!message.is_object())
			{
				error = "Runner socket frame must contain one JSON object.";
				return FrameReadResult::Error;
			}
			return FrameReadResult::Ok;
		}

		bool WriteSocketFrame(int descriptor, const nlohmann::json& message)
		{
			const std::string body = message.dump();
			if (body.empty() || body.size() > kMaxRunnerFrameBytes) return false;
			const std::uint32_t size = static_cast<std::uint32_t>(body.size());
			const unsigned char header[4] = {
			    static_cast<unsigned char>((size >> 24U) & 0xffU),
			    static_cast<unsigned char>((size >> 16U) & 0xffU),
			    static_cast<unsigned char>((size >> 8U) & 0xffU),
			    static_cast<unsigned char>(size & 0xffU),
			};
			return WriteExact(descriptor, reinterpret_cast<const char*>(header), sizeof(header)) &&
			       WriteExact(descriptor, body.data(), body.size());
		}

		bool IsAuthorizedPeer(int descriptor)
		{
#if defined(__APPLE__)
			uid_t effective_user = 0;
			gid_t effective_group = 0;
			return getpeereid(descriptor, &effective_user, &effective_group) == 0 &&
			       effective_user == geteuid();
#else
			struct ucred credentials
			{
			};
			socklen_t size = sizeof(credentials);
			return getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 &&
			       credentials.uid == geteuid();
#endif
		}

		bool PrepareSocketPath(const std::filesystem::path& path, const sockaddr_un& address,
		                       std::string& error)
		{
			struct stat status
			{
			};
			if (lstat(path.c_str(), &status) != 0)
				return errno == ENOENT;
			if (!S_ISSOCK(status.st_mode) || status.st_uid != geteuid())
			{
				error = "Runner socket path exists but is not a socket owned by this user.";
				return false;
			}
			Socket probe(socket(AF_UNIX, SOCK_STREAM, 0));
			if (probe.Get() >= 0 && connect(probe.Get(), reinterpret_cast<const sockaddr*>(&address),
			                                sizeof(address)) == 0)
			{
				error = "A runner service is already listening on this socket.";
				return false;
			}
			if (unlink(path.c_str()) != 0)
			{
				error = "The stale runner socket could not be removed.";
				return false;
			}
			return true;
		}

		bool SocketIsReachable(const std::filesystem::path& socket_path)
		{
			std::string error;
			sockaddr_un address{};
			if (!SocketAddress(socket_path, address, error)) return false;
			Socket probe(socket(AF_UNIX, SOCK_STREAM, 0));
			return probe.Get() >= 0 &&
			       connect(probe.Get(), reinterpret_cast<const sockaddr*>(&address),
			               sizeof(address)) == 0;
		}

		ServiceLock AcquireServiceLock(const std::filesystem::path& socket_path,
		                               bool& already_running, std::string& error)
		{
			already_running = false;
			const std::filesystem::path lock_path = socket_path.string() + ".lock";
			const int descriptor = open(lock_path.c_str(),
			                            O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
			if (descriptor < 0)
			{
				error = "Runner service lock could not be opened.";
				return ServiceLock();
			}
			ServiceLock service_lock(descriptor);
			struct stat status
			{
			};
			if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
			    status.st_uid != geteuid() || fchmod(descriptor, 0600) != 0)
			{
				error = "Runner service lock must be a private file owned by this user.";
				return ServiceLock();
			}
			if (flock(descriptor, LOCK_EX | LOCK_NB) != 0)
			{
				if (errno == EWOULDBLOCK || errno == EAGAIN)
				{
					already_running = true;
					return ServiceLock();
				}
				error = "Runner service lock could not be acquired.";
				return ServiceLock();
			}
			return service_lock;
		}
	}

	int RunRunnerService(const std::filesystem::path& socket_path,
	                     std::string_view runner_version)
	{
		std::string error;
		sockaddr_un address{};
		if (!SocketAddress(socket_path, address, error))
		{
			std::cerr << error << '\n';
			return 2;
		}
		bool already_running = false;
		ServiceLock service_lock = AcquireServiceLock(socket_path, already_running, error);
		if (already_running) return 0;
		if (service_lock.Get() < 0 ||
		    !PrepareSocketPath(socket_path, address, error))
		{
			std::cerr << error << '\n';
			return 2;
		}
		Socket listener(socket(AF_UNIX, SOCK_STREAM, 0));
		if (listener.Get() < 0 || !SetNoSigPipe(listener.Get()))
		{
			std::cerr << "Runner service socket could not be created.\n";
			return 2;
		}
		const mode_t previous_mask = umask(0077);
		const int bind_result = bind(listener.Get(), reinterpret_cast<const sockaddr*>(&address),
		                             sizeof(address));
		const int bind_error = errno;
		(void)umask(previous_mask);
		if (bind_result != 0)
		{
			std::cerr << "Runner service socket could not bind: " << std::strerror(bind_error)
			          << ".\n";
			return 2;
		}
		SocketPathCleanup socket_cleanup(socket_path);
		if (chmod(socket_path.c_str(), 0600) != 0 || listen(listener.Get(), 4) != 0)
		{
			std::cerr << "Runner service socket could not listen: " << std::strerror(errno)
			          << ".\n";
			return 2;
		}

		RunnerState state;
		std::atomic<bool> shutdown_requested{false};
		std::mutex dispatch_mutex;
		std::vector<ClientThread> clients;
		while (!shutdown_requested.load(std::memory_order_acquire))
		{
			const int descriptor = accept(listener.Get(), nullptr, nullptr);
			if (descriptor < 0)
			{
				if (shutdown_requested.load(std::memory_order_acquire)) break;
				if (errno == EINTR) continue;
				std::cerr << "Runner service could not accept a bridge.\n";
				return 2;
			}
			std::erase_if(clients, [](const ClientThread& client)
			{
				return client.done->load(std::memory_order_acquire);
			});
			auto done = std::make_shared<std::atomic<bool>>(false);
			clients.push_back({done, std::jthread(
			    [&, descriptor, done](std::stop_token stop_token)
			    {
				    Socket client(descriptor);
				    std::stop_callback stop_callback(stop_token, [descriptor]
				    {
					    (void)shutdown(descriptor, SHUT_RDWR);
				    });
				    if (SetNoSigPipe(client.Get()) && IsAuthorizedPeer(client.Get()))
				    {
					    for (;;)
					    {
						    nlohmann::json request;
						    std::string client_error;
						    const FrameReadResult result = ReadSocketFrame(
						        client.Get(), request, client_error);
						    if (result != FrameReadResult::Ok) break;
						    if (request.value("type", "") == "service.shutdown")
						    {
							    bool busy = false;
							    {
								    std::scoped_lock dispatch_lock(dispatch_mutex);
								    busy = state.HasManagedProcesses();
								    if (!busy)
									    shutdown_requested.store(true,
									                             std::memory_order_release);
							    }
							    if (busy)
							    {
								    (void)WriteSocketFrame(client.Get(),
								        {{"id", request.value("id", "")},
								         {"type", "service.shutdown"}, {"ok", false},
								         {"error", {{"code", "processes_active"},
								                    {"message", "Runner service still owns remote processes."}}}});
								    continue;
							    }
							    (void)WriteSocketFrame(client.Get(),
							        {{"id", request.value("id", "")},
							         {"type", "service.shutdown"}, {"ok", true},
							         {"result", nlohmann::json::object()}});
							    (void)shutdown(listener.Get(), SHUT_RDWR);
							    break;
						    }
						    nlohmann::json response;
						    if (request.value("type", "") == "process.start")
						    {
							    std::scoped_lock dispatch_lock(dispatch_mutex);
							    response = shutdown_requested.load(std::memory_order_acquire)
							        ? nlohmann::json{{"id", request.value("id", "")},
							                         {"type", "error"}, {"ok", false},
							                         {"error", {{"code", "service_stopping"},
							                                    {"message", "Runner service is stopping."}}}}
							        : HandleRunnerRequest(request, runner_version, &state);
						    }
						    else
							    response = HandleRunnerRequest(request, runner_version, &state);
						    if (!WriteSocketFrame(client.Get(), response)) break;
					    }
				    }
				    done->store(true, std::memory_order_release);
			    })});
		}
		for (ClientThread& client : clients) client.thread.request_stop();
		return 0;
	}

	int StartRunnerService(const std::filesystem::path& socket_path,
	                       std::string_view runner_version)
	{
		if (SocketIsReachable(socket_path)) return 0;
		const std::filesystem::path directory = socket_path.parent_path();
		struct stat status
		{
		};
		if (directory.empty() || stat(directory.c_str(), &status) != 0 ||
		    !S_ISDIR(status.st_mode) || status.st_uid != geteuid())
		{
			std::cerr << "Runner state directory must exist and be owned by this user.\n";
			return 2;
		}

		const pid_t child = fork();
		if (child < 0)
		{
			std::cerr << "Runner service could not be started.\n";
			return 2;
		}
		if (child == 0)
		{
			if (setsid() < 0) _exit(2);
			const int input = open("/dev/null", O_RDONLY);
			const int log = open((directory / "runner.log").c_str(),
			                     O_WRONLY | O_CREAT | O_APPEND, 0600);
			if (input < 0 || log < 0) _exit(2);
			(void)dup2(input, STDIN_FILENO);
			(void)dup2(log, STDOUT_FILENO);
			(void)dup2(log, STDERR_FILENO);
			if (input > STDERR_FILENO) (void)close(input);
			if (log > STDERR_FILENO) (void)close(log);
			if (chdir("/") != 0) _exit(2);
			const int result = RunRunnerService(socket_path, runner_version);
			_exit(result);
		}

		bool child_reaped = false;
		for (int attempt = 0; attempt < 200; ++attempt)
		{
			if (SocketIsReachable(socket_path)) return 0;
			if (!child_reaped)
			{
				int status_value = 0;
				if (waitpid(child, &status_value, WNOHANG) == child)
				{
					child_reaped = true;
					if (!WIFEXITED(status_value) || WEXITSTATUS(status_value) != 0)
					{
						std::cerr << "Runner service exited during startup.\n";
						return 2;
					}
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		std::cerr << "Runner service did not become ready within ten seconds.\n";
		return 2;
	}

	int StopRunnerService(const std::filesystem::path& socket_path)
	{
		std::string error;
		sockaddr_un address{};
		if (!SocketAddress(socket_path, address, error)) return 2;
		Socket service(socket(AF_UNIX, SOCK_STREAM, 0));
		if (service.Get() < 0 || !SetNoSigPipe(service.Get()) ||
		    connect(service.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
			return 0;
		if (!WriteSocketFrame(service.Get(),
		                      {{"id", "stop"}, {"type", "service.shutdown"}}))
			return 2;
		nlohmann::json response;
		return ReadSocketFrame(service.Get(), response, error) == FrameReadResult::Ok &&
		               response.value("ok", false)
		           ? 0
		           : 2;
	}

	int RunRunnerBridge(const std::filesystem::path& socket_path)
	{
		std::string error;
		sockaddr_un address{};
		if (!SocketAddress(socket_path, address, error))
		{
			std::cerr << error << '\n';
			return 2;
		}
		Socket service(socket(AF_UNIX, SOCK_STREAM, 0));
		if (service.Get() < 0 || !SetNoSigPipe(service.Get()) ||
		    connect(service.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
		{
			std::cerr << "Runner service is unavailable.\n";
			return 2;
		}
		for (;;)
		{
			nlohmann::json request;
			const FrameReadResult input_result = ReadFrame(std::cin, request, &error);
			if (input_result == FrameReadResult::EndOfStream) return 0;
			if (input_result == FrameReadResult::Error ||
			    !WriteSocketFrame(service.Get(), request))
			{
				std::cerr << (error.empty() ? "Runner bridge request failed." : error) << '\n';
				return 2;
			}
			nlohmann::json response;
			if (ReadSocketFrame(service.Get(), response, error) != FrameReadResult::Ok ||
			    !WriteFrame(std::cout, response, &error))
			{
				std::cerr << (error.empty() ? "Runner bridge response failed." : error) << '\n';
				return 2;
			}
		}
	}
}

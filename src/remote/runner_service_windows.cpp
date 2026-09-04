#include "remote/runner_service_windows.h"

#include "common/utils/hash_utils.h"
#include "remote/runner_protocol.h"
#include "remote/runner_state.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <sddl.h>
#include <windows.h>

namespace uam::remote
{
	namespace
	{
		class Handle
		{
		  public:
			explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : m_value(value) {}
			~Handle() { if (m_value != INVALID_HANDLE_VALUE) CloseHandle(m_value); }
			Handle(const Handle&) = delete;
			Handle& operator=(const Handle&) = delete;
			HANDLE Get() const { return m_value; }
			HANDLE Release() { return std::exchange(m_value, INVALID_HANDLE_VALUE); }
		  private:
			HANDLE m_value;
		};

		std::wstring CurrentUserSid()
		{
			HANDLE raw_token = nullptr;
			if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) return {};
			Handle token(raw_token);
			DWORD bytes = 0;
			(void)GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &bytes);
			if (bytes == 0) return {};
			std::vector<unsigned char> buffer(bytes);
			if (!GetTokenInformation(token.Get(), TokenUser, buffer.data(), bytes, &bytes)) return {};
			LPWSTR sid = nullptr;
			if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,
			                           &sid)) return {};
			std::wstring value(sid);
			LocalFree(sid);
			return value;
		}

		std::wstring EndpointName(std::string_view runner_version, std::wstring_view kind)
		{
			const std::wstring sid = CurrentUserSid();
			std::uint64_t digest = uam::hashing::kFnv1a64OffsetBasis;
			uam::hashing::UpdateFnv1a64(
			    digest, reinterpret_cast<const unsigned char*>(sid.data()),
			    sid.size() * sizeof(wchar_t));
			uam::hashing::UpdateFnv1a64(digest, runner_version);
			uam::hashing::UpdateFnv1a64(digest, std::to_string(kRunnerProtocolVersion));
			const std::string suffix = uam::hashing::Hex64Padded(digest);
			return std::wstring(kind) +
			       std::wstring(suffix.begin(), suffix.end());
		}

		std::wstring PipeName(std::string_view runner_version)
		{
			return EndpointName(runner_version, L"\\\\.\\pipe\\uam-runner-");
		}

		bool StopRequested(HANDLE stop_event)
		{
			return stop_event != nullptr && WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0;
		}

		bool WaitForOverlappedIo(HANDLE pipe, OVERLAPPED& operation, DWORD& transferred,
		                         HANDLE stop_event)
		{
			const std::array<HANDLE, 2> events = {operation.hEvent, stop_event};
			const DWORD wait = WaitForMultipleObjects(
			    stop_event == nullptr ? 1 : static_cast<DWORD>(events.size()), events.data(),
			    FALSE, INFINITE);
			if (wait == WAIT_OBJECT_0)
				return GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
			if (stop_event != nullptr && wait == WAIT_OBJECT_0 + 1)
			{
				(void)CancelIoEx(pipe, &operation);
				(void)GetOverlappedResult(pipe, &operation, &transferred, TRUE);
			}
			return false;
		}

		bool ReadExact(HANDLE pipe, char* bytes, std::size_t size, DWORD* error_out = nullptr,
		               HANDLE stop_event = nullptr)
		{
			std::size_t offset = 0;
			while (offset < size)
			{
				if (StopRequested(stop_event)) return false;
				Handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
				if (event.Get() == nullptr)
				{
					if (error_out != nullptr) *error_out = GetLastError();
					return false;
				}
				OVERLAPPED operation{};
				operation.hEvent = event.Get();
				DWORD read = 0;
				if (!ReadFile(pipe, bytes + offset,
				              static_cast<DWORD>(std::min<std::size_t>(size - offset, MAXDWORD)),
				              &read, &operation))
				{
					const DWORD error = GetLastError();
					if (error != ERROR_IO_PENDING ||
					    !WaitForOverlappedIo(pipe, operation, read, stop_event))
					{
						if (error_out != nullptr)
							*error_out = error == ERROR_IO_PENDING ? GetLastError() : error;
						return false;
					}
				}
				if (read == 0)
				{
					if (error_out != nullptr) *error_out = ERROR_BROKEN_PIPE;
					return false;
				}
				offset += read;
			}
			return true;
		}

		bool WriteExact(HANDLE pipe, const char* bytes, std::size_t size,
		                HANDLE stop_event = nullptr)
		{
			std::size_t offset = 0;
			while (offset < size)
			{
				if (StopRequested(stop_event)) return false;
				Handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
				if (event.Get() == nullptr) return false;
				OVERLAPPED operation{};
				operation.hEvent = event.Get();
				DWORD written = 0;
				if (!WriteFile(pipe, bytes + offset,
				               static_cast<DWORD>(std::min<std::size_t>(size - offset, MAXDWORD)),
				               &written, &operation))
				{
					if (GetLastError() != ERROR_IO_PENDING ||
					    !WaitForOverlappedIo(pipe, operation, written, stop_event)) return false;
				}
				if (written == 0) return false;
				offset += written;
			}
			return true;
		}

		FrameReadResult ReadPipeFrame(HANDLE pipe, nlohmann::json& message,
		                              HANDLE stop_event = nullptr)
		{
			std::array<unsigned char, 4> header{};
			DWORD error = ERROR_SUCCESS;
			if (!ReadExact(pipe, reinterpret_cast<char*>(header.data()), header.size(), &error,
			               stop_event))
				return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED
				    ? FrameReadResult::EndOfStream : FrameReadResult::Error;
			const std::uint32_t size = (static_cast<std::uint32_t>(header[0]) << 24U) |
			                           (static_cast<std::uint32_t>(header[1]) << 16U) |
			                           (static_cast<std::uint32_t>(header[2]) << 8U) |
			                           static_cast<std::uint32_t>(header[3]);
			if (size == 0 || size > kMaxRunnerFrameBytes) return FrameReadResult::Error;
			std::string body(size, '\0');
			if (!ReadExact(pipe, body.data(), body.size(), nullptr, stop_event))
				return FrameReadResult::Error;
			message = nlohmann::json::parse(body, nullptr, false);
			return message.is_object() ? FrameReadResult::Ok : FrameReadResult::Error;
		}

		bool WritePipeFrame(HANDLE pipe, const nlohmann::json& message,
		                    HANDLE stop_event = nullptr)
		{
			const std::string body = message.dump();
			if (body.empty() || body.size() > kMaxRunnerFrameBytes) return false;
			const std::uint32_t size = static_cast<std::uint32_t>(body.size());
			const std::array<unsigned char, 4> header = {
			    static_cast<unsigned char>((size >> 24U) & 0xffU),
			    static_cast<unsigned char>((size >> 16U) & 0xffU),
			    static_cast<unsigned char>((size >> 8U) & 0xffU),
			    static_cast<unsigned char>(size & 0xffU)};
			return WriteExact(pipe, reinterpret_cast<const char*>(header.data()), header.size(), stop_event) &&
			       WriteExact(pipe, body.data(), body.size(), stop_event);
		}

		Handle ConnectPipe(std::string_view runner_version)
		{
			const std::wstring name = PipeName(runner_version);
			for (int attempt = 0; attempt < 20; ++attempt)
			{
				HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
				                          nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
				if (pipe != INVALID_HANDLE_VALUE) return Handle(pipe);
				const DWORD error = GetLastError();
				if (error == ERROR_PIPE_BUSY)
					(void)WaitNamedPipeW(name.c_str(), 250);
				else if (error == ERROR_FILE_NOT_FOUND)
					Sleep(10);
				else
					break;
			}
			return Handle();
		}

		bool WakeServer(std::string_view runner_version)
		{
			Handle wake = ConnectPipe(runner_version);
			return wake.Get() != INVALID_HANDLE_VALUE;
		}

		struct ClientThread
		{
			std::shared_ptr<std::atomic<bool>> done;
			std::jthread thread;
		};
	}

	int RunRunnerService(std::string_view runner_version)
	{
		const HANDLE singleton_handle = CreateMutexW(nullptr, TRUE,
		    EndpointName(runner_version, L"Local\\uam-runner-").c_str());
		if (singleton_handle == nullptr) return 2;
		Handle singleton(singleton_handle);
		if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
		const std::wstring sid = CurrentUserSid();
		if (sid.empty()) return 2;
		PSECURITY_DESCRIPTOR descriptor = nullptr;
		const std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")";
		if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
		        sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return 2;
		struct DescriptorGuard { PSECURITY_DESCRIPTOR value; ~DescriptorGuard() { LocalFree(value); } }
		    descriptor_guard{descriptor};
		SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
		RunnerState state;
		std::atomic<bool> stopping{false};
		std::mutex dispatch_mutex;
		Handle stop_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
		if (stop_event.Get() == nullptr) return 2;
		std::vector<ClientThread> clients;
		const std::wstring pipe_name = PipeName(runner_version);
		while (!stopping.load(std::memory_order_acquire))
		{
			Handle listener(CreateNamedPipeW(
			    pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
			    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
			    PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &security));
			if (listener.Get() == INVALID_HANDLE_VALUE) return 2;
			Handle connected_event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
			if (connected_event.Get() == nullptr) return 2;
			OVERLAPPED connected_operation{};
			connected_operation.hEvent = connected_event.Get();
			BOOL connected = ConnectNamedPipe(listener.Get(), &connected_operation);
			if (!connected)
			{
				const DWORD connect_error = GetLastError();
				if (connect_error == ERROR_IO_PENDING)
				{
					DWORD transferred = 0;
					connected = GetOverlappedResult(listener.Get(), &connected_operation,
					                                &transferred, TRUE);
				}
				else
					connected = connect_error == ERROR_PIPE_CONNECTED;
			}
			if (!connected) return 2;
			if (stopping.load(std::memory_order_acquire)) break;
			std::erase_if(clients, [](const ClientThread& client)
			{
				return client.done->load(std::memory_order_acquire);
			});
			auto done = std::make_shared<std::atomic<bool>>(false);
			clients.push_back({done, std::jthread([&, pipe = listener.Release(), done]
			{
				Handle client(pipe);
				for (;;)
				{
					nlohmann::json request;
					if (ReadPipeFrame(client.Get(), request, stop_event.Get()) != FrameReadResult::Ok) break;
					if (request.value("type", "") == "service.shutdown")
					{
						bool busy = false;
						{
							std::scoped_lock dispatch_lock(dispatch_mutex);
							busy = state.HasManagedProcesses();
							if (!busy)
							{
								stopping.store(true, std::memory_order_release);
								(void)SetEvent(stop_event.Get());
							}
						}
						if (busy)
						{
							(void)WritePipeFrame(client.Get(),
							    {{"id", request.value("id", "")},
							     {"type", "service.shutdown"}, {"ok", false},
							     {"error", {{"code", "processes_active"},
							                {"message", "Runner service still owns remote processes."}}}});
							continue;
						}
						(void)WritePipeFrame(client.Get(), {{"id", request.value("id", "")},
						    {"type", "service.shutdown"}, {"ok", true},
						    {"result", nlohmann::json::object()}});
						(void)WakeServer(runner_version);
						break;
					}
					nlohmann::json response;
					if (request.value("type", "") == "process.start")
					{
						std::scoped_lock dispatch_lock(dispatch_mutex);
						response = stopping.load(std::memory_order_acquire)
						    ? nlohmann::json{{"id", request.value("id", "")},
						                     {"type", "error"}, {"ok", false},
						                     {"error", {{"code", "service_stopping"},
						                                {"message", "Runner service is stopping."}}}}
						    : HandleRunnerRequest(request, runner_version, &state);
					}
					else
						response = HandleRunnerRequest(request, runner_version, &state);
					if (!WritePipeFrame(client.Get(), response, stop_event.Get())) break;
				}
				done->store(true, std::memory_order_release);
			})});
		}
		return 0;
	}

	int StartRunnerService(std::string_view runner_version)
	{
		Handle existing = ConnectPipe(runner_version);
		if (existing.Get() != INVALID_HANDLE_VALUE) return 0;
		std::wstring executable(32768, L'\0');
		const DWORD size = GetModuleFileNameW(nullptr, executable.data(),
		                                      static_cast<DWORD>(executable.size()));
		if (size == 0 || size == executable.size()) return 2;
		executable.resize(size);
		std::wstring command = L"\"" + executable + L"\" serve";
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION process{};
		if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
		                    CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
		                    nullptr, nullptr, &startup, &process)) return 2;
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		for (int attempt = 0; attempt < 200; ++attempt)
		{
			Handle ready = ConnectPipe(runner_version);
			if (ready.Get() != INVALID_HANDLE_VALUE) return 0;
			Sleep(50);
		}
		return 2;
	}

	int StopRunnerService(std::string_view runner_version)
	{
		{
			Handle service = ConnectPipe(runner_version);
			if (service.Get() == INVALID_HANDLE_VALUE) return 0;
			if (!WritePipeFrame(service.Get(), {{"id", "stop"}, {"type", "service.shutdown"}}))
				return 2;
			nlohmann::json response;
			if (ReadPipeFrame(service.Get(), response) != FrameReadResult::Ok ||
			    !response.value("ok", false)) return 2;
		}
		for (int attempt = 0; attempt < 200; ++attempt)
		{
			Handle remaining = ConnectPipe(runner_version);
			if (remaining.Get() == INVALID_HANDLE_VALUE) return 0;
			Sleep(10);
		}
		return 2;
	}

	int RunRunnerBridge(std::string_view runner_version)
	{
		if (_setmode(_fileno(stdin), _O_BINARY) == -1 ||
		    _setmode(_fileno(stdout), _O_BINARY) == -1)
		{
			std::cerr << "Runner bridge could not enable binary stdio.\n";
			return 2;
		}
		Handle service = ConnectPipe(runner_version);
		if (service.Get() == INVALID_HANDLE_VALUE)
		{
			std::cerr << "Runner service pipe is unavailable (Windows error " << GetLastError()
			          << ").\n";
			return 2;
		}
		for (;;)
		{
			nlohmann::json request;
			std::string error;
			const FrameReadResult input = ReadFrame(std::cin, request, &error);
			if (input == FrameReadResult::EndOfStream) return 0;
			if (input != FrameReadResult::Ok)
			{
				std::cerr << (error.empty() ? "Runner bridge input frame is invalid." : error) << '\n';
				return 2;
			}
			if (!WritePipeFrame(service.Get(), request))
			{
				std::cerr << "Runner bridge could not write to the service pipe (Windows error "
				          << GetLastError() << ").\n";
				return 2;
			}
			nlohmann::json response;
			if (ReadPipeFrame(service.Get(), response) != FrameReadResult::Ok)
			{
				std::cerr << "Runner bridge could not read from the service pipe (Windows error "
				          << GetLastError() << ").\n";
				return 2;
			}
			if (!WriteFrame(std::cout, response, &error))
			{
				std::cerr << (error.empty() ? "Runner bridge could not write its response." : error)
				          << '\n';
				return 2;
			}
		}
	}
}

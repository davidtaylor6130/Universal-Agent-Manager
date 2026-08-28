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

		std::wstring PipeName()
		{
			const std::wstring sid = CurrentUserSid();
			std::uint64_t digest = uam::hashing::kFnv1a64OffsetBasis;
			uam::hashing::UpdateFnv1a64(
			    digest, reinterpret_cast<const unsigned char*>(sid.data()),
			    sid.size() * sizeof(wchar_t));
			const std::string suffix = uam::hashing::Hex64Padded(digest);
			return L"\\\\.\\pipe\\uam-runner-v1-" +
			       std::wstring(suffix.begin(), suffix.end());
		}

		bool ReadExact(HANDLE pipe, char* bytes, std::size_t size)
		{
			std::size_t offset = 0;
			while (offset < size)
			{
				DWORD read = 0;
				if (!ReadFile(pipe, bytes + offset,
				              static_cast<DWORD>(std::min<std::size_t>(size - offset, MAXDWORD)),
				              &read, nullptr) || read == 0) return false;
				offset += read;
			}
			return true;
		}

		bool WriteExact(HANDLE pipe, const char* bytes, std::size_t size)
		{
			std::size_t offset = 0;
			while (offset < size)
			{
				DWORD written = 0;
				if (!WriteFile(pipe, bytes + offset,
				               static_cast<DWORD>(std::min<std::size_t>(size - offset, MAXDWORD)),
				               &written, nullptr) || written == 0) return false;
				offset += written;
			}
			return true;
		}

		FrameReadResult ReadPipeFrame(HANDLE pipe, nlohmann::json& message)
		{
			std::array<unsigned char, 4> header{};
			DWORD first = 0;
			if (!ReadFile(pipe, header.data(), static_cast<DWORD>(header.size()), &first, nullptr))
				return GetLastError() == ERROR_BROKEN_PIPE ? FrameReadResult::EndOfStream
				                                      : FrameReadResult::Error;
			if (first == 0) return FrameReadResult::EndOfStream;
			if (first < header.size() &&
			    !ReadExact(pipe, reinterpret_cast<char*>(header.data()) + first,
			               header.size() - first)) return FrameReadResult::Error;
			const std::uint32_t size = (static_cast<std::uint32_t>(header[0]) << 24U) |
			                           (static_cast<std::uint32_t>(header[1]) << 16U) |
			                           (static_cast<std::uint32_t>(header[2]) << 8U) |
			                           static_cast<std::uint32_t>(header[3]);
			if (size == 0 || size > kMaxRunnerFrameBytes) return FrameReadResult::Error;
			std::string body(size, '\0');
			if (!ReadExact(pipe, body.data(), body.size())) return FrameReadResult::Error;
			message = nlohmann::json::parse(body, nullptr, false);
			return message.is_object() ? FrameReadResult::Ok : FrameReadResult::Error;
		}

		bool WritePipeFrame(HANDLE pipe, const nlohmann::json& message)
		{
			const std::string body = message.dump();
			if (body.empty() || body.size() > kMaxRunnerFrameBytes) return false;
			const std::uint32_t size = static_cast<std::uint32_t>(body.size());
			const std::array<unsigned char, 4> header = {
			    static_cast<unsigned char>((size >> 24U) & 0xffU),
			    static_cast<unsigned char>((size >> 16U) & 0xffU),
			    static_cast<unsigned char>((size >> 8U) & 0xffU),
			    static_cast<unsigned char>(size & 0xffU)};
			return WriteExact(pipe, reinterpret_cast<const char*>(header.data()), header.size()) &&
			       WriteExact(pipe, body.data(), body.size());
		}

		Handle ConnectPipe()
		{
			const std::wstring name = PipeName();
			for (int attempt = 0; attempt < 20; ++attempt)
			{
				HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
				                          nullptr, OPEN_EXISTING, 0, nullptr);
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

		bool WakeServer()
		{
			Handle wake = ConnectPipe();
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
		std::vector<ClientThread> clients;
		const std::wstring pipe_name = PipeName();
		while (!stopping.load(std::memory_order_acquire))
		{
			Handle listener(CreateNamedPipeW(
			    pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
			    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
			    PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &security));
			if (listener.Get() == INVALID_HANDLE_VALUE) return 2;
			const BOOL connected = ConnectNamedPipe(listener.Get(), nullptr) ||
			                       GetLastError() == ERROR_PIPE_CONNECTED;
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
					if (ReadPipeFrame(client.Get(), request) != FrameReadResult::Ok) break;
					if (request.value("type", "") == "service.shutdown")
					{
						(void)WritePipeFrame(client.Get(), {{"id", request.value("id", "")},
						    {"type", "service.shutdown"}, {"ok", true},
						    {"result", nlohmann::json::object()}});
						stopping.store(true, std::memory_order_release);
						(void)WakeServer();
						break;
					}
					if (!WritePipeFrame(client.Get(), HandleRunnerRequest(
					        request, runner_version, &state))) break;
				}
				done->store(true, std::memory_order_release);
			})});
		}
		for (ClientThread& client : clients)
			if (client.thread.joinable())
				(void)CancelSynchronousIo(client.thread.native_handle());
		return 0;
	}

	int StartRunnerService(std::string_view runner_version)
	{
		(void)runner_version;
		Handle existing = ConnectPipe();
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
			Handle ready = ConnectPipe();
			if (ready.Get() != INVALID_HANDLE_VALUE) return 0;
			Sleep(50);
		}
		return 2;
	}

	int StopRunnerService()
	{
		{
			Handle service = ConnectPipe();
			if (service.Get() == INVALID_HANDLE_VALUE) return 0;
			if (!WritePipeFrame(service.Get(), {{"id", "stop"}, {"type", "service.shutdown"}}))
				return 2;
			nlohmann::json response;
			if (ReadPipeFrame(service.Get(), response) != FrameReadResult::Ok ||
			    !response.value("ok", false)) return 2;
		}
		for (int attempt = 0; attempt < 200; ++attempt)
		{
			Handle remaining = ConnectPipe();
			if (remaining.Get() == INVALID_HANDLE_VALUE) return 0;
			Sleep(10);
		}
		return 2;
	}

	int RunRunnerBridge()
	{
		if (_setmode(_fileno(stdin), _O_BINARY) == -1 ||
		    _setmode(_fileno(stdout), _O_BINARY) == -1)
		{
			std::cerr << "Runner bridge could not enable binary stdio.\n";
			return 2;
		}
		Handle service = ConnectPipe();
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

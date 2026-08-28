#include "remote/runner_state.h"

#include "common/platform/platform_services.h"
#include "common/utils/base64.h"
#include "common/utils/hash_utils.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <thread>

#if defined(__APPLE__)
namespace uam::platform_macos_impl
{
	IPlatformProcessService& GetMacProcessService();
}
#elif defined(_WIN32)
namespace uam::platform_windows_impl
{
	IPlatformProcessService& GetWindowsProcessService();
}
#elif defined(__linux__)
namespace uam::platform_linux_impl
{
	IPlatformProcessService& GetLinuxProcessService();
}
#endif

namespace uam::remote
{
	namespace
	{
		inline constexpr std::size_t kMaxSessionIdBytes = 128;
		inline constexpr std::size_t kMaxWorkingDirectoryBytes = 4096;
		inline constexpr std::size_t kMaxArgumentCount = 256;
		inline constexpr std::size_t kMaxArgumentBytes = 32 * 1024;
		inline constexpr std::size_t kMaxEnvironmentEntries = 128;
		inline constexpr std::size_t kMaxEnvironmentValueBytes = 32 * 1024;
		inline constexpr std::size_t kMaxWriteBytes = 256 * 1024;
		inline constexpr std::size_t kMaxReadBytesPerStream = 256 * 1024;
		inline constexpr std::size_t kMaxChannelBytes = 1024 * 1024;
		inline constexpr std::uintmax_t kMaxUploadBytes = 25ull * 1024ull * 1024ull;
		inline constexpr std::uintmax_t kMaxSpoolBytesPerStream = 1024ull * 1024ull * 1024ull;

		IPlatformProcessService& ProcessService()
		{
#if defined(__APPLE__)
			return uam::platform_macos_impl::GetMacProcessService();
#elif defined(_WIN32)
			return uam::platform_windows_impl::GetWindowsProcessService();
#elif defined(__linux__)
			return uam::platform_linux_impl::GetLinuxProcessService();
#else
#error "Remote process execution is implemented only on macOS, Windows, and Linux."
#endif
		}

		bool IsBoundedText(std::string_view value, std::size_t maximum, bool allow_empty = false)
		{
			return (allow_empty || !value.empty()) && value.size() <= maximum &&
			       value.find('\0') == std::string_view::npos;
		}

		bool IsSessionId(std::string_view value)
		{
			return IsBoundedText(value, kMaxSessionIdBytes) &&
			       std::ranges::all_of(value, [](unsigned char character)
			       { return std::isalnum(character) != 0 || character == '-' || character == '_'; });
		}

		bool IsEnvironmentName(std::string_view value)
		{
			if (value.empty() || value.size() > 128 ||
			    !(std::isalpha(static_cast<unsigned char>(value.front())) != 0 || value.front() == '_'))
				return false;
			return std::ranges::all_of(value.substr(1), [](unsigned char character)
			{ return std::isalnum(character) != 0 || character == '_'; });
		}

		nlohmann::json ProcessError(const nlohmann::json& request, std::string code,
		                           std::string message)
		{
			return {{"id", request.value("id", nlohmann::json(nullptr))}, {"type", "error"},
			        {"ok", false},
			        {"error", {{"code", std::move(code)}, {"message", std::move(message)}}}};
		}

		nlohmann::json ProcessSuccess(const nlohmann::json& request, nlohmann::json result)
		{
			return {{"id", request["id"]}, {"type", request["type"]}, {"ok", true},
			        {"result", std::move(result)}};
		}

		bool ParseStart(const nlohmann::json& request, std::string& session_id,
		                std::filesystem::path& working_directory,
		                std::vector<std::string>& arguments,
		                std::vector<std::pair<std::string, std::string>>& environment,
		                std::string& error)
		{
			if (!request.contains("sessionId") || !request["sessionId"].is_string() ||
			    !IsSessionId(request["sessionId"].get_ref<const std::string&>()))
			{
				error = "A portable bounded sessionId is required.";
				return false;
			}
			session_id = request["sessionId"].get<std::string>();
			if (!request.contains("cwd") || !request["cwd"].is_string())
			{
				error = "An absolute working directory is required.";
				return false;
			}
			const std::string cwd = request["cwd"].get<std::string>();
			working_directory = std::filesystem::path(cwd);
			if (!IsBoundedText(cwd, kMaxWorkingDirectoryBytes) || !working_directory.is_absolute())
			{
				error = "The working directory must be an absolute bounded path.";
				return false;
			}
			if (!request.contains("argv") || !request["argv"].is_array() ||
			    request["argv"].empty() || request["argv"].size() > kMaxArgumentCount)
			{
				error = "argv must be a non-empty bounded array.";
				return false;
			}
			for (const nlohmann::json& value : request["argv"])
			{
				if (!value.is_string() ||
				    !IsBoundedText(value.get_ref<const std::string&>(), kMaxArgumentBytes))
				{
					error = "Every argv item must be a non-empty bounded string.";
					return false;
				}
				arguments.push_back(value.get<std::string>());
			}
			if (!request.contains("environment")) return true;
			if (!request["environment"].is_object() ||
			    request["environment"].size() > kMaxEnvironmentEntries)
			{
				error = "environment must be a bounded object.";
				return false;
			}
			for (const auto& [name, value] : request["environment"].items())
			{
				if (!IsEnvironmentName(name) || !value.is_string() ||
				    !IsBoundedText(value.get_ref<const std::string&>(),
				                   kMaxEnvironmentValueBytes, true))
				{
					error = "Environment entries must use safe names and bounded string values.";
					return false;
				}
				environment.emplace_back(name, value.get<std::string>());
			}
			return true;
		}

		std::string ReadAvailable(RunnerState::Process& process, bool standard_error,
		                          std::string& error)
		{
			std::string output;
			std::array<char, 16 * 1024> buffer{};
			while (output.size() < kMaxReadBytesPerStream)
			{
				const std::size_t available = kMaxReadBytesPerStream - output.size();
				const std::size_t request_size = std::min(buffer.size(), available);
				const std::ptrdiff_t read = standard_error
				    ? ProcessService().ReadStdioProcessStderr(
				          process.fields, buffer.data(), request_size, &error)
				    : ProcessService().ReadStdioProcessStdout(
				          process.fields, buffer.data(), request_size, &error);
				if (read > 0)
				{
					output.append(buffer.data(), static_cast<std::size_t>(read));
					continue;
				}
				if (read == -2 || read == 0) break;
				if (error.empty()) error = "Remote process output could not be read.";
				break;
			}
			return output;
		}

		bool AppendSpool(const std::filesystem::path& path, std::ofstream& stream,
		                 std::string_view bytes, std::string& error)
		{
			std::error_code size_error;
			const std::uintmax_t size = std::filesystem::file_size(path, size_error);
			if (size_error || bytes.size() > kMaxSpoolBytesPerStream -
			                              std::min(size, kMaxSpoolBytesPerStream))
			{
				error = "Remote process output exceeded the 1 GiB disconnect spool limit.";
				return false;
			}
			stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
			stream.flush();
			if (!stream)
			{
				error = "Remote process output could not be spooled.";
				return false;
			}
			return true;
		}

		std::string ReadSpool(const std::filesystem::path& path, std::uintmax_t& offset,
		                      std::string& error)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
			{
				error = "Remote process output spool could not be opened.";
				return {};
			}
			stream.seekg(static_cast<std::streamoff>(offset));
			std::string output(kMaxReadBytesPerStream, '\0');
			stream.read(output.data(), static_cast<std::streamsize>(output.size()));
			output.resize(static_cast<std::size_t>(stream.gcount()));
			offset += output.size();
			return output;
		}

		bool SpoolHasUnread(const std::filesystem::path& path, std::uintmax_t offset)
		{
			std::error_code error;
			const std::uintmax_t size = std::filesystem::file_size(path, error);
			return !error && offset < size;
		}

		void StartDrainer(RunnerState::Process& process)
		{
			process.drainer = std::jthread([&process](std::stop_token stop_token)
			{
				std::ofstream stdout_stream(process.stdout_spool,
				                            std::ios::binary | std::ios::app);
				std::ofstream stderr_stream(process.stderr_spool,
				                            std::ios::binary | std::ios::app);
				if (!stdout_stream || !stderr_stream)
				{
					std::scoped_lock lock(process.mutex);
					process.spool_error = "Remote process output spool could not be created.";
					ProcessService().TerminateStdioProcess(process.fields, true);
					process.exited.store(true, std::memory_order_release);
					return;
				}
				while (!stop_token.stop_requested())
				{
					std::string output_error;
					std::string stdout_bytes;
					std::string stderr_bytes;
					bool exited = false;
					int exit_code = -1;
					{
						std::scoped_lock lock(process.mutex);
						stdout_bytes = ReadAvailable(process, false, output_error);
						stderr_bytes = ReadAvailable(process, true, output_error);
						exited = ProcessService().PollStdioProcessExited(process.fields, &exit_code);
					}
					bool spool_ok = output_error.empty();
					if (spool_ok)
					{
						std::scoped_lock lock(process.mutex);
						spool_ok = AppendSpool(process.stdout_spool, stdout_stream, stdout_bytes,
						                       output_error) &&
						           AppendSpool(process.stderr_spool, stderr_stream, stderr_bytes,
						                       output_error);
					}
					if (!spool_ok)
					{
						std::scoped_lock lock(process.mutex);
						process.spool_error = std::move(output_error);
						ProcessService().TerminateStdioProcess(process.fields, true);
						process.exited.store(true, std::memory_order_release);
						return;
					}
					if (exited)
					{
						process.exit_code.store(exit_code, std::memory_order_release);
						process.exited.store(true, std::memory_order_release);
						return;
					}
					if (stdout_bytes.empty() && stderr_bytes.empty())
						std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}
			});
		}
	}

	RunnerState::RunnerState()
	{
		std::error_code error;
		m_spoolDirectory = std::filesystem::temp_directory_path(error) /
		                   ("uam-runner-spool-" + ProcessService().GenerateUuid());
		if (!error && std::filesystem::create_directory(m_spoolDirectory, error))
			std::filesystem::permissions(
			    m_spoolDirectory, std::filesystem::perms::owner_all,
			    std::filesystem::perm_options::replace, error);
		if (error) m_spoolDirectory.clear();
	}

	RunnerState::~RunnerState()
	{
		for (auto& [session_id, process] : m_processes)
		{
			(void)session_id;
			process->drainer.request_stop();
			{
				std::scoped_lock lock(process->mutex);
				ProcessService().TerminateStdioProcess(process->fields, true);
			}
			if (process->drainer.joinable()) process->drainer.join();
			ProcessService().CloseStdioProcessHandles(process->fields);
		}
		for (const auto& [upload_id, upload] : m_uploads)
		{
			(void)upload_id;
			std::error_code remove_error;
			std::filesystem::remove(upload.temporary, remove_error);
		}
		std::error_code cleanup_error;
		if (!m_spoolDirectory.empty())
			std::filesystem::remove_all(m_spoolDirectory, cleanup_error);
	}

	nlohmann::json RunnerState::HandleProcessRequest(const nlohmann::json& request)
	{
		std::scoped_lock state_lock(m_stateMutex);
		const std::string type = request["type"].get<std::string>();
		if (type.starts_with("file."))
		{
			if (!request.contains("uploadId") || !request["uploadId"].is_string() ||
			    !IsSessionId(request["uploadId"].get_ref<const std::string&>()))
				return ProcessError(request, "invalid_request",
				                    "A portable bounded uploadId is required.");
			const std::string upload_id = request["uploadId"].get<std::string>();
			if (type == "file.remove")
			{
				if (!request.contains("path") || !request["path"].is_string())
					return ProcessError(request, "invalid_request", "A file path is required.");
				const std::string path_text = request["path"].get<std::string>();
				const std::filesystem::path path(path_text);
				if (!IsBoundedText(path_text, kMaxWorkingDirectoryBytes) || !path.is_absolute())
					return ProcessError(request, "invalid_request", "The file path is invalid.");
				std::error_code error;
				if (!std::filesystem::is_regular_file(path, error) || error ||
				    !std::filesystem::remove(path, error) || error)
					return ProcessError(request, "remove_failed", "The staged file could not be removed.");
				return ProcessSuccess(request, nlohmann::json::object());
			}
			if (type == "file.begin")
			{
				if (m_uploads.contains(upload_id))
					return ProcessError(request, "upload_exists",
					                    "An upload already uses this uploadId.");
				if (!request.contains("path") || !request["path"].is_string() ||
				    !request.contains("size") ||
				    (!request["size"].is_number_unsigned() &&
				     (!request["size"].is_number_integer() ||
				      request["size"].get<std::int64_t>() < 0)) ||
				    !request.contains("digest") || !request["digest"].is_string())
					return ProcessError(request, "invalid_request",
					                    "Upload path, size, and digest are required.");
				const std::string path_text = request["path"].get<std::string>();
				const std::uintmax_t size = request["size"].is_number_unsigned()
				    ? request["size"].get<std::uintmax_t>()
				    : static_cast<std::uintmax_t>(request["size"].get<std::int64_t>());
				const std::string digest = request["digest"].get<std::string>();
				const std::filesystem::path target(path_text);
				if (!IsBoundedText(path_text, kMaxWorkingDirectoryBytes) || !target.is_absolute() ||
				    size > kMaxUploadBytes || digest.size() != 16 ||
				    !std::ranges::all_of(digest, [](unsigned char character)
				    { return std::isxdigit(character) != 0 && !std::isupper(character); }))
					return ProcessError(request, "invalid_request", "Upload metadata is invalid.");
				std::error_code error;
				std::filesystem::create_directories(target.parent_path(), error);
				if (error || std::filesystem::exists(target, error) || error)
					return ProcessError(request, "target_unavailable",
					                    "The upload target already exists or cannot be created.");
				const std::filesystem::path temporary =
				    target.parent_path() / (".uam-upload-" + upload_id + ".tmp");
				std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
				if (!stream)
					return ProcessError(request, "target_unavailable",
					                    "The upload temporary file could not be created.");
				std::filesystem::permissions(
				    temporary, std::filesystem::perms::owner_read |
				                   std::filesystem::perms::owner_write,
				    std::filesystem::perm_options::replace, error);
				m_uploads.emplace(upload_id, Upload{target, temporary, size, 0,
				                                          uam::hashing::kFnv1a64OffsetBasis,
				                                          digest});
				return ProcessSuccess(request, {{"uploadId", upload_id}});
			}
			const auto found = m_uploads.find(upload_id);
			if (found == m_uploads.end())
				return ProcessError(request, "upload_not_found", "The upload does not exist.");
			Upload& upload = found->second;
			if (type == "file.abort")
			{
				std::error_code error;
				std::filesystem::remove(upload.temporary, error);
				m_uploads.erase(found);
				return ProcessSuccess(request, nlohmann::json::object());
			}
			if (type == "file.write")
			{
				if (!request.contains("dataBase64") || !request["dataBase64"].is_string())
					return ProcessError(request, "invalid_request", "dataBase64 is required.");
				std::string decoded;
				if (!uam::base64::Decode(request["dataBase64"].get_ref<const std::string&>(), decoded) ||
				    decoded.size() > kMaxWriteBytes ||
				    decoded.size() > upload.expected_size - upload.received_size)
					return ProcessError(request, "invalid_request", "Upload data is invalid or too large.");
				std::ofstream stream(upload.temporary, std::ios::binary | std::ios::app);
				stream.write(decoded.data(), static_cast<std::streamsize>(decoded.size()));
				if (!stream)
					return ProcessError(request, "write_failed", "Upload data could not be written.");
				uam::hashing::UpdateFnv1a64(upload.digest,
				    reinterpret_cast<const unsigned char*>(decoded.data()), decoded.size());
				upload.received_size += decoded.size();
				return ProcessSuccess(request, {{"acceptedBytes", decoded.size()}});
			}
			if (type == "file.commit")
			{
				if (upload.received_size != upload.expected_size ||
				    uam::hashing::Hex64Padded(upload.digest) != upload.expected_digest)
					return ProcessError(request, "digest_mismatch",
					                    "Upload size or digest verification failed.");
				std::error_code error;
				std::filesystem::rename(upload.temporary, upload.target, error);
				if (error)
					return ProcessError(request, "commit_failed", "Upload could not be committed.");
				const std::string path = upload.target.string();
				m_uploads.erase(found);
				return ProcessSuccess(request, {{"path", path}});
			}
			return ProcessError(request, "unsupported_request", "Unknown runner file request.");
		}
		if (type.starts_with("channel."))
		{
			if (!request.contains("channelId") || !request["channelId"].is_string() ||
			    !IsSessionId(request["channelId"].get_ref<const std::string&>()))
				return ProcessError(request, "invalid_request",
				                    "A portable bounded channelId is required.");
			const std::string channel_id = request["channelId"].get<std::string>();
			if (type == "channel.open")
			{
				const bool attached = m_channels.contains(channel_id);
				if (attached && !request.value("attachIfExists", false))
					return ProcessError(request, "channel_exists",
					                    "A runner channel already uses this channelId.");
				m_channels.try_emplace(channel_id);
				return ProcessSuccess(request, {{"channelId", channel_id}, {"attached", attached}});
			}
			const auto found = m_channels.find(channel_id);
			if (found == m_channels.end())
				return ProcessError(request, "channel_not_found", "The runner channel does not exist.");
			if (type == "channel.close")
			{
				m_channels.erase(found);
				return ProcessSuccess(request, nlohmann::json::object());
			}
			if (!request.contains("direction") || !request["direction"].is_string())
				return ProcessError(request, "invalid_request", "A channel direction is required.");
			const std::string direction = request["direction"].get<std::string>();
			std::string* buffer = direction == "remoteToDesktop"
			                          ? &found->second.remote_to_desktop
			                          : direction == "desktopToRemote"
			                              ? &found->second.desktop_to_remote
			                              : nullptr;
			if (buffer == nullptr)
				return ProcessError(request, "invalid_request", "The channel direction is invalid.");
			if (type == "channel.write")
			{
				if (!request.contains("dataBase64") || !request["dataBase64"].is_string())
					return ProcessError(request, "invalid_request", "dataBase64 is required.");
				std::string decoded;
				if (!uam::base64::Decode(request["dataBase64"].get_ref<const std::string&>(), decoded))
					return ProcessError(request, "invalid_request", "dataBase64 is invalid.");
				if (decoded.size() > kMaxWriteBytes ||
				    decoded.size() > kMaxChannelBytes - buffer->size())
					return ProcessError(request, "channel_full", "The channel buffer limit was reached.");
				buffer->append(decoded);
				return ProcessSuccess(request, {{"acceptedBytes", decoded.size()}});
			}
			if (type == "channel.poll")
			{
				const std::size_t size = std::min(buffer->size(), kMaxReadBytesPerStream);
				const std::string bytes = buffer->substr(0, size);
				buffer->erase(0, size);
				return ProcessSuccess(request, {{"dataBase64", uam::base64::Encode(bytes)}});
			}
			return ProcessError(request, "unsupported_request", "Unknown runner channel request.");
		}
		if (type == "process.start")
		{
			std::string session_id;
			std::filesystem::path working_directory;
			std::vector<std::string> arguments;
			std::vector<std::pair<std::string, std::string>> environment;
			std::string error;
			if (!ParseStart(request, session_id, working_directory, arguments, environment,
			                error))
				return ProcessError(request, "invalid_request", std::move(error));
			if (const auto existing = m_processes.find(session_id); existing != m_processes.end())
			{
				if (!request.value("attachIfExists", false))
					return ProcessError(request, "session_exists",
					                    "A remote process already uses this sessionId.");
				Process& process = *existing->second;
				if (process.working_directory != working_directory ||
				    process.arguments != arguments || process.environment != environment)
					return ProcessError(request, "session_conflict",
					                    "The existing remote process does not match this launch request.");
				const bool exited = process.exited.load(std::memory_order_acquire);
				return ProcessSuccess(request,
				                      {{"sessionId", session_id}, {"running", !exited},
				                       {"attached", true}});
			}
			auto process = std::make_unique<Process>();
			process->working_directory = working_directory;
			process->arguments = arguments;
			process->environment = environment;
			if (m_spoolDirectory.empty())
				return ProcessError(request, "spool_unavailable",
				                    "The remote process output spool is unavailable.");
			process->stdout_spool = m_spoolDirectory / (session_id + ".stdout");
			process->stderr_spool = m_spoolDirectory / (session_id + ".stderr");
			{
				std::ofstream stdout_file(process->stdout_spool, std::ios::binary);
				std::ofstream stderr_file(process->stderr_spool, std::ios::binary);
				if (!stdout_file || !stderr_file)
					return ProcessError(request, "spool_unavailable",
					                    "The remote process output spool could not be created.");
			}
			if (!ProcessService().StartStdioProcess(process->fields, working_directory,
			                                                arguments, &error, environment))
				return ProcessError(request, "start_failed",
				                    error.empty() ? "The remote process could not start." : error);
			StartDrainer(*process);
			m_processes.emplace(session_id, std::move(process));
			return ProcessSuccess(request, {{"sessionId", session_id}, {"running", true}});
		}

		if (!request.contains("sessionId") || !request["sessionId"].is_string() ||
		    !IsSessionId(request["sessionId"].get_ref<const std::string&>()))
			return ProcessError(request, "invalid_request",
			                    "A portable bounded sessionId is required.");
		const std::string session_id = request["sessionId"].get<std::string>();
		const auto found = m_processes.find(session_id);
		if (found == m_processes.end())
			return ProcessError(request, "session_not_found", "The remote process does not exist.");
		Process& process = *found->second;

		if (type == "process.write")
		{
			if (process.exited.load(std::memory_order_acquire))
				return ProcessError(request, "process_exited", "The remote process has exited.");
			if (!request.contains("dataBase64") || !request["dataBase64"].is_string())
				return ProcessError(request, "invalid_request", "dataBase64 is required.");
			std::string decoded;
			if (!uam::base64::Decode(request["dataBase64"].get_ref<const std::string&>(), decoded) ||
			    decoded.size() > kMaxWriteBytes)
				return ProcessError(request, "invalid_request", "Process input is invalid or too large.");
			std::string error;
			std::scoped_lock lock(process.mutex);
			if (!ProcessService().WriteToStdioProcess(process.fields, decoded.data(), decoded.size(), &error))
				return ProcessError(request, "write_failed", std::move(error));
			return ProcessSuccess(request, {{"acceptedBytes", decoded.size()}});
		}

		if (type == "process.closeInput")
		{
			std::scoped_lock lock(process.mutex);
			ProcessService().CloseStdioProcessInput(process.fields);
			return ProcessSuccess(request, nlohmann::json::object());
		}

		if (type == "process.stop")
		{
			{
				std::scoped_lock lock(process.mutex);
				ProcessService().TerminateStdioProcess(process.fields, true);
			}
			process.exited.store(true, std::memory_order_release);
			process.exit_code.store(-1, std::memory_order_release);
			return ProcessSuccess(request, {{"sessionId", session_id}, {"running", false}});
		}

		if (type == "process.remove")
		{
			if (!process.exited.load(std::memory_order_acquire))
				return ProcessError(request, "process_running",
				                    "Stop the remote process before removing it.");
			process.drainer.request_stop();
			if (process.drainer.joinable()) process.drainer.join();
			ProcessService().CloseStdioProcessHandles(process.fields);
			std::error_code remove_error;
			std::filesystem::remove(process.stdout_spool, remove_error);
			std::filesystem::remove(process.stderr_spool, remove_error);
			m_processes.erase(found);
			return ProcessSuccess(request, nlohmann::json::object());
		}

		if (type == "process.poll")
		{
			std::string output_error;
			std::string standard_output;
			std::string standard_error;
			{
				std::scoped_lock lock(process.mutex);
				if (!process.spool_error.empty()) output_error = process.spool_error;
				if (output_error.empty())
				{
					standard_output = ReadSpool(process.stdout_spool, process.stdout_offset,
					                            output_error);
					standard_error = ReadSpool(process.stderr_spool, process.stderr_offset,
					                           output_error);
				}
			}
			if (!output_error.empty())
				return ProcessError(request, "read_failed", std::move(output_error));
			const bool exited = process.exited.load(std::memory_order_acquire);
			const int exit_code = process.exit_code.load(std::memory_order_acquire);
			const bool output_pending = SpoolHasUnread(process.stdout_spool,
			                                           process.stdout_offset) ||
			                            SpoolHasUnread(process.stderr_spool,
			                                           process.stderr_offset);
			return ProcessSuccess(request,
			                      {{"sessionId", session_id},
			                       {"running", !exited || output_pending},
			                       {"outputPending", output_pending},
			                       {"exitCode", exited && !output_pending ? nlohmann::json(exit_code)
			                                                   : nlohmann::json(nullptr)},
			                       {"stdoutBase64", uam::base64::Encode(standard_output)},
			                       {"stderrBase64", uam::base64::Encode(standard_error)}});
		}

		return ProcessError(request, "unsupported_request", "Unknown remote process request.");
	}
}

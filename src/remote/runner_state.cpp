#include "remote/runner_state.h"

#include "common/paths/path_utils.h"
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
		inline constexpr auto kInitialIdleDrainDelay = std::chrono::milliseconds(10);
		inline constexpr auto kMaximumIdleDrainDelay = std::chrono::milliseconds(100);
		inline constexpr std::size_t kMaxChannelBytes = 1024 * 1024;
		inline constexpr std::size_t kMaxRememberedInputDeliveries = 64;
		inline constexpr std::int64_t kMaxTransientLeaseMs = 60000;
		inline constexpr std::size_t kMaxListedDirectories = 200;
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

		std::int64_t LeaseClockMilliseconds()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(
			           std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		void RenewLease(RunnerState::Process& process)
		{
			if (!process.transient_lease) return;
			process.lease_deadline_ms.store(
			    LeaseClockMilliseconds() + process.lease_duration_ms,
			    std::memory_order_release);
		}

		void CleanupProcess(const std::shared_ptr<RunnerState::Process>& process)
		{
			process->drainer.request_stop();
			{
				std::scoped_lock lock(process->mutex);
				if (!process->exited.load(std::memory_order_acquire))
					ProcessService().TerminateStdioProcess(process->fields, true);
				process->exited.store(true, std::memory_order_release);
			}
			if (process->drainer.joinable()) process->drainer.join();
			ProcessService().CloseStdioProcessHandles(process->fields);
			std::error_code remove_error;
			std::filesystem::remove(process->stdout_spool, remove_error);
			std::filesystem::remove(process->stderr_spool, remove_error);
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
			if (bytes.empty()) return true;
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

		std::string ReadSpool(const std::filesystem::path& path, std::uintmax_t offset,
		                      std::uintmax_t& end_offset, std::string& error)
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
			end_offset = offset + output.size();
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
				auto idle_delay = kInitialIdleDrainDelay;
				bool exit_observed = false;
				int exit_code = -1;
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
						if (process.transient_lease &&
						    LeaseClockMilliseconds() >= process.lease_deadline_ms.load(
						        std::memory_order_acquire))
						{
							std::scoped_lock lock(process.mutex);
							ProcessService().TerminateStdioProcess(process.fields, true);
							process.exit_code.store(-1, std::memory_order_release);
							process.exited.store(true, std::memory_order_release);
							return;
						}
						const bool exit_was_observed = exit_observed;
					std::string output_error;
					std::string stdout_bytes;
					std::string stderr_bytes;
					{
						std::scoped_lock lock(process.mutex);
						stdout_bytes = ReadAvailable(process, false, output_error);
						stderr_bytes = ReadAvailable(process, true, output_error);
						if (!exit_observed)
							exit_observed =
							    ProcessService().PollStdioProcessExited(process.fields, &exit_code);
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
					if (exit_observed && exit_was_observed &&
					    stdout_bytes.empty() && stderr_bytes.empty())
					{
						process.exit_code.store(exit_code, std::memory_order_release);
						process.exited.store(true, std::memory_order_release);
						return;
					}
					if (stdout_bytes.empty() && stderr_bytes.empty())
					{
						std::this_thread::sleep_for(idle_delay);
						idle_delay = std::min(idle_delay * 2, kMaximumIdleDrainDelay);
					}
					else
					{
						idle_delay = kInitialIdleDrainDelay;
					}
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
			std::filesystem::remove(upload->temporary, remove_error);
		}
		std::error_code cleanup_error;
		if (!m_spoolDirectory.empty())
			std::filesystem::remove_all(m_spoolDirectory, cleanup_error);
	}

	bool RunnerState::HasManagedProcesses()
	{
		SweepExpiredTransientProcesses();
		std::scoped_lock lock(m_stateMutex);
		return !m_processes.empty();
	}

	void RunnerState::SweepExpiredTransientProcesses()
	{
		std::vector<std::shared_ptr<Process>> expired;
		const std::int64_t now = LeaseClockMilliseconds();
		{
			std::scoped_lock lock(m_stateMutex);
			for (auto iterator = m_processes.begin(); iterator != m_processes.end();)
			{
				const std::shared_ptr<Process>& process = iterator->second;
				if (process->transient_lease &&
				    now >= process->lease_deadline_ms.load(std::memory_order_acquire))
				{
					expired.push_back(process);
					iterator = m_processes.erase(iterator);
				}
				else
				{
					++iterator;
				}
			}
		}
		for (const std::shared_ptr<Process>& process : expired) CleanupProcess(process);
	}

	nlohmann::json RunnerState::HandleProcessRequest(const nlohmann::json& request)
	{
		SweepExpiredTransientProcesses();
		std::unique_lock state_lock(m_stateMutex);
		const std::string type = request["type"].get<std::string>();
		if (type == "directory.list")
		{
			state_lock.unlock();
			if (!request.contains("path") || !request["path"].is_string())
				return ProcessError(request, "invalid_request", "An absolute directory path is required.");
			const std::string path_text = request["path"].get<std::string>();
			const std::filesystem::path requested(path_text);
			if (!IsBoundedText(path_text, kMaxWorkingDirectoryBytes) || !requested.is_absolute())
				return ProcessError(request, "invalid_request", "An absolute bounded directory path is required.");

			std::error_code error;
			const std::filesystem::file_status status = std::filesystem::status(requested, error);
			if (error == std::errc::permission_denied)
				return ProcessError(request, "permission_denied", "Permission denied while opening the remote directory.");
			if (error || !std::filesystem::exists(status))
				return ProcessError(request, "not_found", "The remote directory does not exist.");
			if (!std::filesystem::is_directory(status))
				return ProcessError(request, "not_directory", "The selected remote path is not a directory.");

			const std::filesystem::path directory = requested.lexically_normal();
			std::vector<std::pair<std::string, std::string>> directories;
			bool truncated = false;
			std::filesystem::directory_iterator iterator(directory, error);
			if (error == std::errc::permission_denied)
				return ProcessError(request, "permission_denied", "Permission denied while listing the remote directory.");
			if (error)
				return ProcessError(request, "list_failed", "The remote directory could not be listed.");
			const std::filesystem::directory_iterator end;
			for (; iterator != end && !error; iterator.increment(error))
			{
				const std::filesystem::directory_entry& entry = *iterator;
				std::error_code entry_error;
				if (!entry.is_directory(entry_error) || entry_error) continue;
				if (directories.size() == kMaxListedDirectories)
				{
					truncated = true;
					break;
				}
				directories.emplace_back(uam::paths::Utf8PathString(entry.path().filename()),
				                         uam::paths::Utf8PathString(entry.path().lexically_normal()));
			}
			if (error == std::errc::permission_denied)
				return ProcessError(request, "permission_denied", "Permission denied while listing the remote directory.");
			if (error)
				return ProcessError(request, "list_failed", "The remote directory could not be listed.");
			std::ranges::sort(directories, [](const auto& left, const auto& right)
			{ return left.first < right.first; });
			nlohmann::json entries = nlohmann::json::array();
			for (const auto& [name, path] : directories)
				entries.push_back({{"name", name}, {"path", path}});
			const std::filesystem::path parent = directory.parent_path();
			return ProcessSuccess(request,
			                      {{"directory", uam::paths::Utf8PathString(directory)},
			                       {"parentDirectory", parent == directory
			                                               ? ""
			                                               : uam::paths::Utf8PathString(parent)},
			                       {"directories", std::move(entries)},
			                       {"truncated", truncated}});
		}
		if (type.starts_with("file."))
		{
			if (!request.contains("uploadId") || !request["uploadId"].is_string() ||
			    !IsSessionId(request["uploadId"].get_ref<const std::string&>()))
				return ProcessError(request, "invalid_request",
				                    "A portable bounded uploadId is required.");
			const std::string upload_id = request["uploadId"].get<std::string>();
			if (type == "file.copy")
			{
				if (!request.contains("sourcePath") || !request["sourcePath"].is_string() ||
				    !request.contains("targetPath") || !request["targetPath"].is_string())
					return ProcessError(request, "invalid_request", "Source and target file paths are required.");
				const std::string source_text = request["sourcePath"].get<std::string>();
				const std::string target_text = request["targetPath"].get<std::string>();
				const std::filesystem::path source(source_text);
				const std::filesystem::path target(target_text);
				if (!IsBoundedText(source_text, kMaxWorkingDirectoryBytes) ||
				    !IsBoundedText(target_text, kMaxWorkingDirectoryBytes) ||
				    !source.is_absolute() || !target.is_absolute())
					return ProcessError(request, "invalid_request", "The source or target file path is invalid.");
				state_lock.unlock();
				std::error_code error;
				if (!std::filesystem::is_regular_file(source, error) || error)
					return ProcessError(request, "not_found", "The source file does not exist.");
				const auto options = request.value("overwrite", false)
				    ? std::filesystem::copy_options::overwrite_existing
				    : std::filesystem::copy_options::none;
				if (!std::filesystem::copy_file(source, target, options, error) || error)
					return ProcessError(request, "copy_failed", "The file could not be copied.");
				return ProcessSuccess(request, {{"path", target_text}});
			}
			if (type == "file.remove")
			{
				if (!request.contains("path") || !request["path"].is_string())
					return ProcessError(request, "invalid_request", "A file path is required.");
				const std::string path_text = request["path"].get<std::string>();
				const std::filesystem::path path(path_text);
				if (!IsBoundedText(path_text, kMaxWorkingDirectoryBytes) || !path.is_absolute())
					return ProcessError(request, "invalid_request", "The file path is invalid.");
				state_lock.unlock();
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
				auto upload = std::make_shared<Upload>();
				upload->target = target;
				upload->temporary = target.parent_path() / (".uam-upload-" + upload_id + ".tmp");
				upload->expected_size = size;
				upload->digest = uam::hashing::kFnv1a64OffsetBasis;
				upload->expected_digest = digest;
				std::unique_lock upload_lock(upload->mutex);
				m_uploads.emplace(upload_id, upload);
				state_lock.unlock();
				const auto abandon_upload = [&upload, &upload_lock, &upload_id, this]
				{
					std::error_code remove_error;
					std::filesystem::remove(upload->temporary, remove_error);
					upload->state = Upload::State::Finished;
					upload_lock.unlock();
					std::scoped_lock lock(m_stateMutex);
					const auto found = m_uploads.find(upload_id);
					if (found != m_uploads.end() && found->second == upload)
						m_uploads.erase(found);
				};
				std::error_code error;
				std::filesystem::create_directories(target.parent_path(), error);
				if (error || std::filesystem::exists(target, error) || error)
				{
					abandon_upload();
					return ProcessError(request, "target_unavailable",
					                    "The upload target already exists or cannot be created.");
				}
				std::ofstream stream(upload->temporary, std::ios::binary | std::ios::trunc);
				if (!stream)
				{
					abandon_upload();
					return ProcessError(request, "target_unavailable",
					                    "The upload temporary file could not be created.");
				}
				std::filesystem::permissions(
				    upload->temporary, std::filesystem::perms::owner_read |
				                   std::filesystem::perms::owner_write,
				    std::filesystem::perm_options::replace, error);
				upload->state = Upload::State::Active;
				return ProcessSuccess(request, {{"uploadId", upload_id}});
			}
			const auto found = m_uploads.find(upload_id);
			if (found == m_uploads.end())
				return ProcessError(request, "upload_not_found", "The upload does not exist.");
			const std::shared_ptr<Upload> upload_pointer = found->second;
			Upload& upload = *upload_pointer;
			std::unique_lock upload_lock(upload.mutex);
			if (upload.state == Upload::State::Starting)
				return ProcessError(request, "upload_starting", "The upload is still starting.");
			if (upload.state != Upload::State::Active)
				return ProcessError(request, "upload_finished", "The upload is already finishing or finished.");
			const auto finish_upload = [&upload, &upload_lock, &upload_id, &upload_pointer, this]
			{
				upload.state = Upload::State::Finished;
				upload_lock.unlock();
				std::scoped_lock lock(m_stateMutex);
				const auto current = m_uploads.find(upload_id);
				if (current != m_uploads.end() && current->second == upload_pointer)
					m_uploads.erase(current);
			};
			if (type == "file.abort")
			{
				upload.state = Upload::State::Finishing;
				state_lock.unlock();
				std::error_code error;
				std::filesystem::remove(upload.temporary, error);
				if (error)
				{
					upload.state = Upload::State::Active;
					return ProcessError(request, "abort_failed", "Upload temporary data could not be removed.");
				}
				finish_upload();
				return ProcessSuccess(request, nlohmann::json::object());
			}
			if (type == "file.commit")
			{
				if (upload.received_size != upload.expected_size ||
				    uam::hashing::Hex64Padded(upload.digest) != upload.expected_digest)
					return ProcessError(request, "digest_mismatch",
					                    "Upload size or digest verification failed.");
				upload.state = Upload::State::Finishing;
				state_lock.unlock();
				std::error_code error;
				std::filesystem::rename(upload.temporary, upload.target, error);
				if (error)
				{
					upload.state = Upload::State::Active;
					return ProcessError(request, "commit_failed", "Upload could not be committed.");
				}
				const std::string path = upload.target.string();
				finish_upload();
				return ProcessSuccess(request, {{"path", path}});
			}
			state_lock.unlock();
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
				auto [channel, inserted] = m_channels.try_emplace(channel_id);
				(void)inserted;
				return ProcessSuccess(request, {{"channelId", channel_id}, {"attached", attached},
				    {"remoteToDesktopCursor", channel->second.remote_to_desktop.base_cursor},
				    {"desktopToRemoteCursor", channel->second.desktop_to_remote.base_cursor},
				    {"remoteToDesktopWriteSequence", channel->second.remote_to_desktop.write_sequence},
				    {"desktopToRemoteWriteSequence", channel->second.desktop_to_remote.write_sequence}});
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
			ChannelBuffer* buffer = direction == "remoteToDesktop"
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
				const bool sequenced = request.contains("writeSequence");
				std::uint64_t write_sequence = 0;
				std::uint64_t write_digest = uam::hashing::kFnv1a64OffsetBasis;
				uam::hashing::UpdateFnv1a64(
				    write_digest, reinterpret_cast<const unsigned char*>(decoded.data()), decoded.size());
				if (sequenced)
				{
					if (!request["writeSequence"].is_number_unsigned())
						return ProcessError(request, "invalid_request", "The channel write sequence is invalid.");
					write_sequence = request["writeSequence"].get<std::uint64_t>();
					if (write_sequence == buffer->write_sequence)
					{
						if (decoded.size() != buffer->write_size || write_digest != buffer->write_digest)
							return ProcessError(request, "input_conflict", "The channel write sequence was reused with different bytes.");
						return ProcessSuccess(request, {{"acceptedBytes", decoded.size()},
						                                {"writeSequence", write_sequence}, {"duplicate", true}});
					}
					if (write_sequence != buffer->write_sequence + 1)
						return ProcessError(request, "input_sequence_gap", "The channel write sequence is not contiguous.");
				}
				if (decoded.size() > kMaxWriteBytes ||
				    decoded.size() > kMaxChannelBytes - buffer->bytes.size())
					return ProcessError(request, "channel_full", "The channel buffer limit was reached.");
				buffer->bytes.append(decoded);
				if (sequenced)
				{
					buffer->write_sequence = write_sequence;
					buffer->write_digest = write_digest;
					buffer->write_size = decoded.size();
				}
				return ProcessSuccess(request, {{"acceptedBytes", decoded.size()},
				                                {"writeSequence", buffer->write_sequence}});
			}
			if (type == "channel.poll")
			{
				const bool acknowledged = request.value("acknowledgedOutput", false);
				std::uintmax_t cursor = buffer->base_cursor;
				if (acknowledged && request.contains("cursor"))
				{
					if (!request["cursor"].is_number_unsigned())
						return ProcessError(request, "invalid_request", "The channel cursor is invalid.");
					cursor = request["cursor"].get<std::uintmax_t>();
					if (cursor < buffer->base_cursor ||
					    cursor > buffer->base_cursor + buffer->bytes.size())
						return ProcessError(request, "invalid_request", "The channel cursor is invalid.");
				}
				const std::size_t offset = static_cast<std::size_t>(cursor - buffer->base_cursor);
				const std::size_t size = std::min(buffer->bytes.size() - offset,
				                                  kMaxReadBytesPerStream);
				const std::string bytes = buffer->bytes.substr(offset, size);
				const std::uintmax_t end_cursor = cursor + size;
				if (!acknowledged)
				{
					buffer->bytes.erase(0, offset + size);
					buffer->base_cursor = end_cursor;
				}
				return ProcessSuccess(request, {{"dataBase64", uam::base64::Encode(bytes)},
				                                {"cursor", end_cursor}});
			}
			if (type == "channel.ack")
			{
				if (!request.contains("cursor") || !request["cursor"].is_number_unsigned())
					return ProcessError(request, "invalid_request", "The channel cursor is required.");
				const std::uintmax_t cursor = request["cursor"].get<std::uintmax_t>();
				if (cursor < buffer->base_cursor ||
				    cursor > buffer->base_cursor + buffer->bytes.size())
					return ProcessError(request, "invalid_request", "The channel cursor is invalid.");
				buffer->bytes.erase(0, static_cast<std::size_t>(cursor - buffer->base_cursor));
				buffer->base_cursor = cursor;
				return ProcessSuccess(request, {{"cursor", cursor}});
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
			if (!request.contains("controlToken") || !request["controlToken"].is_string() ||
			    !IsBoundedText(request["controlToken"].get_ref<const std::string&>(), 256) ||
			    request["controlToken"].get_ref<const std::string&>().empty())
				return ProcessError(request, "invalid_request",
				                    "A bounded process control token is required.");
			const std::string control_token = request["controlToken"].get<std::string>();
			std::int64_t transient_lease_ms = 0;
			if (request.contains("transientLeaseMs"))
			{
				if (!request["transientLeaseMs"].is_number_integer())
					return ProcessError(request, "invalid_request",
					                    "The transient process lease is invalid.");
				transient_lease_ms = request["transientLeaseMs"].get<std::int64_t>();
				if (transient_lease_ms <= 0 || transient_lease_ms > kMaxTransientLeaseMs)
					return ProcessError(request, "invalid_request",
					                    "The transient process lease is out of range.");
			}
			if (const auto existing = m_processes.find(session_id); existing != m_processes.end())
			{
				if (existing->second->control_token != control_token)
					return ProcessError(request, "unauthorized",
					                    "The remote process control token is invalid.");
				if (!request.value("attachIfExists", false))
					return ProcessError(request, "session_exists",
					                    "A remote process already uses this sessionId.");
				Process& process = *existing->second;
				if (process.working_directory != working_directory ||
				    process.arguments != arguments || process.environment != environment)
					return ProcessError(request, "session_conflict",
					                    "The existing remote process does not match this launch request.");
				if (!process.ready.load(std::memory_order_acquire))
					return ProcessError(request, "session_starting",
					                    "The existing remote process is still starting.");
				std::scoped_lock process_lock(process.mutex);
				RenewLease(process);
				const bool exited = process.exited.load(std::memory_order_acquire);
				return ProcessSuccess(request,
				                      {{"sessionId", session_id}, {"running", !exited},
				                       {"attached", true},
				                       {"inputSequence", process.input_sequence}});
			}
			auto process = std::make_shared<Process>();
			process->working_directory = working_directory;
			process->arguments = arguments;
			process->environment = environment;
			process->control_token = control_token;
			process->transient_lease = transient_lease_ms > 0;
			process->lease_duration_ms = transient_lease_ms;
			RenewLease(*process);
			if (m_spoolDirectory.empty())
				return ProcessError(request, "spool_unavailable",
				                    "The remote process output spool is unavailable.");
			process->stdout_spool = m_spoolDirectory / (session_id + ".stdout");
			process->stderr_spool = m_spoolDirectory / (session_id + ".stderr");
			m_processes.emplace(session_id, process);
			state_lock.unlock();
			const auto abandon_start = [&]
			{
				std::scoped_lock lock(m_stateMutex);
				const auto found = m_processes.find(session_id);
				if (found != m_processes.end() && found->second == process)
					m_processes.erase(found);
			};
			{
				std::ofstream stdout_file(process->stdout_spool, std::ios::binary);
				std::ofstream stderr_file(process->stderr_spool, std::ios::binary);
				if (!stdout_file || !stderr_file)
				{
					abandon_start();
					return ProcessError(request, "spool_unavailable",
					                    "The remote process output spool could not be created.");
				}
			}
			if (!ProcessService().StartStdioProcess(process->fields, working_directory,
			                                                arguments, &error, environment))
			{
				abandon_start();
				return ProcessError(request, "start_failed",
				                    error.empty() ? "The remote process could not start." : error);
			}
			StartDrainer(*process);
			process->ready.store(true, std::memory_order_release);
			return ProcessSuccess(request, {{"sessionId", session_id}, {"running", true},
			                                {"inputSequence", 0}});
		}

		if (!request.contains("sessionId") || !request["sessionId"].is_string() ||
		    !IsSessionId(request["sessionId"].get_ref<const std::string&>()))
			return ProcessError(request, "invalid_request",
			                    "A portable bounded sessionId is required.");
		const std::string session_id = request["sessionId"].get<std::string>();
		const auto found = m_processes.find(session_id);
		if (found == m_processes.end())
			return ProcessError(request, "session_not_found", "The remote process does not exist.");
		const std::shared_ptr<Process> process_pointer = found->second;
		Process& process = *process_pointer;
		if (!request.contains("controlToken") || !request["controlToken"].is_string() ||
		    request["controlToken"].get_ref<const std::string&>() != process.control_token)
			return ProcessError(request, "unauthorized",
			                    "The remote process control token is invalid.");
		if (!process.ready.load(std::memory_order_acquire))
			return ProcessError(request, "session_starting",
			                    "The remote process is still starting.");
		RenewLease(process);
		if (type == "process.remove")
		{
			if (!process.exited.load(std::memory_order_acquire))
				return ProcessError(request, "process_running",
				                    "Stop the remote process before removing it.");
			m_processes.erase(found);
			state_lock.unlock();
			process.drainer.request_stop();
			if (process.drainer.joinable()) process.drainer.join();
			{
				std::scoped_lock lock(process.mutex);
				ProcessService().CloseStdioProcessHandles(process.fields);
				std::error_code remove_error;
				std::filesystem::remove(process.stdout_spool, remove_error);
				std::filesystem::remove(process.stderr_spool, remove_error);
			}
			return ProcessSuccess(request, nlohmann::json::object());
		}
		state_lock.unlock();

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
			const bool sequenced = request.contains("inputSequence");
			const bool delivered = request.contains("deliveryId");
			std::uint64_t input_sequence = 0;
			std::uint64_t input_digest = uam::hashing::kFnv1a64OffsetBasis;
			uam::hashing::UpdateFnv1a64(
			    input_digest, reinterpret_cast<const unsigned char*>(decoded.data()), decoded.size());
			std::string error;
			std::scoped_lock lock(process.mutex);
			std::string delivery_id;
			if (delivered)
			{
				if (!request["deliveryId"].is_string() ||
				    !IsBoundedText(request["deliveryId"].get_ref<const std::string&>(), 128))
					return ProcessError(request, "invalid_request",
					                    "The process input delivery id is invalid.");
				delivery_id = request["deliveryId"].get<std::string>();
				const auto remembered = process.input_deliveries.find(delivery_id);
				if (remembered != process.input_deliveries.end())
				{
					if (decoded.size() != remembered->second.size ||
					    input_digest != remembered->second.digest)
						return ProcessError(request, "input_conflict",
						                    "The process input delivery id was reused with different bytes.");
					return ProcessSuccess(request, {{"acceptedBytes", decoded.size()},
					                                {"inputSequence", remembered->second.sequence},
					                                {"deliveryId", delivery_id}, {"duplicate", true}});
				}
			}
			if (sequenced)
			{
				if (!request["inputSequence"].is_number_unsigned())
					return ProcessError(request, "invalid_request", "Process input sequence is invalid.");
				input_sequence = request["inputSequence"].get<std::uint64_t>();
				if (input_sequence == process.input_sequence)
				{
					if (decoded.size() != process.input_size || input_digest != process.input_digest)
						return ProcessError(request, "input_conflict", "Process input sequence was reused with different bytes.");
					return ProcessSuccess(request, {{"acceptedBytes", decoded.size()},
					                                {"inputSequence", input_sequence}, {"duplicate", true}});
				}
				if (input_sequence != process.input_sequence + 1)
					return ProcessError(request, "input_sequence_gap", "Process input sequence is not contiguous.");
			}
			if (!ProcessService().WriteToStdioProcess(process.fields, decoded.data(), decoded.size(), &error))
				return ProcessError(request, "write_failed", std::move(error));
			if (sequenced)
			{
				process.input_sequence = input_sequence;
				process.input_digest = input_digest;
				process.input_size = decoded.size();
			}
			if (delivered)
			{
				process.input_deliveries.emplace(
				    delivery_id, Process::InputDelivery{input_digest, decoded.size(),
				                                        process.input_sequence});
				process.input_delivery_order.push_back(delivery_id);
				while (process.input_delivery_order.size() > kMaxRememberedInputDeliveries)
				{
					process.input_deliveries.erase(process.input_delivery_order.front());
					process.input_delivery_order.pop_front();
				}
			}
			return ProcessSuccess(request, {{"acceptedBytes", decoded.size()},
			                                {"inputSequence", process.input_sequence},
			                                {"deliveryId", delivery_id}});
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

		if (type == "process.poll")
		{
			const bool acknowledged_output = request.value("acknowledgedOutput", false);
			std::string output_error;
			std::string standard_output;
			std::string standard_error;
			std::uintmax_t stdout_cursor = 0;
			std::uintmax_t stderr_cursor = 0;
			std::uint64_t input_sequence = 0;
			bool output_pending = false;
			{
				std::scoped_lock lock(process.mutex);
				if (!process.spool_error.empty()) output_error = process.spool_error;
				if (output_error.empty())
				{
					std::uintmax_t stdout_offset = process.stdout_offset;
					std::uintmax_t stderr_offset = process.stderr_offset;
					if (acknowledged_output &&
					    (request.contains("stdoutCursor") || request.contains("stderrCursor")))
					{
						if (!request.contains("stdoutCursor") ||
						    !request["stdoutCursor"].is_number_unsigned() ||
						    !request.contains("stderrCursor") ||
						    !request["stderrCursor"].is_number_unsigned())
							return ProcessError(request, "invalid_request",
							                    "Both process output cursors are required.");
						stdout_offset = request["stdoutCursor"].get<std::uintmax_t>();
						stderr_offset = request["stderrCursor"].get<std::uintmax_t>();
						std::error_code stdout_size_error;
						std::error_code stderr_size_error;
						const std::uintmax_t stdout_size =
						    std::filesystem::file_size(process.stdout_spool, stdout_size_error);
						const std::uintmax_t stderr_size =
						    std::filesystem::file_size(process.stderr_spool, stderr_size_error);
						if (stdout_size_error || stderr_size_error || stdout_offset > stdout_size ||
						    stderr_offset > stderr_size)
							return ProcessError(request, "invalid_request",
							                    "Process output resume cursor is invalid.");
					}
					standard_output = ReadSpool(process.stdout_spool, stdout_offset,
					                            stdout_cursor, output_error);
					standard_error = ReadSpool(process.stderr_spool, stderr_offset,
					                           stderr_cursor, output_error);
					if (!acknowledged_output)
					{
						process.stdout_offset = stdout_cursor;
						process.stderr_offset = stderr_cursor;
					}
				}
				output_pending = SpoolHasUnread(process.stdout_spool, stdout_cursor) ||
				                 SpoolHasUnread(process.stderr_spool, stderr_cursor);
				input_sequence = process.input_sequence;
			}
			if (!output_error.empty())
				return ProcessError(request, "read_failed", std::move(output_error));
			const bool exited = process.exited.load(std::memory_order_acquire);
			const int exit_code = process.exit_code.load(std::memory_order_acquire);
			return ProcessSuccess(request,
			                      {{"sessionId", session_id},
			                       {"running", !exited || output_pending},
			                       {"outputPending", output_pending},
				                       {"exitCode", exited && !output_pending ? nlohmann::json(exit_code)
				                                                   : nlohmann::json(nullptr)},
				                       {"stdoutCursor", stdout_cursor},
				                       {"stderrCursor", stderr_cursor},
					                       {"inputSequence", input_sequence},
				                       {"stdoutBase64", uam::base64::Encode(standard_output)},
				                       {"stderrBase64", uam::base64::Encode(standard_error)}});
		}

		if (type == "process.ack")
		{
			if (!request.contains("stdoutCursor") || !request["stdoutCursor"].is_number_unsigned() ||
			    !request.contains("stderrCursor") || !request["stderrCursor"].is_number_unsigned())
				return ProcessError(request, "invalid_request", "Process output cursors are required.");
			const std::uintmax_t stdout_cursor = request["stdoutCursor"].get<std::uintmax_t>();
			const std::uintmax_t stderr_cursor = request["stderrCursor"].get<std::uintmax_t>();
			std::scoped_lock lock(process.mutex);
			std::error_code stdout_error;
			std::error_code stderr_error;
			const std::uintmax_t stdout_size = std::filesystem::file_size(process.stdout_spool, stdout_error);
			const std::uintmax_t stderr_size = std::filesystem::file_size(process.stderr_spool, stderr_error);
			if (stdout_error || stderr_error || stdout_cursor > stdout_size ||
			    stderr_cursor > stderr_size)
				return ProcessError(request, "invalid_request", "Process output cursor is invalid.");
			process.stdout_offset = std::max(process.stdout_offset, stdout_cursor);
			process.stderr_offset = std::max(process.stderr_offset, stderr_cursor);
			return ProcessSuccess(request, {{"stdoutCursor", stdout_cursor},
			                                {"stderrCursor", stderr_cursor}});
		}

		return ProcessError(request, "unsupported_request", "Unknown remote process request.");
	}
}

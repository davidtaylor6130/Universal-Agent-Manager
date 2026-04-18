#pragma once

#include "common/models/app_models.h"

#include <filesystem>
#include <ctime>
#include <optional>
#include <cstddef>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace uam
{
	struct CliTerminalState;

	namespace platform
	{
		struct StdioProcessPlatformFields;

		class DataRootLock
		{
		  public:
			virtual ~DataRootLock() = default;
		};
	} // namespace platform
} // namespace uam

enum class PlatformPathBrowseTarget
{
	Directory,
	File,
};

/// <summary>
/// Platform terminal runtime abstraction boundary.
/// </summary>
class IPlatformTerminalRuntime
{
  public:
	virtual ~IPlatformTerminalRuntime() = default;
	virtual bool IsAvailable() const = 0;
	virtual bool StartCliTerminalProcess(uam::CliTerminalState& terminal,
	                                     const std::filesystem::path& working_directory,
	                                     const std::vector<std::string>& argv,
	                                     std::string* error_out = nullptr) const = 0;
	virtual void CloseCliTerminalHandles(uam::CliTerminalState& terminal) const = 0;
	virtual bool WriteToCliTerminal(uam::CliTerminalState& terminal, const char* bytes, std::size_t len) const = 0;
	virtual void StopCliTerminalProcess(uam::CliTerminalState& terminal, bool fast_exit) const = 0;
	virtual void ResizeCliTerminal(uam::CliTerminalState& terminal) const = 0;
	// Returns: >0 bytes read, 0 process exited, -2 no data available, -1 read failure.
	virtual std::ptrdiff_t ReadCliTerminalOutput(uam::CliTerminalState& terminal, char* buffer, std::size_t buffer_size) const = 0;
	virtual bool HasReadableTerminalOutputHandle(const uam::CliTerminalState& terminal) const = 0;
	virtual bool PollCliTerminalProcessExited(uam::CliTerminalState& terminal) const = 0;
	virtual bool SupportsAsyncNativeGeminiHistoryRefresh() const = 0;
};

/// <summary>
/// Platform process spawning/service abstraction boundary.
/// </summary>
class IPlatformProcessService
{
  public:
	virtual ~IPlatformProcessService() = default;
	virtual bool SupportsDetachedProcesses() const = 0;
	virtual bool PopulateLocalTime(std::time_t timestamp, std::tm* tm_out) const = 0;
	virtual std::string BuildShellCommandWithWorkingDirectory(const std::filesystem::path& working_directory, const std::string& command) const = 0;
	virtual bool CaptureCommandOutput(const std::string& command, std::string* output_out, int* raw_status_out, std::string* error_out = nullptr) const = 0;
	virtual int NormalizeCapturedCommandExitCode(int raw_status) const = 0;
	virtual ProcessExecutionResult ExecuteCommand(const std::string& command,
	                                              int timeout_ms = -1,
	                                              std::stop_token stop_token = {}) const = 0;
	virtual bool StartStdioProcess(uam::platform::StdioProcessPlatformFields& process,
	                               const std::filesystem::path& working_directory,
	                               const std::vector<std::string>& argv,
	                               std::string* error_out = nullptr) const = 0;
	virtual void CloseStdioProcessHandles(uam::platform::StdioProcessPlatformFields& process) const = 0;
		virtual bool WriteToStdioProcess(uam::platform::StdioProcessPlatformFields& process, const char* bytes, std::size_t len, std::string* error_out = nullptr) const = 0;
		virtual void StopStdioProcess(uam::platform::StdioProcessPlatformFields& process, bool fast_exit) const = 0;
		virtual std::ptrdiff_t ReadStdioProcessStdout(uam::platform::StdioProcessPlatformFields& process, char* buffer, std::size_t buffer_size, std::string* error_out = nullptr) const = 0;
		virtual std::ptrdiff_t ReadStdioProcessStderr(uam::platform::StdioProcessPlatformFields& process, char* buffer, std::size_t buffer_size, std::string* error_out = nullptr) const = 0;
		virtual bool PollStdioProcessExited(uam::platform::StdioProcessPlatformFields& process, int* exit_code_out = nullptr) const = 0;
	virtual std::string GeminiDowngradeCommand() const = 0;
	virtual std::filesystem::path ResolveCurrentExecutablePath() const = 0;
	virtual std::unique_ptr<uam::platform::DataRootLock> TryAcquireDataRootLock(const std::filesystem::path& data_root, std::string* error_out = nullptr) const = 0;
	virtual uintmax_t NativeGeminiSessionMaxFileBytes() const = 0;
	virtual std::size_t NativeGeminiSessionMaxMessages() const = 0;
	virtual std::string GenerateUuid() const = 0;
};

/// <summary>
/// Platform-native file dialog abstraction boundary.
/// </summary>
class IPlatformFileDialogService
{
  public:
	virtual ~IPlatformFileDialogService() = default;
	virtual bool SupportsNativeDialogs() const = 0;
	virtual bool BrowsePath(PlatformPathBrowseTarget target, const std::filesystem::path& initial_path, std::string* selected_path_out, std::string* error_out = nullptr) const = 0;
	virtual bool OpenFolderInFileManager(const std::filesystem::path& folder_path, std::string* error_out = nullptr) const = 0;
	virtual bool RevealPathInFileManager(const std::filesystem::path& file_path, std::string* error_out = nullptr) const = 0;
};

/// <summary>
/// Platform path/default-location abstraction boundary.
/// </summary>
class IPlatformPathService
{
  public:
	virtual ~IPlatformPathService() = default;
	virtual std::filesystem::path DefaultDataRootPath() const = 0;
	virtual std::optional<std::filesystem::path> ResolveUserHomePath() const = 0;
	virtual std::filesystem::path ExpandLeadingTildePath(const std::string& raw_path) const = 0;
};

/// <summary>
/// Aggregated platform services used by core/orchestration code.
/// </summary>
struct PlatformServices
{
	IPlatformTerminalRuntime& terminal_runtime;
	IPlatformProcessService& process_service;
	IPlatformFileDialogService& file_dialog_service;
	IPlatformPathService& path_service;
};

/// <summary>
/// Creates and owns platform service implementations.
/// </summary>
class PlatformServicesFactory
{
  public:
	static PlatformServices& Instance();
};

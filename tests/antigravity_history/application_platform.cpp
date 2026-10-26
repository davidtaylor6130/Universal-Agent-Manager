// Only the process/UI/ambient-path boundary is substituted. Reader, provider
// identity validation, repository, folder save and publication code stay real.
#include "application_platform.h"
#include "common/platform/platform_services.h"
#include "common/paths/path_utils.h"
#include <atomic>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace
{
	std::filesystem::path fixture_root;
	std::atomic<std::size_t> unexpected{0}, identities{0};
	bool Denied(std::string* error = nullptr)
	{
		++unexpected;
		if (error)
			*error = "Process/UI work is forbidden in native import fixtures.";
		return false;
	}
	class Process final : public IPlatformProcessService
	{
	  public:
		bool SupportsDetachedProcesses() const override
		{
			return false;
		}
		bool PopulateLocalTime(std::time_t stamp, std::tm* output) const override
		{
			static std::mutex mutex;
			std::lock_guard lock(mutex);
			const auto* value = std::localtime(&stamp);
			if (!value || !output)
				return false;
			*output = *value;
			return true;
		}
		std::string BuildShellCommandWithWorkingDirectory(const std::filesystem::path&, const std::string&) const override
		{
			Denied();
			return {};
		}
		bool CaptureCommandOutput(const std::string&, std::string*, int*, std::string* error) const override
		{
			return Denied(error);
		}
		int NormalizeCapturedCommandExitCode(int status) const override
		{
			return status;
		}
		ProcessExecutionResult ExecuteCommand(const std::string&, int, std::stop_token) const override
		{
			Denied();
			return {};
		}
		bool StartStdioProcess(uam::platform::StdioProcessPlatformFields&, const std::filesystem::path&, const std::vector<std::string>&, std::string* error, const std::vector<std::pair<std::string, std::string>>&) const override
		{
			return Denied(error);
		}
		bool StartStdioProcessWithInput(uam::platform::StdioProcessPlatformFields&, const std::filesystem::path&, const std::vector<std::string>&, std::string_view, std::string* error, const std::vector<std::pair<std::string, std::string>>&) const override
		{
			return Denied(error);
		}
		void CloseStdioProcessHandles(uam::platform::StdioProcessPlatformFields&) const override
		{
			Denied();
		}
		bool WriteToStdioProcess(uam::platform::StdioProcessPlatformFields&, const char*, std::size_t, std::string* error) const override
		{
			return Denied(error);
		}
		void CloseStdioProcessInput(uam::platform::StdioProcessPlatformFields&) const override
		{
			Denied();
		}
		void TerminateStdioProcess(uam::platform::StdioProcessPlatformFields&, bool) const override
		{
			Denied();
		}
		void StopStdioProcess(uam::platform::StdioProcessPlatformFields&, bool) const override
		{
			Denied();
		}
		std::ptrdiff_t ReadStdioProcessStdout(uam::platform::StdioProcessPlatformFields&, char*, std::size_t, std::string* error) const override
		{
			Denied(error);
			return -1;
		}
		std::ptrdiff_t ReadStdioProcessStderr(uam::platform::StdioProcessPlatformFields&, char*, std::size_t, std::string* error) const override
		{
			Denied(error);
			return -1;
		}
		bool PollStdioProcessExited(uam::platform::StdioProcessPlatformFields&, int*) const override
		{
			return Denied();
		}
		std::filesystem::path ResolveCurrentExecutablePath() const override
		{
			Denied();
			return {};
		}
		std::unique_ptr<uam::platform::DataRootLock> TryAcquireDataRootLock(const std::filesystem::path&, std::string* error) const override
		{
			Denied(error);
			return {};
		}
		uintmax_t NativeGeminiSessionMaxFileBytes() const override
		{
			return 64U * 1024U * 1024U;
		}
		std::size_t NativeGeminiSessionMaxMessages() const override
		{
			return 100000;
		}
		std::string GenerateUuid() const override
		{
			// Synthetic UAM object/asset generations, never provider session IDs.
			std::ostringstream out;
			out << "00000000-0000-4000-8000-" << std::hex << std::setfill('0') << std::setw(12) << ++identities;
			return out.str();
		}
		bool LaunchShellAt(const std::filesystem::path&, std::string* error) const override
		{
			return Denied(error);
		}
		bool LaunchTerminalCommand(const std::vector<std::string>&, std::string* error, std::string_view) const override
		{
			return Denied(error);
		}
		void SetKeepSystemAwake(bool) const override
		{
			Denied();
		}
		bool EmbeddedBrowserUsesMockKeychain() const override
		{
			return true;
		}
	};
	class Terminal final : public IPlatformTerminalRuntime
	{
	  public:
		bool IsAvailable() const override
		{
			return false;
		}
		bool StartCliTerminalProcess(uam::CliTerminalState&, const std::filesystem::path&, const std::vector<std::string>&, std::string* error, const std::vector<std::pair<std::string, std::string>>&) const override
		{
			return Denied(error);
		}
		void CloseCliTerminalHandles(uam::CliTerminalState&) const override
		{
			Denied();
		}
		bool WriteToCliTerminal(uam::CliTerminalState&, const char*, std::size_t) const override
		{
			return Denied();
		}
		void StopCliTerminalProcess(uam::CliTerminalState&, bool) const override
		{
			Denied();
		}
		void ResizeCliTerminal(uam::CliTerminalState&) const override
		{
			Denied();
		}
		std::ptrdiff_t ReadCliTerminalOutput(uam::CliTerminalState&, char*, std::size_t) const override
		{
			Denied();
			return -1;
		}
		bool HasReadableTerminalOutputHandle(const uam::CliTerminalState&) const override
		{
			return false;
		}
		bool PollCliTerminalProcessExited(uam::CliTerminalState&) const override
		{
			return Denied();
		}
		bool SupportsAsyncNativeGeminiHistoryRefresh() const override
		{
			return false;
		}
	};
	class Dialog final : public IPlatformFileDialogService
	{
	  public:
		bool SupportsNativeDialogs() const override
		{
			return false;
		}
		bool BrowsePath(PlatformPathBrowseTarget, const std::filesystem::path&, std::string*, std::string* error) const override
		{
			return Denied(error);
		}
		bool OpenFolderInFileManager(const std::filesystem::path&, std::string* error, const std::filesystem::path&) const override
		{
			return Denied(error);
		}
		bool OpenFolderInEditorPreset(const std::filesystem::path&, const std::string&, std::string* error) const override
		{
			return Denied(error);
		}
		bool RevealPathInFileManager(const std::filesystem::path&, std::string* error) const override
		{
			return Denied(error);
		}
		bool OpenFileInTextEditor(const std::filesystem::path&, std::string* error) const override
		{
			return Denied(error);
		}
	};
	class Paths final : public IPlatformPathService
	{
	  public:
		std::filesystem::path DefaultDataRootPath() const override
		{
			return fixture_root / "default-data";
		}
		std::optional<std::filesystem::path> ResolveUserHomePath() const override
		{
			return fixture_root;
		}
		std::filesystem::path ExpandLeadingTildePath(const std::string& path) const override
		{
			return path.starts_with("~/") ? fixture_root / uam::paths::PathFromUtf8(path.substr(2)) : uam::paths::PathFromUtf8(path);
		}
		bool CanProbeDirectoryWithoutPrompt(const std::filesystem::path& path) const override
		{
			const auto relative = path.lexically_normal().lexically_relative(fixture_root.lexically_normal());
			return !relative.empty() && *relative.begin() != "..";
		}
		bool EnsureHiddenDirectory(const std::filesystem::path& path, std::error_code* error) const override
		{
			if (!CanProbeDirectoryWithoutPrompt(path))
				return Denied();
			std::error_code result;
			std::filesystem::create_directories(path, result);
			if (error)
				*error = result;
			return !result;
		}
	};
	class Dictation final : public IPlatformDictationService
	{
	  public:
		bool Start(const DictationOptions&, std::string* error) override
		{
			return Denied(error);
		}
		void Stop(std::string_view) override
		{
			Denied();
		}
		bool IsRunning() const override
		{
			return false;
		}
		std::vector<DictationEvent> PollEvents() override
		{
			Denied();
			return {};
		}
	};
} // namespace

PlatformServices& PlatformServicesFactory::Instance()
{
	static Process process;
	static Terminal terminal;
	static Dialog dialog;
	static Paths paths;
	static Dictation dictation;
	static PlatformServices services{terminal, process, dialog, paths, dictation};
	return services;
}

namespace uam_test
{
	void SetNativeImportFixtureRoot(const std::filesystem::path& root)
	{
		fixture_root = root;
	}
	std::size_t NativeImportFixtureUnexpectedCalls()
	{
		return unexpected.load();
	}
} // namespace uam_test

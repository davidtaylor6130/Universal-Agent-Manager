#include "computer_use/computer_use_install_macos.h"
#include "computer_use/computer_use_install_staging.h"
#include "computer_use/computer_use_mcp_config.h"

#include "common/paths/app_paths.h"
#include "common/utils/env_utils.h"
#include "common/platform/data_folder_writer_lease.h"
#include "app/data_folder_transition.h"

#import <Foundation/Foundation.h>

#include <CommonCrypto/CommonDigest.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <array>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace uam::computer_use
{
	namespace
	{
		constexpr auto kTimeout = std::chrono::seconds(30);
		constexpr const char* kIdentifier = "com.universalagentmanager.desktop.computer-use";

		void SetError(std::string* error, const std::string& message)
		{
			if (error != nullptr) *error = message;
		}

		class Lock
		{
		  public:
			explicit Lock(const std::filesystem::path& path)
			{
				m_fd = open(path.c_str(), O_CREAT | O_RDWR, 0600);
				if (m_fd < 0) return;
				const auto deadline = std::chrono::steady_clock::now() + kTimeout;
				while (flock(m_fd, LOCK_EX | LOCK_NB) != 0)
				{
					if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) { close(m_fd); m_fd = -1; return; }
					if (std::chrono::steady_clock::now() >= deadline) { close(m_fd); m_fd = -1; return; }
					std::this_thread::sleep_for(std::chrono::milliseconds(25));
				}
			}
			~Lock()
			{
				if (m_fd >= 0) { (void)flock(m_fd, LOCK_UN); close(m_fd); }
			}
			bool acquired() const { return m_fd >= 0; }

		  private:
			int m_fd = -1;
		};

		std::string Sha256(const std::filesystem::path& path, std::string* error)
		{
			std::ifstream input(path, std::ios::binary);
			if (!input) { SetError(error, "Could not read " + path.string() + "."); return {}; }
			CC_SHA256_CTX context{};
			CC_SHA256_Init(&context);
			std::array<char, 1024 * 1024> buffer{};
			while (input)
			{
				input.read(buffer.data(), buffer.size());
				if (input.gcount() > 0) CC_SHA256_Update(&context, buffer.data(), static_cast<CC_LONG>(input.gcount()));
			}
			if (!input.eof()) { SetError(error, "Could not read " + path.string() + "."); return {}; }
			unsigned char digest[CC_SHA256_DIGEST_LENGTH]{};
			CC_SHA256_Final(digest, &context);
			std::ostringstream result;
			result << std::hex;
			for (unsigned char byte : digest) result.width(2), result.fill('0'), result << static_cast<unsigned int>(byte);
			return result.str();
		}

		bool RunTool(const std::vector<std::string>& arguments, std::string* error)
		{
			std::vector<char*> argv;
			for (const std::string& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
			argv.push_back(nullptr);
			pid_t pid = 0;
			const int spawn_error = posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ);
			if (spawn_error != 0) { SetError(error, "Could not start codesign: " + std::string(std::strerror(spawn_error)) + "."); return false; }
			int status = 0;
			const auto deadline = std::chrono::steady_clock::now() + kTimeout;
			while (std::chrono::steady_clock::now() < deadline)
			{
				const pid_t result = waitpid(pid, &status, WNOHANG);
				if (result == pid)
				{
					if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
					SetError(error, "codesign failed with exit code " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status)) + ".");
					return false;
				}
				if (result < 0 && errno != EINTR) { SetError(error, "Could not wait for codesign: " + std::string(std::strerror(errno)) + "."); return false; }
				std::this_thread::sleep_for(std::chrono::milliseconds(25));
			}
			kill(pid, SIGKILL);
			(void)waitpid(pid, &status, 0);
			SetError(error, "codesign timed out after 30 seconds.");
			return false;
		}

		bool CopyItem(NSFileManager* manager, const std::filesystem::path& source, const std::filesystem::path& destination, std::string* error)
		{
			NSError* ns_error = nil;
			if ([manager copyItemAtPath:[NSString stringWithUTF8String:source.c_str()] toPath:[NSString stringWithUTF8String:destination.c_str()] error:&ns_error]) return true;
			SetError(error, "Could not copy Computer Use helper: " + std::string(ns_error.localizedDescription.UTF8String));
			return false;
		}

		constexpr const char* kInstallFlag = "--uam-computer-use-install";

		/// Validated bundle hashes plus the leased install folder.
		struct InstallContext
		{
			std::shared_ptr<platform::DataFolderWriterLease> writer;
			std::filesystem::path root;
			std::string binary_hash;
			std::string plist_hash;
		};

		bool OpenInstallContext(const std::filesystem::path& embedded_app, InstallContext& context, std::string* error)
		{
			const std::filesystem::path source_binary = embedded_app / "Contents" / "MacOS" / "UAM Computer Use";
			const std::filesystem::path source_plist = embedded_app / "Contents" / "Info.plist";
			if (embedded_app.filename() != "UAM Computer Use.app" || !std::filesystem::is_directory(embedded_app) ||
			    !std::filesystem::is_regular_file(source_binary) || !std::filesystem::is_regular_file(source_plist))
			{
				SetError(error, "Embedded Computer Use helper bundle is missing or invalid.");
				return false;
			}
			context.binary_hash = Sha256(source_binary, error);
			if (context.binary_hash.empty()) return false;
			context.plist_hash = Sha256(source_plist, error);
			if (context.plist_hash.empty()) return false;

			std::filesystem::path profile;
			std::string ownership_error;
			if (const auto effective = env::GetTrimmedPath("UAM_DATA_DIR"))
				profile = *effective;
			else if (!DataFolderTransition::ProbeStartupRoot(AppPaths::DefaultDataRootPath(), {}, profile, ownership_error))
			{
				SetError(error, ownership_error);
				return false;
			}
			context.writer = platform::DataFolderWriterLease::TryAcquire(profile, platform::DataFolderWriterLease::Access::Shared, ownership_error);
			if (!context.writer || !DataFolderTransition::CheckExternalSourceFence(profile, ownership_error, true))
			{
				SetError(error, ownership_error);
				return false;
			}
			context.root = profile / "Computer Use";
			std::error_code fs_error;
			std::filesystem::create_directories(context.root, fs_error);
			if (fs_error) { SetError(error, "Could not create Computer Use storage: " + fs_error.message() + "."); return false; }
			return true;
		}

		/// Copies, signs and swaps in the helper. The caller holds the install lock.
		std::filesystem::path InstallLocked(const std::filesystem::path& embedded_app, const InstallContext& context, std::string* error)
		{
			const std::filesystem::path& root = context.root;
			const std::filesystem::path installed = root / "UAM Computer Use.app";
			const std::filesystem::path receipt = root / ".receipt";
			const std::filesystem::path staging = InstallStagingPath(root, getpid());
			const std::filesystem::path previous = root / ".previous";
			NSFileManager* manager = [NSFileManager defaultManager];
			[manager removeItemAtPath:[NSString stringWithUTF8String:previous.c_str()] error:nil];
			// Every return below removes the copy; after a successful swap the staging path is already gone.
			InstallStagingGuard staging_guard(staging);
			if (!CopyItem(manager, embedded_app, staging, error)) return {};

			const std::filesystem::path embedded_framework_link = embedded_app / "Contents" / "Frameworks" / "Chromium Embedded Framework.framework";
			std::error_code link_error;
			std::filesystem::path framework_source;
			if (std::filesystem::is_symlink(embedded_framework_link, link_error))
			{
				const std::filesystem::path link_target = std::filesystem::read_symlink(embedded_framework_link, link_error);
				if (link_error) { SetError(error, "Embedded Computer Use helper has an unreadable CEF framework link."); return {}; }
				framework_source = (embedded_framework_link.parent_path() / link_target).lexically_normal();
			}
			else
			{
				framework_source = embedded_framework_link;
			}
			if (!std::filesystem::is_directory(framework_source, link_error) || link_error)
			{
				SetError(error, "Computer Use helper CEF framework is missing.");
				return {};
			}
			const std::filesystem::path framework_destination = staging / "Contents" / "Frameworks" / "Chromium Embedded Framework.framework";
			[manager removeItemAtPath:[NSString stringWithUTF8String:framework_destination.c_str()] error:nil];
			if (!CopyItem(manager, framework_source, framework_destination, error)) return {};

			if (!RunTool({"/usr/bin/codesign", "--force", "--sign", "-", "--identifier", kIdentifier, staging.string()}, error) ||
			    !RunTool({"/usr/bin/codesign", "--verify", "--deep", "--strict", staging.string()}, error)) return {};

			std::error_code fs_error;
			const bool installed_exists = std::filesystem::exists(installed, fs_error);
			if (fs_error || (installed_exists && std::rename(installed.c_str(), previous.c_str()) != 0))
			{
				SetError(error, "Could not preserve the current Computer Use helper: " + (fs_error ? fs_error.message() : std::string(std::strerror(errno))) + ".");
				return {};
			}
			if (std::rename(staging.c_str(), installed.c_str()) != 0)
			{
				const std::string reason = std::strerror(errno);
				if (std::filesystem::exists(previous)) std::rename(previous.c_str(), installed.c_str());
				SetError(error, "Could not activate the Computer Use helper: " + reason + ".");
				return {};
			}
			const std::string receipt_temp = receipt.string() + ".tmp";
			std::ofstream receipt_output(receipt_temp, std::ios::trunc);
			if (receipt_output) receipt_output << context.binary_hash << '\n' << context.plist_hash << '\n';
			receipt_output.close();
			if (!receipt_output || std::rename(receipt_temp.c_str(), receipt.c_str()) != 0)
			{
				SetError(error, "Could not write the Computer Use install receipt.");
				// Move the unrecorded copy back to staging (the guard removes it) and restore the previous helper.
				if (std::filesystem::exists(installed)) std::rename(installed.c_str(), staging.c_str());
				if (std::filesystem::exists(previous)) std::rename(previous.c_str(), installed.c_str());
				return {};
			}
			[manager removeItemAtPath:[NSString stringWithUTF8String:previous.c_str()] error:nil];
			return installed;
		}

		/// Takes the lock, clears copies left by dead installers and sets `installed` when the receipt already matches.
		bool InstalledIfCurrent(const InstallContext& context, std::filesystem::path& installed, std::string* error)
		{
			Lock lock(context.root / ".install.lock");
			if (!lock.acquired()) { SetError(error, "Timed out waiting for the Computer Use install lock."); return false; }
			SweepStaleInstallStaging(context.root, getpid());
			installed = InstallReceiptMatches(context.root, context.binary_hash, context.plist_hash) ? context.root / "UAM Computer Use.app" : std::filesystem::path{};
			return true;
		}

		/// Installs in a separate session so a provider CLI that kills its MCP server on a startup timeout
		/// cannot interrupt the copy; the next start then finds the finished install instead of copying again.
		bool RunDetachedInstall(const std::filesystem::path& embedded_app, std::string* error)
		{
			int output[2] = {-1, -1};
			if (pipe(output) != 0) { SetError(error, "Could not start the Computer Use installer."); return false; }
			posix_spawnattr_t attributes;
			posix_spawn_file_actions_t actions;
			posix_spawnattr_init(&attributes);
			posix_spawn_file_actions_init(&actions);
			posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT);
			posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
			posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
			posix_spawn_file_actions_adddup2(&actions, output[1], STDERR_FILENO);
			std::string binary = (embedded_app / "Contents" / "MacOS" / "UAM Computer Use").string();
			std::string mcp_flag = kMcpServerFlag, install_flag = kInstallFlag;
			char* argv[] = {binary.data(), mcp_flag.data(), install_flag.data(), nullptr};
			pid_t pid = 0;
			const int spawn_error = posix_spawn(&pid, argv[0], &actions, &attributes, argv, environ);
			posix_spawn_file_actions_destroy(&actions);
			posix_spawnattr_destroy(&attributes);
			close(output[1]);
			if (spawn_error != 0)
			{
				close(output[0]);
				SetError(error, "Could not start the Computer Use installer: " + std::string(std::strerror(spawn_error)) + ".");
				return false;
			}
			std::string diagnostics;
			std::array<char, 4096> buffer{};
			for (ssize_t count; (count = read(output[0], buffer.data(), buffer.size())) != 0;)
			{
				if (count < 0) { if (errno == EINTR) continue; break; }
				if (diagnostics.size() < 4096) diagnostics.append(buffer.data(), static_cast<std::size_t>(count));
			}
			close(output[0]);
			int status = 0;
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
			if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
			while (!diagnostics.empty() && (diagnostics.back() == '\n' || diagnostics.back() == '\r')) diagnostics.pop_back();
			SetError(error, diagnostics.empty() ? "The Computer Use installer failed." : diagnostics);
			return false;
		}
	} // namespace

	std::filesystem::path PrepareStandaloneComputerUse(const std::filesystem::path& embedded_app, std::string* error)
	{
		if (error != nullptr) error->clear();
		InstallContext context;
		std::filesystem::path installed;
		if (!OpenInstallContext(embedded_app, context, error) || !InstalledIfCurrent(context, installed, error)) return {};
		if (!installed.empty()) return installed;
		// Only a changed embedded helper reaches this point; an unchanged one never copies again.
		if (!RunDetachedInstall(embedded_app, error) || !InstalledIfCurrent(context, installed, error)) return {};
		if (installed.empty()) SetError(error, "The Computer Use installer finished without a matching install receipt.");
		return installed;
	}

	bool IsComputerUseInstallInvocation(const std::vector<std::string>& arguments)
	{
		return std::find(arguments.begin(), arguments.end(), kInstallFlag) != arguments.end();
	}

	int RunComputerUseInstall()
	{
		// The parent MCP process may exit first; a closed diagnostics pipe must not end the install.
		signal(SIGPIPE, SIG_IGN);
		// The installer only ever copies the bundle it runs from, never a path from its arguments.
		const std::filesystem::path embedded_app = std::filesystem::path(McpExecutablePath()).parent_path().parent_path().parent_path();
		std::string error;
		InstallContext context;
		if (!OpenInstallContext(embedded_app, context, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
		Lock lock(context.root / ".install.lock");
		if (!lock.acquired()) { std::fprintf(stderr, "Timed out waiting for the Computer Use install lock.\n"); return 1; }
		const std::size_t remaining = SweepStaleInstallStaging(context.root, getpid());
		if (InstallReceiptMatches(context.root, context.binary_hash, context.plist_hash)) return 0;
		if (remaining >= kMaxInstallStagingCopies)
		{
			std::fprintf(stderr, "Computer Use install refused: %zu unfinished helper copies are still owned by running processes.\n", remaining);
			return 1;
		}
		if (InstallLocked(embedded_app, context, &error).empty()) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
		return 0;
	}
} // namespace uam::computer_use

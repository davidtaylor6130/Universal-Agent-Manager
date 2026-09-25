#include "computer_use/computer_use_install_macos.h"

#include "common/paths/app_paths.h"

#import <Foundation/Foundation.h>

#include <CommonCrypto/CommonDigest.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <array>
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
	} // namespace

	std::filesystem::path PrepareStandaloneComputerUse(const std::filesystem::path& embedded_app, std::string* error)
	{
		if (error != nullptr) error->clear();
		const std::filesystem::path source_binary = embedded_app / "Contents" / "MacOS" / "UAM Computer Use";
		const std::filesystem::path source_plist = embedded_app / "Contents" / "Info.plist";
		if (!std::filesystem::is_directory(embedded_app) || !std::filesystem::is_regular_file(source_binary) || !std::filesystem::is_regular_file(source_plist))
		{
			SetError(error, "Embedded Computer Use helper bundle is missing or invalid.");
			return {};
		}
		const std::string binary_hash = Sha256(source_binary, error);
		if (binary_hash.empty()) return {};
		const std::string plist_hash = Sha256(source_plist, error);
		if (plist_hash.empty()) return {};

		const std::filesystem::path root = AppPaths::DefaultDataRootPath() / "Computer Use";
		std::error_code fs_error;
		std::filesystem::create_directories(root, fs_error);
		if (fs_error) { SetError(error, "Could not create Computer Use storage: " + fs_error.message() + "."); return {}; }
		Lock lock(root / ".install.lock");
		if (!lock.acquired()) { SetError(error, "Timed out waiting for the Computer Use install lock."); return {}; }

		const std::filesystem::path installed = root / "UAM Computer Use.app";
		const std::filesystem::path receipt = root / ".receipt";
		std::ifstream receipt_input(receipt);
		std::string receipt_binary, receipt_plist;
		if (receipt_input) receipt_input >> receipt_binary >> receipt_plist;
		if (receipt_binary == binary_hash && receipt_plist == plist_hash && std::filesystem::is_directory(installed)) return installed;

		const std::filesystem::path staging = root / (".staging-" + std::to_string(getpid()));
		const std::filesystem::path previous = root / ".previous";
		NSFileManager* manager = [NSFileManager defaultManager];
		[manager removeItemAtPath:[NSString stringWithUTF8String:staging.c_str()] error:nil];
		[manager removeItemAtPath:[NSString stringWithUTF8String:previous.c_str()] error:nil];
		if (!CopyItem(manager, embedded_app, staging, error)) return {};

		const std::filesystem::path embedded_framework_link = embedded_app / "Contents" / "Frameworks" / "Chromium Embedded Framework.framework";
		std::error_code link_error;
		std::filesystem::path framework_source;
		if (std::filesystem::is_symlink(embedded_framework_link, link_error))
		{
			const std::filesystem::path link_target = std::filesystem::read_symlink(embedded_framework_link, link_error);
			if (link_error) { SetError(error, "Embedded Computer Use helper has an unreadable CEF framework link."); [manager removeItemAtPath:[NSString stringWithUTF8String:staging.c_str()] error:nil]; return {}; }
			framework_source = (embedded_framework_link.parent_path() / link_target).lexically_normal();
		}
		else
		{
			framework_source = embedded_framework_link;
		}
		if (!std::filesystem::is_directory(framework_source, link_error) || link_error)
		{
			SetError(error, "Computer Use helper CEF framework is missing.");
			[manager removeItemAtPath:[NSString stringWithUTF8String:staging.c_str()] error:nil];
			return {};
		}
		const std::filesystem::path framework_destination = staging / "Contents" / "Frameworks" / "Chromium Embedded Framework.framework";
		[manager removeItemAtPath:[NSString stringWithUTF8String:framework_destination.c_str()] error:nil];
		if (!CopyItem(manager, framework_source, framework_destination, error)) { [manager removeItemAtPath:[NSString stringWithUTF8String:staging.c_str()] error:nil]; return {}; }

		if (!RunTool({"/usr/bin/codesign", "--force", "--sign", "-", "--identifier", kIdentifier, staging.string()}, error) ||
			!RunTool({"/usr/bin/codesign", "--verify", "--deep", "--strict", staging.string()}, error)) { [manager removeItemAtPath:[NSString stringWithUTF8String:staging.c_str()] error:nil]; return {}; }

		fs_error.clear();
		const bool installed_exists = std::filesystem::exists(installed, fs_error);
		if (fs_error || (installed_exists && std::rename(installed.c_str(), previous.c_str()) != 0))
		{
			SetError(error, "Could not preserve the current Computer Use helper: " + (fs_error ? fs_error.message() : std::string(std::strerror(errno))) + ".");
			[manager removeItemAtPath:[NSString stringWithUTF8String:staging.c_str()] error:nil];
			return {};
		}
		if (std::rename(staging.c_str(), installed.c_str()) != 0)
		{
			const std::string reason = std::strerror(errno);
			if (std::filesystem::exists(previous)) std::rename(previous.c_str(), installed.c_str());
			SetError(error, "Could not activate the Computer Use helper: " + reason + ".");
			return {};
		}
		{
			std::ofstream receipt_output(receipt.string() + ".tmp", std::ios::trunc);
			if (!receipt_output) { SetError(error, "Could not write the Computer Use install receipt."); goto rollback; }
			receipt_output << binary_hash << '\n' << plist_hash << '\n';
			receipt_output.close();
			if (!receipt_output) { SetError(error, "Could not write the Computer Use install receipt."); goto rollback; }
			if (std::rename((receipt.string() + ".tmp").c_str(), receipt.c_str()) != 0)
			{
				SetError(error, "Could not activate the Computer Use install receipt: " + std::string(std::strerror(errno)) + ".");
				goto rollback;
			}
		}
		[manager removeItemAtPath:[NSString stringWithUTF8String:previous.c_str()] error:nil];
		return installed;

	rollback:
		[manager removeItemAtPath:[NSString stringWithUTF8String:staging.c_str()] error:nil];
		if (std::filesystem::exists(installed)) std::rename(installed.c_str(), staging.c_str());
		if (std::filesystem::exists(previous)) std::rename(previous.c_str(), installed.c_str());
		[manager removeItemAtPath:[NSString stringWithUTF8String:staging.c_str()] error:nil];
		return {};
	}
} // namespace uam::computer_use

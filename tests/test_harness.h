#pragma once

#include "app/chat_domain_service.h"
#include "app/chat_lifecycle_service.h"
#include "app/git_worktree_service.h"
#include "app/goal_service.h"
#include "app/markdown_store_service.h"
#include "app/memory_library_service.h"
#include "app/memory_service.h"
#include "app/native_session_link_service.h"
#include "app/provider_model_catalog_service.h"
#include "app/provider_worker_command.h"
#include "app/provider_profile_migration_service.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"
#include "app/shell_action_service.h"
#include "app/vcs_commit_service.h"
#include "common/chat/chat_branching.h"
#include "common/chat/chat_folder_store.h"
#include "common/chat/chat_ids.h"
#include "common/chat/native_chat_identity.h"
#include "common/chat/chat_repository.h"
#include "common/config/approval_modes.h"
#include "common/config/editor_file_associations.h"
#include "common/config/frontend_actions.h"
#include "common/config/line_value_codec.h"
#include "common/config/settings_normalization.h"
#include "common/config/settings_store.h"
#include "common/constants/app_constants.h"
#include "common/memory/memory_categories.h"
#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"
#include "common/provider/codex/codex_options.h"
#include "common/provider/codex/cli/codex_session_index.h"
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
#include "common/provider/gemini/base/gemini_history_loader.h"
#endif
#include "common/provider/provider_ids.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_profile_constants.h"
#include "common/provider/provider_runtime.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/provider/opencode/cli/opencode_cli_provider_runtime.h"
#include "common/runtime/acp/acp_attention_kind.h"
#include "common/runtime/acp/acp_claude_stream.h"
#include "common/runtime/acp/acp_content.h"
#include "common/runtime/acp/acp_json_rpc.h"
#include "common/runtime/acp/acp_model_json.h"
#include "common/runtime/acp/acp_permissions.h"
#include "common/runtime/acp/acp_polling.h"
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/runtime/acp/acp_request_defaults.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_statuses.h"
#include "common/runtime/acp/acp_stream_types.h"
#include "common/runtime/acp/acp_tool_items.h"
#include "common/runtime/acp/acp_tool_kinds.h"
#include "common/security/command_safety.h"
#include "common/runtime/app_time.h"
#include "common/runtime/json_runtime.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_debug_diagnostics.h"
#include "common/runtime/terminal/terminal_dimensions.h"
#include "common/runtime/terminal/terminal_idle_classifier.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/runtime/terminal/terminal_provider_cli.h"
#include "common/runtime/terminal_polling.h"
#include "common/utils/base64.h"
#include "common/utils/io_utils.h"
#include "common/utils/command_line_words.h"
#include "common/utils/env_utils.h"
#include "common/utils/hash_utils.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/parse_utils.h"
#include "common/utils/range_utils.h"
#include "common/utils/sensitive_text.h"
#include "common/utils/shell_escape.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"
#include "cef/uam_bridge_request.h"
#include "cef/uam_cef_command_line_config.h"
#include "cef/uam_cef_security.h"
#include "cef/cef_push.h"
#include "cef/state_serializer.h"
#include "core/chat_import_utils.h"
#include "core/gemini_cli_compat.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <cerrno>
#include <csignal>
#endif

namespace fs = std::filesystem;

namespace uam_test
{
	struct TestCase
	{
		std::string name;
		std::function<void()> fn;
	};

	inline std::vector<TestCase>& Registry()
	{
		static std::vector<TestCase> tests;
		return tests;
	}

	struct RegisterTest
	{
		RegisterTest(std::string name, std::function<void()> fn)
		{
			Registry().push_back({std::move(name), std::move(fn)});
		}
	};

	struct TestFailure : std::runtime_error
	{
		using std::runtime_error::runtime_error;
	};

	inline void Fail(const char* expr, const char* file, int line)
	{
		std::ostringstream out;
		out << file << ':' << line << ": assertion failed: " << expr;
		throw TestFailure(out.str());
	}

	template <typename T, typename U> inline void AssertEq(const T& actual, const U& expected, const char* actual_expr, const char* expected_expr, const char* file, int line)
	{
		if (!(actual == expected))
		{
			std::ostringstream out;
			out << file << ':' << line << ": expected " << actual_expr << " == " << expected_expr;
			throw TestFailure(out.str());
		}
	}

	struct TempDir
	{
		fs::path root;

		explicit TempDir(const std::string& prefix)
		{
			root = fs::temp_directory_path() / (prefix + "-" + uam::time::SteadyEpochNanosecondsTokenNow());
			fs::create_directories(root);
		}

		~TempDir()
		{
			std::error_code ec;
			fs::remove_all(root, ec);
		}
	};

	inline std::string ReadFile(const fs::path& path)
	{
		return uam::io::ReadTextFile(path);
	}

	inline std::size_t CountSubstring(const std::string& text, const std::string& needle)
	{
		if (needle.empty())
		{
			return 0;
		}

		std::size_t count = 0;
		std::size_t pos = 0;
		while ((pos = text.find(needle, pos)) != std::string::npos)
		{
			++count;
			pos += needle.size();
		}
		return count;
	}

	inline bool IsLowerHexString(const std::string& value)
	{
		return std::ranges::all_of(value, [](char ch) { return uam::strings::IsAsciiHexDigit(static_cast<unsigned char>(ch)) && (ch < 'A' || ch > 'F'); });
	}

	inline bool IsAsciiDigitString(const std::string& value)
	{
		return uam::strings::AllAsciiDigits(value);
	}

	inline std::string LastDashSegment(const std::string& value)
	{
		const std::size_t index = value.rfind('-');
		return index == std::string::npos ? "" : value.substr(index + 1);
	}

	inline std::string ShellQuoteForTest(const std::string& value)
	{
#if defined(_WIN32)
		std::string escaped = "\"";
		for (const char ch : value)
		{
			if (ch == '"')
			{
				escaped += "\"\"";
			}
			else if (ch == '%')
			{
				escaped += "%%";
			}
			else
			{
				escaped.push_back(ch == '\n' || ch == '\r' ? ' ' : ch);
			}
		}
		escaped.push_back('"');
		return escaped;
#else
		std::string escaped = "'";
		for (const char ch : value)
		{
			if (ch == '\'')
			{
				escaped += "'\\''";
			}
			else
			{
				escaped.push_back(ch);
			}
		}
		escaped.push_back('\'');
		return escaped;
#endif
	}

	inline bool RunTestCommand(const std::string& command)
	{
		const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(command, 120000);
		return result.ok && !result.timed_out && !result.canceled && result.exit_code == 0;
	}

	inline bool GitAvailableForTests()
	{
		return RunTestCommand("git --version");
	}

	inline bool RunGitForTest(const fs::path& repo, const std::string& args)
	{
		return RunTestCommand("git -C " + ShellQuoteForTest(repo.string()) + " " + args);
	}

	struct ScopedEnvVar
	{
		std::string name;
		std::string previous;
		bool had_previous = false;

		ScopedEnvVar(std::string key, const std::string& value) : name(std::move(key))
		{
			if (const char* existing = std::getenv(name.c_str()); existing != nullptr)
			{
				had_previous = true;
				previous = existing;
			}

#if defined(_WIN32)
			_putenv_s(name.c_str(), value.c_str());
#else
			setenv(name.c_str(), value.c_str(), 1);
#endif
		}

		~ScopedEnvVar()
		{
#if defined(_WIN32)
			_putenv_s(name.c_str(), had_previous ? previous.c_str() : "");
#else
			if (had_previous)
			{
				setenv(name.c_str(), previous.c_str(), 1);
			}
			else
			{
				unsetenv(name.c_str());
			}
#endif
		}
	};

#if !defined(_WIN32)
	struct ScopedDirectoryNoWrite
	{
		fs::path path;

		explicit ScopedDirectoryNoWrite(fs::path target) : path(std::move(target))
		{
			std::error_code ec;
			fs::permissions(path, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace, ec);
		}

		~ScopedDirectoryNoWrite()
		{
			std::error_code ec;
			fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace, ec);
		}
	};
#endif
} // namespace uam_test

#define UAM_TEST(name)                                                    \
	static void name();                                                   \
	static uam_test::RegisterTest register_##name(#name, name);          \
	static void name()

#define UAM_ASSERT(expr)                                                  \
	do                                                                    \
	{                                                                     \
		if (!(expr))                                                      \
			uam_test::Fail(#expr, __FILE__, __LINE__);                    \
	} while (false)

#define UAM_ASSERT_EQ(actual, expected)                                   \
	do                                                                    \
	{                                                                     \
		uam_test::AssertEq((actual), (expected), #actual, #expected, __FILE__, __LINE__); \
	} while (false)

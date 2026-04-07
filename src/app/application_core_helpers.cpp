#include "application_core_helpers.h"

#include <imgui.h>

#include "common/paths/app_paths.h"
#include "common/rag/rag_app_helpers.h"
#include "common/platform/platform_services.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;
using uam::AppState;

namespace
{

	const ChatFolder* FindFolderByIdForRag(const AppState& app, const std::string& folder_id)
	{
		for (const ChatFolder& folder : app.folders)
		{
			if (folder.id == folder_id)
			{
				return &folder;
			}
		}

		return nullptr;
	}

	const ChatSession* SelectedChatForRag(const AppState& app)
	{
		if (app.selected_chat_index < 0 || app.selected_chat_index >= static_cast<int>(app.chats.size()))
		{
			return nullptr;
		}

		return &app.chats[app.selected_chat_index];
	}

} // namespace

std::string Trim(const std::string& p_value)
{
	const auto l_start = p_value.find_first_not_of(" \t\r\n");

	if (l_start == std::string::npos)
	{
		return "";
	}

	const auto l_end = p_value.find_last_not_of(" \t\r\n");
	return p_value.substr(l_start, l_end - l_start + 1);
}

std::string NormalizeVectorDatabaseName(std::string p_value)
{
	p_value = Trim(p_value);
	auto l_shouldStripCharacter = [](const char p_ch)
	{
		const unsigned char l_c = static_cast<unsigned char>(p_ch);
		return !(std::isalnum(l_c) != 0 || p_ch == '_' || p_ch == '-' || p_ch == '.');
	};

	p_value.erase(std::remove_if(p_value.begin(), p_value.end(), l_shouldStripCharacter), p_value.end());
	return p_value;
}

std::string TimestampNow()
{
	const auto l_now = std::chrono::system_clock::now();
	const std::time_t l_tt = std::chrono::system_clock::to_time_t(l_now);
	std::tm l_tmSnapshot{};

	if (!PlatformServicesFactory::Instance().process_service.PopulateLocalTime(l_tt, &l_tmSnapshot))
	{
		return "";
	}

	std::ostringstream l_out;
	l_out << std::put_time(&l_tmSnapshot, "%Y-%m-%d %H:%M:%S");
	return l_out.str();
}

std::string NewSessionId()
{
	const auto l_now = std::chrono::system_clock::now().time_since_epoch();
	const auto l_epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(l_now).count();
	std::mt19937 l_rng(std::random_device{}());
	std::uniform_int_distribution<int> l_hexDigit(0, 15);
	std::ostringstream l_id;
	l_id << "chat-" << l_epochMs << "-";

	for (int l_i = 0; l_i < 6; ++l_i)
	{
		l_id << std::hex << l_hexDigit(l_rng);
	}

	return l_id.str();
}

std::string ReadTextFile(const fs::path& p_path)
{
	std::ifstream l_in(p_path, std::ios::binary);

	if (!l_in.good())
	{
		return "";
	}

	std::ostringstream l_buffer;
	l_buffer << l_in.rdbuf();
	return l_buffer.str();
}

bool WriteTextFile(const fs::path& p_path, const std::string& p_content)
{
	std::ofstream l_out(p_path, std::ios::binary | std::ios::trunc);

	if (!l_out.good())
	{
		return false;
	}

	l_out << p_content;
	return l_out.good();
}

fs::path ResolvePromptProfileRootPath(const AppSettings& p_settings)
{
	fs::path l_candidate = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(p_settings.prompt_profile_root_path);

	if (l_candidate.empty())
	{
		l_candidate = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(p_settings.gemini_global_root_path);
	}

	if (!l_candidate.empty())
	{
		return l_candidate;
	}

	return AppPaths::DefaultGeminiUniversalRootPath();
}

fs::path ResolveWorkspaceRootPath(const AppState& p_app, const ChatSession& p_chat)
{
	fs::path l_workspaceRoot;

	if (const ChatFolder* lcp_folder = FindFolderByIdForRag(p_app, p_chat.folder_id); lcp_folder != nullptr)
	{
		l_workspaceRoot = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(lcp_folder->directory);
	}

	if (l_workspaceRoot.empty())
	{
		l_workspaceRoot = fs::current_path();
	}

	std::error_code l_ec;
	const fs::path l_absoluteRoot = fs::absolute(l_workspaceRoot, l_ec);
	return l_ec ? l_workspaceRoot : l_absoluteRoot;
}

fs::path WorkspacePromptProfileRootPath(const AppState& p_app, const ChatSession& p_chat)
{
	return ResolveWorkspaceRootPath(p_app, p_chat) / ".gemini";
}

fs::path WorkspacePromptProfileTemplatePath(const AppState& p_app, const ChatSession& p_chat)
{
	return WorkspacePromptProfileRootPath(p_app, p_chat) / "gemini.md";
}

fs::path ResolveCurrentRagFallbackSourceRoot(const AppState& p_app)
{
	if (const ChatSession* lcp_selected = SelectedChatForRag(p_app); lcp_selected != nullptr)
	{
		return ResolveWorkspaceRootPath(p_app, *lcp_selected);
	}

	std::error_code l_cwdEc;
	const fs::path l_cwd = fs::current_path(l_cwdEc);
	return l_cwdEc ? fs::path{} : l_cwd;
}

std::uint64_t Fnv1a64(const std::string& p_text)
{
	std::uint64_t l_hash = 1469598103934665603ULL;

	for (const unsigned char l_ch : p_text)
	{
		l_hash ^= static_cast<std::uint64_t>(l_ch);
		l_hash *= 1099511628211ULL;
	}

	return l_hash;
}

std::string Hex64(const std::uint64_t p_value)
{
	std::ostringstream l_out;
	l_out << std::hex << p_value;
	return l_out.str();
}

std::string ToLowerAscii(std::string p_value)
{
	std::transform(p_value.begin(), p_value.end(), p_value.begin(), [](const unsigned char p_ch) { return static_cast<char>(std::tolower(p_ch)); });
	return p_value;
}

bool IsLikelyBinaryBlob(const std::string& p_content)
{
	return p_content.find('\0') != std::string::npos;
}

bool IsRagIndexableTextFile(const fs::path& p_path)
{
	static const std::unordered_set<std::string> l_kAllowedExtensions = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".ixx", ".ipp", ".m", ".mm", ".java", ".kt", ".kts", ".go", ".rs", ".swift", ".cs", ".py", ".js", ".ts", ".tsx", ".jsx", ".php", ".rb", ".lua", ".sh", ".zsh", ".bash", ".ps1", ".sql", ".json", ".yaml", ".yml", ".toml", ".ini", ".cfg", ".conf", ".xml", ".html", ".css", ".scss", ".md", ".markdown", ".txt", ".rst", ".adoc", ".cmake", ".mk", ".make"};
	const std::string l_extension = ToLowerAscii(p_path.extension().string());
	return l_kAllowedExtensions.find(l_extension) != l_kAllowedExtensions.end();
}

std::string TruncateToApproxTokenCount(const std::string& p_content, const std::size_t p_maxTokens)
{
	if (p_maxTokens == 0 || p_content.empty())
	{
		return p_content;
	}

	std::size_t l_tokenCount = 0;
	bool l_inToken = false;
	std::size_t l_tokenStart = 0;

	for (std::size_t l_i = 0; l_i < p_content.size(); ++l_i)
	{
		const unsigned char l_ch = static_cast<unsigned char>(p_content[l_i]);
		const bool l_whitespace = (std::isspace(l_ch) != 0);

		if (!l_whitespace && !l_inToken)
		{
			l_tokenStart = l_i;
			++l_tokenCount;

			if (l_tokenCount > p_maxTokens)
			{
				std::size_t l_end = l_tokenStart;

				while (l_end > 0 && std::isspace(static_cast<unsigned char>(p_content[l_end - 1])) != 0)
				{
					--l_end;
				}

				return p_content.substr(0, l_end);
			}

			l_inToken = true;
		}
		else if (l_whitespace)
		{
			l_inToken = false;
		}
	}

	return p_content;
}

std::filesystem::path BuildRagTokenCappedStagingRoot(const AppState& p_app, const std::string& p_workspaceKey)
{
	return p_app.data_root / "rag_scan_staging" / ("ws_" + Hex64(Fnv1a64(p_workspaceKey)));
}

bool BuildRagTokenCappedStagingTree(const std::filesystem::path& p_sourceRoot, const std::filesystem::path& p_stagingRoot, const int p_maxTokens, std::size_t* p_indexedFilesOut, std::string* p_errorOut)
{
	if (p_indexedFilesOut != nullptr)
	{
		*p_indexedFilesOut = 0;
	}

	if (p_maxTokens <= 0)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Token cap must be greater than zero.";
		}

		return false;
	}

	std::error_code l_ec;
	fs::remove_all(p_stagingRoot, l_ec);
	l_ec.clear();
	fs::create_directories(p_stagingRoot, l_ec);

	if (l_ec)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Failed to prepare token-capped staging directory: " + p_stagingRoot.string();
		}

		return false;
	}

	std::size_t l_copiedFiles = 0;
	fs::recursive_directory_iterator l_it(p_sourceRoot, fs::directory_options::skip_permission_denied, l_ec);
	const fs::recursive_directory_iterator l_end;

	while (!l_ec && l_it != l_end)
	{
		const fs::directory_entry l_entry = *l_it;
		++l_it;

		if (l_ec)
		{
			l_ec.clear();
			continue;
		}

		if (!l_entry.is_regular_file(l_ec))
		{
			l_ec.clear();
			continue;
		}

		if (!IsRagIndexableTextFile(l_entry.path()))
		{
			continue;
		}

		const fs::path l_absolute = l_entry.path();
		const fs::path l_relative = fs::relative(l_absolute, p_sourceRoot, l_ec);

		if (l_ec || l_relative.empty())
		{
			l_ec.clear();
			continue;
		}

		const fs::path l_destination = p_stagingRoot / l_relative;
		fs::create_directories(l_destination.parent_path(), l_ec);

		if (l_ec)
		{
			l_ec.clear();
			continue;
		}

		std::ifstream l_in(l_absolute, std::ios::binary);

		if (!l_in.good())
		{
			continue;
		}

		std::ostringstream l_buffer;
		l_buffer << l_in.rdbuf();
		std::string l_content = l_buffer.str();

		if (IsLikelyBinaryBlob(l_content))
		{
			continue;
		}

		l_content = TruncateToApproxTokenCount(l_content, static_cast<std::size_t>(p_maxTokens));

		if (!WriteTextFile(l_destination, l_content))
		{
			continue;
		}

		++l_copiedFiles;
	}

	if (p_indexedFilesOut != nullptr)
	{
		*p_indexedFilesOut = l_copiedFiles;
	}

	return true;
}

fs::path ResolveProjectRagSourceRoot(const AppState& p_app, const fs::path& p_fallbackSourceRoot)
{
	fs::path l_sourceRoot = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(p_app.settings.rag_project_source_directory);

	if (l_sourceRoot.empty())
	{
		l_sourceRoot = p_fallbackSourceRoot;
	}

	if (l_sourceRoot.empty())
	{
		std::error_code l_cwdEc;
		l_sourceRoot = fs::current_path(l_cwdEc);
	}

	std::error_code l_ec;
	const fs::path l_absoluteRoot = fs::absolute(l_sourceRoot, l_ec);
	return l_ec ? l_sourceRoot.lexically_normal() : l_absoluteRoot.lexically_normal();
}

fs::path NormalizeAbsolutePath(const fs::path& p_path)
{
	if (p_path.empty())
	{
		return {};
	}

	std::error_code l_ec;
	const fs::path l_absolute = fs::absolute(p_path, l_ec);
	return l_ec ? p_path.lexically_normal() : l_absolute.lexically_normal();
}

#include "common/rag/rag_app_helpers.inl"

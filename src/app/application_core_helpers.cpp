#include "application_core_helpers.h"

#include <imgui.h>

#include "common/app_paths.h"
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

std::vector<fs::path> ResolveRagSourceRootsForChat(const AppState& p_app, const ChatSession& p_chat, const fs::path& p_fallbackSourceRoot)
{
	std::vector<fs::path> l_roots;
	std::unordered_set<std::string> l_seen;
	l_roots.reserve(p_chat.rag_source_directories.size() + 1);

	for (const std::string& l_rawSource : p_chat.rag_source_directories)
	{
		fs::path l_sourceRoot = NormalizeAbsolutePath(PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(l_rawSource));

		if (l_sourceRoot.empty())
		{
			continue;
		}

		const std::string l_sourceKey = l_sourceRoot.generic_string();

		if (l_sourceKey.empty())
		{
			continue;
		}

		if (l_seen.insert(l_sourceKey).second)
		{
			l_roots.push_back(l_sourceRoot);
		}
	}

	if (l_roots.empty())
	{
		l_roots.push_back(ResolveProjectRagSourceRoot(p_app, p_fallbackSourceRoot));
	}

	return l_roots;
}

std::vector<fs::path> DiscoverRagSourceFolders(const fs::path& p_workspaceRoot)
{
	std::vector<fs::path> l_folders;
	std::error_code l_ec;
	const fs::path l_normalizedWorkspace = NormalizeAbsolutePath(p_workspaceRoot);

	if (l_normalizedWorkspace.empty() || !fs::exists(l_normalizedWorkspace, l_ec) || !fs::is_directory(l_normalizedWorkspace, l_ec))
	{
		return l_folders;
	}

	l_folders.push_back(l_normalizedWorkspace);

	static const std::unordered_set<std::string> l_kExcluded = {".git", ".svn", ".hg", "node_modules", "dist", "build", "Builds", "out", "target", "__pycache__", ".venv", "venv"};

	for (const auto& l_entry : fs::directory_iterator(l_normalizedWorkspace, fs::directory_options::skip_permission_denied, l_ec))
	{
		if (l_ec || !l_entry.is_directory(l_ec))
		{
			l_ec.clear();
			continue;
		}

		const std::string l_name = l_entry.path().filename().string();

		if (l_name.empty() || l_name[0] == '.' || l_kExcluded.find(l_name) != l_kExcluded.end())
		{
			continue;
		}

		l_folders.push_back(NormalizeAbsolutePath(l_entry.path()));
	}

	std::sort(l_folders.begin(), l_folders.end(), [](const fs::path& p_lhs, const fs::path& p_rhs) { return p_lhs.generic_string() < p_rhs.generic_string(); });
	l_folders.erase(std::remove_if(l_folders.begin(), l_folders.end(), [](const fs::path& p_path) { return p_path.empty(); }), l_folders.end());
	l_folders.erase(std::unique(l_folders.begin(), l_folders.end(), [](const fs::path& p_lhs, const fs::path& p_rhs) { return p_lhs.generic_string() == p_rhs.generic_string(); }), l_folders.end());
	return l_folders;
}

std::string RagDatabaseNameForSourceRoot(const AppSettings& p_settings, const fs::path& p_sourceRoot)
{
	const std::string l_overrideName = Trim(p_settings.vector_database_name_override);

	if (!l_overrideName.empty())
	{
		return l_overrideName;
	}

	const std::string l_sourceKey = p_sourceRoot.lexically_normal().generic_string();

	if (l_sourceKey.empty())
	{
		return "";
	}

	return "uam_" + Hex64(Fnv1a64(l_sourceKey));
}

bool ChatHasRagSourceDirectory(const ChatSession& p_chat, const fs::path& p_sourceRoot)
{
	const std::string l_candidateKey = NormalizeAbsolutePath(p_sourceRoot).generic_string();

	if (l_candidateKey.empty())
	{
		return false;
	}

	for (const std::string& l_existingSource : p_chat.rag_source_directories)
	{
		const fs::path l_existingPath = NormalizeAbsolutePath(PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(l_existingSource));

		if (!l_existingPath.empty() && l_existingPath.generic_string() == l_candidateKey)
		{
			return true;
		}
	}

	return false;
}

bool AddChatRagSourceDirectory(ChatSession& p_chat, const fs::path& p_sourceRoot)
{
	const fs::path l_normalizedRoot = NormalizeAbsolutePath(p_sourceRoot);
	const std::string l_candidateKey = l_normalizedRoot.generic_string();

	if (l_candidateKey.empty())
	{
		return false;
	}

	if (ChatHasRagSourceDirectory(p_chat, l_normalizedRoot))
	{
		return false;
	}

	p_chat.rag_source_directories.push_back(l_normalizedRoot.string());
	return true;
}

bool RemoveChatRagSourceDirectoryAt(ChatSession& p_chat, const std::size_t p_index)
{
	if (p_index >= p_chat.rag_source_directories.size())
	{
		return false;
	}

	p_chat.rag_source_directories.erase(p_chat.rag_source_directories.begin() + static_cast<std::ptrdiff_t>(p_index));
	return true;
}

bool RemoveChatRagSourceDirectory(ChatSession& p_chat, const fs::path& p_sourceRoot)
{
	const std::string l_removeKey = NormalizeAbsolutePath(p_sourceRoot).generic_string();

	if (l_removeKey.empty())
	{
		return false;
	}

	for (std::size_t l_i = 0; l_i < p_chat.rag_source_directories.size(); ++l_i)
	{
		const fs::path l_existing = NormalizeAbsolutePath(PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(p_chat.rag_source_directories[l_i]));

		if (!l_existing.empty() && l_existing.generic_string() == l_removeKey)
		{
			return RemoveChatRagSourceDirectoryAt(p_chat, l_i);
		}
	}

	return false;
}

bool DirectoryContainsGguf(const fs::path& p_directory)
{
	std::error_code l_ec;

	if (p_directory.empty() || !fs::exists(p_directory, l_ec) || !fs::is_directory(p_directory, l_ec))
	{
		return false;
	}

	fs::recursive_directory_iterator l_it(p_directory, fs::directory_options::skip_permission_denied, l_ec);
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

		std::string l_extension = l_entry.path().extension().string();
		std::transform(l_extension.begin(), l_extension.end(), l_extension.begin(), [](const unsigned char p_ch) { return static_cast<char>(std::tolower(p_ch)); });

		if (l_extension == ".gguf")
		{
			return true;
		}
	}

	return false;
}

fs::path ResolveRagModelFolder(const AppState& p_app, const AppSettings* p_settingsOverride)
{
	const AppSettings& l_settings = (p_settingsOverride != nullptr) ? *p_settingsOverride : p_app.settings;
	const fs::path l_configuredModelFolder = NormalizeAbsolutePath(PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(l_settings.models_folder_directory));

	if (!l_configuredModelFolder.empty())
	{
		std::error_code l_configuredEc;
		fs::create_directories(l_configuredModelFolder, l_configuredEc);
		return l_configuredModelFolder;
	}

	std::vector<fs::path> l_candidates;
	l_candidates.push_back(p_app.data_root / "models");

	if (const char* lcp_envModels = std::getenv("UAM_OLLAMA_ENGINE_MODELS_DIR"))
	{
		const std::string l_value = Trim(lcp_envModels);

		if (!l_value.empty())
		{
			l_candidates.push_back(PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(l_value));
		}
	}

	std::error_code l_cwdEc;
	const fs::path l_cwd = fs::current_path(l_cwdEc);

	if (!l_cwdEc)
	{
		l_candidates.push_back(l_cwd / "models");
		l_candidates.push_back(l_cwd / "Builds" / "models");
		l_candidates.push_back(l_cwd / "build" / "models");
	}

	for (const fs::path& l_candidate : l_candidates)
	{
		if (DirectoryContainsGguf(l_candidate))
		{
			return l_candidate;
		}
	}

	std::error_code l_ec;
	fs::create_directories(p_app.data_root / "models", l_ec);
	return p_app.data_root / "models";
}

RagIndexService::Config RagConfigFromSettings(const AppSettings& p_settings)
{
	RagIndexService::Config l_config;
	l_config.enabled = p_settings.rag_enabled;
#if UAM_ENABLE_ENGINE_RAG
	l_config.vector_backend = (p_settings.vector_db_backend == "none") ? "none" : "ollama-engine";
	l_config.vector_enabled = (l_config.vector_backend != "none");
#else
	l_config.vector_backend = "none";
	l_config.vector_enabled = false;
#endif
	l_config.top_k = std::clamp(p_settings.rag_top_k, 1, 20);
	l_config.max_snippet_chars = static_cast<std::size_t>(std::clamp(p_settings.rag_max_snippet_chars, 120, 4000));
	l_config.max_file_bytes = static_cast<std::size_t>(std::clamp(p_settings.rag_max_file_bytes, 16 * 1024, 20 * 1024 * 1024));
	l_config.vector_max_tokens = static_cast<std::size_t>(std::clamp(p_settings.rag_scan_max_tokens, 0, 32768));
	l_config.vector_model_id = Trim(p_settings.selected_vector_model_id);
	l_config.vector_database_name_override = Trim(p_settings.vector_database_name_override);
	return l_config;
}

void SyncRagServiceConfig(AppState& p_app)
{
	p_app.rag_index_service.SetConfig(RagConfigFromSettings(p_app.settings));
	const fs::path l_modelFolder = ResolveRagModelFolder(p_app);
	p_app.rag_index_service.SetModelFolder(l_modelFolder);
}

std::string BuildRagContextBlock(const std::vector<RagSnippet>& p_snippets)
{
	if (p_snippets.empty())
	{
		return "";
	}

	std::ostringstream l_out;
	l_out << "Retrieved context:\n";

	for (std::size_t l_i = 0; l_i < p_snippets.size(); ++l_i)
	{
		const RagSnippet& l_snippet = p_snippets[l_i];
		l_out << (l_i + 1) << ". ";

		if (!l_snippet.relative_path.empty())
		{
			l_out << l_snippet.relative_path;

			if (l_snippet.start_line > 0 && l_snippet.end_line >= l_snippet.start_line)
			{
				l_out << ":" << l_snippet.start_line << "-" << l_snippet.end_line;
			}
		}
		else
		{
			l_out << "Snippet";
		}

		l_out << "\n";
		l_out << l_snippet.text << "\n\n";
	}

	return l_out.str();
}

bool IsRagEnabledForChat(const AppState& p_app, const ChatSession& p_chat)
{
	return p_app.settings.rag_enabled && p_chat.rag_enabled;
}

std::string BuildRagEnhancedPrompt(AppState& p_app, const ChatSession& p_chat, const std::string& p_promptText)
{
	if (!IsRagEnabledForChat(p_app, p_chat))
	{
		return p_promptText;
	}

	const fs::path l_chatWorkspaceRoot = ResolveWorkspaceRootPath(p_app, p_chat);
	const std::vector<fs::path> l_sourceRoots = ResolveRagSourceRootsForChat(p_app, p_chat, l_chatWorkspaceRoot);
	const std::size_t l_topK = static_cast<std::size_t>(std::clamp(p_app.settings.rag_top_k, 1, 20));
	const bool l_multipleSources = l_sourceRoots.size() > 1;

	std::vector<std::vector<RagSnippet>> l_snippetsBySource;
	l_snippetsBySource.reserve(l_sourceRoots.size());

	for (const fs::path& l_sourceRoot : l_sourceRoots)
	{
		const std::string l_sourceKey = l_sourceRoot.lexically_normal().generic_string();
		std::error_code l_ec;

		if (l_sourceRoot.empty() || !fs::exists(l_sourceRoot, l_ec) || !fs::is_directory(l_sourceRoot, l_ec))
		{
			if (!l_sourceKey.empty())
			{
				p_app.rag_last_refresh_by_workspace[l_sourceKey] = "RAG source directory is invalid or missing.";
			}

			continue;
		}

		std::string l_ragError;
		std::vector<RagSnippet> l_snippets = p_app.rag_index_service.Retrieve(l_sourceRoot, p_promptText, l_topK, 1, &l_ragError);

		if (!l_ragError.empty() && !l_sourceKey.empty())
		{
			p_app.rag_last_refresh_by_workspace[l_sourceKey] = l_ragError;
		}

		if (l_snippets.empty())
		{
			continue;
		}

		if (l_multipleSources)
		{
			std::string l_sourceLabel = l_sourceRoot.filename().string();

			if (l_sourceLabel.empty())
			{
				l_sourceLabel = l_sourceRoot.string();
			}

			for (RagSnippet& l_snippet : l_snippets)
			{
				if (l_snippet.relative_path.empty())
				{
					l_snippet.relative_path = l_sourceLabel;
				}
				else
				{
					l_snippet.relative_path = l_sourceLabel + "/" + l_snippet.relative_path;
				}
			}
		}

		l_snippetsBySource.push_back(std::move(l_snippets));
	}

	std::vector<RagSnippet> l_mergedSnippets;
	l_mergedSnippets.reserve(l_topK);
	std::vector<std::size_t> l_sourceOffsets(l_snippetsBySource.size(), 0);

	while (l_mergedSnippets.size() < l_topK)
	{
		bool l_addedAny = false;

		for (std::size_t l_i = 0; l_i < l_snippetsBySource.size(); ++l_i)
		{
			if (l_sourceOffsets[l_i] >= l_snippetsBySource[l_i].size())
			{
				continue;
			}

			l_mergedSnippets.push_back(l_snippetsBySource[l_i][l_sourceOffsets[l_i]]);
			++l_sourceOffsets[l_i];
			l_addedAny = true;

			if (l_mergedSnippets.size() >= l_topK)
			{
				break;
			}
		}

		if (!l_addedAny)
		{
			break;
		}
	}

	if (l_mergedSnippets.empty())
	{
		return p_promptText;
	}

	return BuildRagContextBlock(l_mergedSnippets) + "User prompt:\n" + p_promptText;
}

bool TriggerProjectRagScan(AppState& p_app, const bool p_reusePreviousSource, const fs::path& p_fallbackSourceRoot, std::string* p_errorOut)
{
	const auto l_normalizeRoot = [](const fs::path& p_sourceRoot)
	{
		if (p_sourceRoot.empty())
		{
			return fs::path{};
		}

		std::error_code l_ec;
		const fs::path l_absoluteRoot = fs::absolute(p_sourceRoot, l_ec);
		return l_ec ? p_sourceRoot.lexically_normal() : l_absoluteRoot.lexically_normal();
	};

	std::vector<fs::path> l_sourceRoots;
	const fs::path l_requestedRoot = l_normalizeRoot(p_fallbackSourceRoot);

	if (const ChatSession* lcp_selectedChat = SelectedChatForRag(p_app); lcp_selectedChat != nullptr)
	{
		l_sourceRoots = ResolveRagSourceRootsForChat(p_app, *lcp_selectedChat, p_fallbackSourceRoot);
	}

	if (l_sourceRoots.empty())
	{
		l_sourceRoots.push_back(l_requestedRoot.empty() ? ResolveProjectRagSourceRoot(p_app, p_fallbackSourceRoot) : l_requestedRoot);
	}

	if (!l_requestedRoot.empty())
	{
		const auto l_requestedIt = std::find_if(l_sourceRoots.begin(), l_sourceRoots.end(), [&](const fs::path& p_sourceRoot) { return l_normalizeRoot(p_sourceRoot).generic_string() == l_requestedRoot.generic_string(); });

		if (l_requestedIt != l_sourceRoots.end() && l_requestedIt != l_sourceRoots.begin())
		{
			std::rotate(l_sourceRoots.begin(), l_requestedIt, l_requestedIt + 1);
		}
	}

	const fs::path l_workspaceRoot = l_sourceRoots.front();
	const std::string l_workspaceDisplay = l_workspaceRoot.empty() ? "<unset>" : l_workspaceRoot.string();
	const std::string l_workspaceKey = l_workspaceRoot.lexically_normal().generic_string();

	if (l_sourceRoots.size() > 1)
	{
		AppendRagScanReport(p_app, "Multiple RAG source folders are selected for this chat; scan action targets the first folder: " + l_workspaceDisplay);
	}

	std::error_code l_ec;

	if (l_workspaceRoot.empty() || !fs::exists(l_workspaceRoot, l_ec) || !fs::is_directory(l_workspaceRoot, l_ec))
	{
		AppendRagScanReport(p_app, "Scan start rejected: source directory is invalid (" + l_workspaceDisplay + ").");
		p_app.open_rag_console_popup = true;

		if (p_errorOut != nullptr)
		{
			*p_errorOut = "RAG source directory is invalid or missing.";
		}

		return false;
	}

	p_app.settings.rag_scan_max_tokens = std::clamp(p_app.settings.rag_scan_max_tokens, 0, 32768);

	if (!p_reusePreviousSource)
	{
		if (p_app.settings.rag_scan_max_tokens > 0)
		{
			const fs::path l_stagingRoot = BuildRagTokenCappedStagingRoot(p_app, l_workspaceKey);
			std::size_t l_stagedFiles = 0;
			std::string l_stageError;

			if (!BuildRagTokenCappedStagingTree(l_workspaceRoot, l_stagingRoot, p_app.settings.rag_scan_max_tokens, &l_stagedFiles, &l_stageError))
			{
				const std::string l_failure = l_stageError.empty() ? "Failed to build token-capped staging source." : l_stageError;
				AppendRagScanReport(p_app, "Scan start rejected: " + l_failure);
				p_app.open_rag_console_popup = true;

				if (p_errorOut != nullptr)
				{
					*p_errorOut = l_failure;
				}

				return false;
			}

			p_app.rag_index_service.SetScanSourceOverride(l_workspaceRoot, l_stagingRoot);
			std::ostringstream l_report;
			l_report << "Using token-capped staging source: " << l_stagingRoot.string() << " (" << l_stagedFiles << " files) | embedding token cap=" << p_app.settings.rag_scan_max_tokens;
			AppendRagScanReport(p_app, l_report.str());
		}
		else
		{
			p_app.rag_index_service.ClearScanSourceOverride(l_workspaceRoot);
		}
	}

	const bool l_hasLocalModels = !p_app.rag_index_service.ListModels().empty();
	const RagRefreshResult l_refresh = p_reusePreviousSource ? p_app.rag_index_service.RescanPreviousSource(l_workspaceRoot) : p_app.rag_index_service.RebuildIndex(l_workspaceRoot);

	if (!l_refresh.ok)
	{
		p_app.rag_last_refresh_by_workspace[l_workspaceKey] = l_refresh.error;
		AppendRagScanReport(p_app, "Scan start failed: " + l_refresh.error);
		p_app.open_rag_console_popup = true;

		if (p_errorOut != nullptr)
		{
			*p_errorOut = l_refresh.error;
		}

		return false;
	}

	p_app.rag_scan_workspace_key = l_workspaceKey;
	p_app.rag_scan_state = p_app.rag_index_service.FetchState();
	p_app.rag_scan_status_last_emit_s = ImGui::GetTime();
	p_app.rag_finished_visible_until_s = 0.0;
	p_app.rag_last_refresh_by_workspace[l_workspaceKey] = p_reusePreviousSource ? "RAG rescan started (previous source)." : "RAG scan started.";
	p_app.status_line = p_reusePreviousSource ? "RAG rescan started (previous source)." : "RAG scan started.";
	AppendRagScanReport(p_app, (p_reusePreviousSource ? "Rescan started (previous source)." : "Scan started.") + std::string(" Source: ") + l_workspaceRoot.string());
	p_app.open_rag_console_popup = true;

	if (!l_hasLocalModels)
	{
		p_app.status_line += " (no local .gguf detected; relying on llama server if available)";
		AppendRagScanReport(p_app, "No local .gguf models detected; scan relies on configured llama server.");
	}

	if (p_errorOut != nullptr)
	{
		p_errorOut->clear();
	}

	return true;
}

void PollRagScanState(AppState& p_app)
{
	const RagScanState l_previousState = p_app.rag_scan_state;
	p_app.rag_scan_state = p_app.rag_index_service.FetchState();
	const double l_now = ImGui::GetTime();
	const bool l_transitionedToFinished = l_previousState.lifecycle != RagScanLifecycleState::Finished && p_app.rag_scan_state.lifecycle == RagScanLifecycleState::Finished;

	if (l_transitionedToFinished)
	{
		p_app.rag_finished_visible_until_s = l_now + 8.0;

		if (!p_app.rag_scan_workspace_key.empty())
		{
			std::string l_finished = "Finished";

			if (p_app.rag_scan_state.vector_database_size > 0)
			{
				l_finished += " | " + std::to_string(p_app.rag_scan_state.vector_database_size) + " vectors";
			}

			p_app.rag_last_refresh_by_workspace[p_app.rag_scan_workspace_key] = l_finished;
			p_app.rag_last_rebuild_at_by_workspace[p_app.rag_scan_workspace_key] = TimestampNow();
		}

		p_app.status_line = "RAG scan finished: " + std::to_string(p_app.rag_scan_state.files_processed) + "/" + std::to_string(p_app.rag_scan_state.total_files) + " files";

		if (p_app.rag_scan_state.vector_database_size > 0)
		{
			p_app.status_line += " | " + std::to_string(p_app.rag_scan_state.vector_database_size) + " vectors";
		}

		AppendRagScanReport(p_app, p_app.status_line);
		p_app.rag_scan_status_last_emit_s = l_now;
		return;
	}

	if (p_app.rag_scan_state.lifecycle == RagScanLifecycleState::Running && !p_app.rag_scan_workspace_key.empty())
	{
		std::ostringstream l_running;
		l_running << "Running";

		if (p_app.rag_scan_state.total_files > 0)
		{
			l_running << " " << p_app.rag_scan_state.files_processed << "/" << p_app.rag_scan_state.total_files << " files";
		}

		if (p_app.rag_scan_state.vector_database_size > 0)
		{
			l_running << " | " << p_app.rag_scan_state.vector_database_size << " vectors";
		}

		p_app.rag_last_refresh_by_workspace[p_app.rag_scan_workspace_key] = l_running.str();

		const bool l_changedProgress = l_previousState.lifecycle != RagScanLifecycleState::Running || l_previousState.files_processed != p_app.rag_scan_state.files_processed || l_previousState.total_files != p_app.rag_scan_state.total_files || l_previousState.vector_database_size != p_app.rag_scan_state.vector_database_size;

		if (l_changedProgress && (l_now - p_app.rag_scan_status_last_emit_s >= 0.33 || l_previousState.lifecycle != RagScanLifecycleState::Running))
		{
			p_app.status_line = "RAG scan: " + l_running.str();
			AppendRagScanReport(p_app, p_app.status_line);
			p_app.rag_scan_status_last_emit_s = l_now;
		}

		return;
	}

	if (l_previousState.lifecycle == RagScanLifecycleState::Running && p_app.rag_scan_state.lifecycle == RagScanLifecycleState::Stopped)
	{
		if (!p_app.rag_scan_state.error.empty())
		{
			p_app.status_line = "RAG scan failed: " + p_app.rag_scan_state.error;
		}
		else if (l_previousState.total_files == 0)
		{
			p_app.status_line = "RAG scan stopped quickly: no indexable files found "
			                    "(.cpp/.h/.md/.txt/etc) in source directory.";
		}
		else if (l_previousState.vector_database_size == 0)
		{
			p_app.status_line = "RAG scan stopped with 0 vectors. Check embedding model "
			                    "(.gguf) or UAM_EMBEDDING_MODEL_PATH.";
		}
		else
		{
			p_app.status_line = "RAG scan stopped.";
		}

		AppendRagScanReport(p_app, p_app.status_line);
		p_app.rag_scan_status_last_emit_s = l_now;
	}
}

RagScanState EffectiveRagScanState(const AppState& p_app)
{
	RagScanState l_state = p_app.rag_scan_state;

	if (l_state.lifecycle == RagScanLifecycleState::Stopped && p_app.rag_finished_visible_until_s > ImGui::GetTime())
	{
		l_state.lifecycle = RagScanLifecycleState::Finished;
	}

	return l_state;
}

std::string BuildRagStatusText(const AppState& p_app)
{
	const RagScanState l_state = EffectiveRagScanState(p_app);

	if (l_state.lifecycle == RagScanLifecycleState::Finished)
	{
		return "RAG: Finished";
	}

	if (l_state.lifecycle == RagScanLifecycleState::Running)
	{
		std::ostringstream l_out;
		l_out << "RAG: Running";

		if (l_state.total_files > 0)
		{
			l_out << " " << l_state.files_processed << "/" << l_state.total_files << " files";
		}
		else
		{
			l_out << " (scanning...)";
		}

		if (l_state.vector_database_size > 0)
		{
			if (l_state.total_files > 0)
			{
				l_out << " | ";
			}
			else
			{
				l_out << " ";
			}

			l_out << l_state.vector_database_size << " vectors";
		}

		return l_out.str();
	}

	return "RAG: Stopped";
}

void EnsureRagManualQueryWorkspaceState(AppState& p_app, const std::string& p_workspaceKey)
{
	if (p_app.rag_manual_query_workspace_key == p_workspaceKey)
	{
		return;
	}

	p_app.rag_manual_query_workspace_key = p_workspaceKey;
	p_app.rag_manual_query_results.clear();
	p_app.rag_manual_query_error.clear();
	p_app.rag_manual_query_last_query.clear();
}

void AppendRagScanReport(AppState& p_app, const std::string& p_message)
{
	const std::string l_trimmed = Trim(p_message);

	if (l_trimmed.empty())
	{
		return;
	}

	if (!p_app.rag_scan_reports.empty())
	{
		const std::string& l_last = p_app.rag_scan_reports.back();
		const std::size_t l_separator = l_last.find(" | ");

		if (l_separator != std::string::npos && l_last.substr(l_separator + 3) == l_trimmed)
		{
			return;
		}
	}

	p_app.rag_scan_reports.push_back(TimestampNow() + " | " + l_trimmed);
	constexpr std::size_t l_kMaxRagReports = 320;

	if (p_app.rag_scan_reports.size() > l_kMaxRagReports)
	{
		const std::size_t l_trimCount = p_app.rag_scan_reports.size() - l_kMaxRagReports;
		p_app.rag_scan_reports.erase(p_app.rag_scan_reports.begin(), p_app.rag_scan_reports.begin() + static_cast<std::ptrdiff_t>(l_trimCount));
	}

	p_app.rag_scan_reports_scroll_to_bottom = true;
}

void RunRagManualTestQuery(AppState& p_app, const std::filesystem::path& p_workspaceRoot)
{
	p_app.rag_manual_query_max = std::clamp(p_app.rag_manual_query_max, 1, 50);
	p_app.rag_manual_query_min = std::clamp(p_app.rag_manual_query_min, 1, p_app.rag_manual_query_max);
	p_app.rag_manual_query_running = true;
	p_app.rag_manual_query_error.clear();
	p_app.rag_manual_query_last_query = p_app.rag_manual_query_input;
	std::string l_queryError;
	p_app.rag_manual_query_results = p_app.rag_index_service.Retrieve(p_workspaceRoot, p_app.rag_manual_query_input, static_cast<std::size_t>(p_app.rag_manual_query_max), static_cast<std::size_t>(p_app.rag_manual_query_min), &l_queryError);
	p_app.rag_manual_query_running = false;
	p_app.rag_manual_query_error = l_queryError;

	if (l_queryError.empty())
	{
		p_app.status_line = "RAG test query completed.";
		AppendRagScanReport(p_app, "Manual query returned " + std::to_string(p_app.rag_manual_query_results.size()) + " snippet(s).");
	}
	else
	{
		p_app.status_line = "RAG test query failed: " + l_queryError;
		AppendRagScanReport(p_app, "Manual query failed: " + l_queryError);
	}
}

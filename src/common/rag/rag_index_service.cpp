#include "common/rag/rag_index_service.h"

#include "common/rag/ollama_engine_client.h"
#include "common/rag/ollama_engine_service.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>

namespace fs = std::filesystem;

#if 0

namespace
{
	namespace fs = std::filesystem;

	std::string Trim(const std::string& value)
	{
		const std::size_t start = value.find_first_not_of(" \t\r\n");

		if (start == std::string::npos)
		{
			return "";
		}

		const std::size_t end = value.find_last_not_of(" \t\r\n");
		return value.substr(start, end - start + 1);
	}

	std::string ToLower(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		return value;
	}

	std::string TruncateSnippet(const std::string& text, const std::size_t max_chars)
	{
		if (text.size() <= max_chars)
		{
			return text;
		}

		if (max_chars <= 3)
		{
			return text.substr(0, max_chars);
		}

		return text.substr(0, max_chars - 3) + "...";
	}

	std::uint64_t Fnv1a64(const std::string& text)
	{
		std::uint64_t hash = 1469598103934665603ULL;

		for (const unsigned char ch : text)
		{
			hash ^= static_cast<std::uint64_t>(ch);
			hash *= 1099511628211ULL;
		}

		return hash;
	}

	std::string Hex64(const std::uint64_t value)
	{
		std::ostringstream out;
		out << std::hex << value;
		return out.str();
	}

	int ParsePositiveLineNumber(const std::string& text, const int fallback)
	{
		const std::string trimmed = Trim(text);

		if (trimmed.empty())
		{
			return fallback;
		}

		int value = fallback;

		try
		{
			value = std::stoi(trimmed);
		}
		catch (...)
		{
			return fallback;
		}

		return std::max(1, value);
	}

	bool IsLikelyBinaryBlob(const std::string& content)
	{
		return content.find('\0') != std::string::npos;
	}

	std::vector<std::string> QueryTokensLower(const std::string& query)
	{
		std::vector<std::string> tokens;
		std::string current;

		for (const char ch : query)
		{
			if (std::isalnum(static_cast<unsigned char>(ch)) != 0)
			{
				current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
			}
			else if (!current.empty())
			{
				tokens.push_back(current);
				current.clear();
			}
		}

		if (!current.empty())
		{
			tokens.push_back(current);
		}

		return tokens;
	}

	std::vector<RagSnippet> RetrieveLexicalFallback(const fs::path& workspace_root, const std::string& query, const std::size_t max_results, const std::size_t max_file_bytes, const std::size_t max_snippet_chars)
	{
		struct Candidate
		{
			RagSnippet snippet;
			int score = 0;
		};

		const std::vector<std::string> query_tokens = QueryTokensLower(query);

		if (query_tokens.empty())
		{
			return {};
		}

		std::vector<Candidate> candidates;
		std::error_code ec;

		for (const auto& entry : fs::recursive_directory_iterator(workspace_root, fs::directory_options::skip_permission_denied, ec))
		{
			if (ec || !entry.is_regular_file(ec))
			{
				ec.clear();
				continue;
			}

			if (entry.file_size(ec) > max_file_bytes)
			{
				ec.clear();
				continue;
			}

			std::ifstream in(entry.path(), std::ios::binary);

			if (!in.good())
			{
				continue;
			}

			std::ostringstream buffer;
			buffer << in.rdbuf();
			std::string text = buffer.str();

			if (IsLikelyBinaryBlob(text))
			{
				continue;
			}

			std::string text_lower = ToLower(text);

			int score = 0;

			for (const std::string& token : query_tokens)
			{
				std::size_t pos = text_lower.find(token);

				while (pos != std::string::npos)
				{
					++score;
					pos = text_lower.find(token, pos + token.size());
				}
			}

			if (score <= 0)
			{
				continue;
			}

			RagSnippet snippet;
			snippet.relative_path = fs::relative(entry.path(), workspace_root, ec).generic_string();
			ec.clear();
			snippet.text = TruncateSnippet(text, max_snippet_chars);
			snippet.start_line = 1;
			snippet.end_line = 1;
			snippet.score = static_cast<double>(score);
			candidates.push_back(Candidate{snippet, score});
		}

		const auto sort_candidates_by_score_then_path = [](const Candidate& lhs, const Candidate& rhs)
		{
			if (lhs.score != rhs.score)
			{
				return lhs.score > rhs.score;
			}

			return lhs.snippet.relative_path < rhs.snippet.relative_path;
		};
		std::sort(candidates.begin(), candidates.end(), sort_candidates_by_score_then_path);

		std::vector<RagSnippet> snippets;
		snippets.reserve(std::min<std::size_t>(max_results, candidates.size()));

		for (std::size_t i = 0; i < candidates.size() && i < max_results; ++i)
		{
			snippets.push_back(std::move(candidates[i].snippet));
		}

		return snippets;
	}

	std::unordered_map<std::string, std::size_t> CollectLexicalFileHashes(const fs::path& workspace_root, const std::size_t max_file_bytes)
	{
		std::unordered_map<std::string, std::size_t> hashes;
		std::error_code ec;

		for (const auto& entry : fs::recursive_directory_iterator(workspace_root, fs::directory_options::skip_permission_denied, ec))
		{
			if (ec || !entry.is_regular_file(ec))
			{
				ec.clear();
				continue;
			}

			if (entry.file_size(ec) > max_file_bytes)
			{
				ec.clear();
				continue;
			}

			std::ifstream in(entry.path(), std::ios::binary);

			if (!in.good())
			{
				continue;
			}

			std::ostringstream buffer;
			buffer << in.rdbuf();
			const std::string content = buffer.str();

			if (IsLikelyBinaryBlob(content))
			{
				continue;
			}

			const fs::path relative = fs::relative(entry.path(), workspace_root, ec);
			ec.clear();
			hashes[relative.generic_string()] = std::hash<std::string>{}(content);
		}

		return hashes;
	}

} // namespace

RagIndexService::RagIndexService() : config_(Config{}), model_folder_(std::filesystem::current_path() / "models"), model_engine_client_(&OllamaEngineService::Instance().Client())
{
	SetConfig(config_);

	if (model_engine_client_ != nullptr)
	{
		model_engine_client_->SetModelFolder(model_folder_);
	}
}

RagIndexService::RagIndexService(const Config& config) : config_(config), model_folder_(std::filesystem::current_path() / "models"), model_engine_client_(&OllamaEngineService::Instance().Client())
{
	SetConfig(config_);

	if (model_engine_client_ != nullptr)
	{
		model_engine_client_->SetModelFolder(model_folder_);
	}
}

RagIndexService::~RagIndexService() = default;

void RagIndexService::SetConfig(const Config& config)
{
	config_ = config;
	config_.vector_backend = (ToLower(config_.vector_backend) == "none") ? "none" : "ollama-engine";
	config_.vector_enabled = config_.vector_enabled && (config_.vector_backend != "none");
	config_.top_k = std::clamp(config_.top_k, 1, 20);
	config_.max_snippet_chars = std::clamp<std::size_t>(config_.max_snippet_chars, 120, 4000);
	config_.max_file_bytes = std::clamp<std::size_t>(config_.max_file_bytes, 16 * 1024, 20 * 1024 * 1024);
	config_.vector_dimensions = std::clamp<std::size_t>(config_.vector_dimensions, 32, 4096);
	config_.vector_max_tokens = std::clamp<std::size_t>(config_.vector_max_tokens, 0, 32768);

	if (model_engine_client_ != nullptr)
	{
		model_engine_client_->SetEmbeddingDimensions(config_.vector_dimensions);
		model_engine_client_->SetEmbeddingMaxTokens(config_.vector_max_tokens);
	}
}

const RagIndexService::Config& RagIndexService::GetConfig() const
{
	return config_;
}

void RagIndexService::SetModelFolder(const std::filesystem::path& model_folder)
{
	model_folder_ = model_folder.empty() ? (std::filesystem::current_path() / "models") : model_folder;

	if (model_engine_client_ != nullptr)
	{
		model_engine_client_->SetModelFolder(model_folder_);
		model_engine_client_->SetEmbeddingDimensions(config_.vector_dimensions);
		model_engine_client_->SetEmbeddingMaxTokens(config_.vector_max_tokens);
	}
}

std::vector<std::string> RagIndexService::ListModels()
{
	if (config_.vector_backend == "none")
	{
		return {};
	}

	if (model_engine_client_ == nullptr)
	{
		model_engine_client_ = &OllamaEngineService::Instance().Client();
	}

	model_engine_client_->SetModelFolder(model_folder_);
	model_engine_client_->SetEmbeddingDimensions(config_.vector_dimensions);
	model_engine_client_->SetEmbeddingMaxTokens(config_.vector_max_tokens);
	return model_engine_client_->ListModels();
}

bool RagIndexService::LoadModel(const std::string& model_name, std::string* error_out)
{
	if (config_.vector_backend == "none")
	{
		if (error_out != nullptr)
		{
			*error_out = "Vector backend is disabled.";
		}

		return false;
	}

	if (model_engine_client_ == nullptr)
	{
		model_engine_client_ = &OllamaEngineService::Instance().Client();
	}

	model_engine_client_->SetModelFolder(model_folder_);
	model_engine_client_->SetEmbeddingDimensions(config_.vector_dimensions);
	model_engine_client_->SetEmbeddingMaxTokens(config_.vector_max_tokens);
	const bool ok = model_engine_client_->Load(model_name, error_out);

	if (ok)
	{
		loaded_vector_model_id_ = model_name;
	}

	return ok;
}

void RagIndexService::SetScanSourceOverride(const std::filesystem::path& workspace_root, const std::filesystem::path& scan_source_root)
{
	const std::string workspace_key = NormalizeWorkspaceKey(workspace_root);

	if (workspace_key.empty())
	{
		return;
	}

	const std::string source_key = NormalizeWorkspaceKey(scan_source_root);

	if (source_key.empty())
	{
		scan_source_override_by_workspace_.erase(workspace_key);
		return;
	}

	scan_source_override_by_workspace_[workspace_key] = source_key;
}

void RagIndexService::ClearScanSourceOverride(const std::filesystem::path& workspace_root)
{
	const std::string workspace_key = NormalizeWorkspaceKey(workspace_root);

	if (workspace_key.empty())
	{
		return;
	}

	scan_source_override_by_workspace_.erase(workspace_key);
}

RagRefreshResult RagIndexService::RefreshIndexIncremental(const std::filesystem::path& workspace_root)
{
	return ScanWorkspace(workspace_root, false);
}

RagRefreshResult RagIndexService::RebuildIndex(const std::filesystem::path& workspace_root)
{
	return ScanWorkspace(workspace_root, false);
}

RagRefreshResult RagIndexService::RescanPreviousSource(const std::filesystem::path& workspace_root)
{
	return ScanWorkspace(workspace_root, true);
}

RagRefreshResult RagIndexService::ScanWorkspace(const std::filesystem::path& workspace_root, const bool reuse_previous_source)
{
	RagRefreshResult result;

	if (!config_.enabled)
	{
		return result;
	}

	std::error_code ec;

	if (workspace_root.empty() || !fs::exists(workspace_root, ec) || !fs::is_directory(workspace_root, ec))
	{
		result.ok = false;
		result.error = "Workspace root is missing or not a directory.";
		last_scan_error_ = result.error;
		return result;
	}

	if (!config_.vector_enabled || config_.vector_backend == "none")
	{
		const std::string workspace_key = NormalizeWorkspaceKey(workspace_root);
		const auto new_hashes = CollectLexicalFileHashes(workspace_root, config_.max_file_bytes);
		const auto previous_it = lexical_fallback_hashes_by_workspace_.find(workspace_key);
		int updated_files = static_cast<int>(new_hashes.size());

		if (previous_it != lexical_fallback_hashes_by_workspace_.end())
		{
			updated_files = 0;

			for (const auto& item : new_hashes)
			{
				const auto old_item = previous_it->second.find(item.first);

				if (old_item == previous_it->second.end() || old_item->second != item.second)
				{
					++updated_files;
				}
			}

			for (const auto& old_item : previous_it->second)
			{
				if (new_hashes.find(old_item.first) == new_hashes.end())
				{
					++updated_files;
				}
			}
		}

		lexical_fallback_hashes_by_workspace_[workspace_key] = new_hashes;
		result.indexed_files = static_cast<int>(new_hashes.size());
		result.updated_files = updated_files;
		last_scan_error_.clear();
		return result;
	}

	if (model_engine_client_ == nullptr)
	{
		model_engine_client_ = &OllamaEngineService::Instance().Client();
		model_engine_client_->SetModelFolder(model_folder_);
		model_engine_client_->SetEmbeddingDimensions(config_.vector_dimensions);
		model_engine_client_->SetEmbeddingMaxTokens(config_.vector_max_tokens);
	}

	if (!Trim(config_.vector_model_id).empty() && loaded_vector_model_id_ != config_.vector_model_id)
	{
		std::string load_error;

		if (!model_engine_client_->Load(config_.vector_model_id, &load_error))
		{
			result.ok = false;
			result.error = load_error.empty() ? "Failed to load configured vector model." : load_error;
			last_scan_error_ = result.error;
			return result;
		}

		loaded_vector_model_id_ = config_.vector_model_id;
	}

	const std::string workspace_key = NormalizeWorkspaceKey(workspace_root);

	std::string setup_error;

	if (!ConfigureWorkspaceDatabase(workspace_root, &setup_error))
	{
		const std::string workspace_key = NormalizeWorkspaceKey(workspace_root);
		const auto new_hashes = CollectLexicalFileHashes(workspace_root, config_.max_file_bytes);
		lexical_fallback_hashes_by_workspace_[workspace_key] = new_hashes;
		result.indexed_files = static_cast<int>(new_hashes.size());
		result.updated_files = static_cast<int>(new_hashes.size());
		result.ok = true;
		last_scan_error_.clear();
		return result;
	}

	std::string scan_error;
	std::optional<std::string> vector_file;

	if (!reuse_previous_source)
	{
		const auto it = scan_source_override_by_workspace_.find(workspace_key);
		vector_file = (it != scan_source_override_by_workspace_.end() && !it->second.empty()) ? it->second : workspace_key;
	}

	if (!model_engine_client_->Scan(vector_file, &scan_error))
	{
		const std::string workspace_key = NormalizeWorkspaceKey(workspace_root);
		const auto new_hashes = CollectLexicalFileHashes(workspace_root, config_.max_file_bytes);
		const auto previous_it = lexical_fallback_hashes_by_workspace_.find(workspace_key);
		int updated_files = static_cast<int>(new_hashes.size());

		if (previous_it != lexical_fallback_hashes_by_workspace_.end())
		{
			updated_files = 0;

			for (const auto& item : new_hashes)
			{
				const auto old_item = previous_it->second.find(item.first);

				if (old_item == previous_it->second.end() || old_item->second != item.second)
				{
					++updated_files;
				}
			}

			for (const auto& old_item : previous_it->second)
			{
				if (new_hashes.find(old_item.first) == new_hashes.end())
				{
					++updated_files;
				}
			}
		}

		lexical_fallback_hashes_by_workspace_[workspace_key] = new_hashes;
		result.indexed_files = static_cast<int>(new_hashes.size());
		result.updated_files = updated_files;
		result.ok = true;
		last_scan_error_.clear();
		return result;
	}

	last_scan_error_.clear();
	const RagScanState state = FetchState();
	const int lexical_count = static_cast<int>(CollectLexicalFileHashes(workspace_root, config_.max_file_bytes).size());
	result.indexed_files = (state.total_files > 0) ? static_cast<int>(state.total_files) : lexical_count;
	result.updated_files = (state.files_processed > 0) ? static_cast<int>(state.files_processed) : lexical_count;
	return result;
}

std::vector<RagSnippet> RagIndexService::RetrieveTopK(const std::filesystem::path& workspace_root, const std::string& query)
{
	return Retrieve(workspace_root, query, static_cast<std::size_t>(config_.top_k), 1);
}

std::vector<RagSnippet> RagIndexService::Retrieve(const std::filesystem::path& workspace_root, const std::string& query, const std::size_t max_results, const std::size_t min_results, std::string* error_out)
{
	std::vector<RagSnippet> snippets;

	if (!config_.enabled)
	{
		return snippets;
	}

	const std::string trimmed_query = Trim(query);

	if (trimmed_query.empty())
	{
		return snippets;
	}

	if (!config_.vector_enabled)
	{
		return RetrieveLexicalFallback(workspace_root, trimmed_query, std::max<std::size_t>(1, max_results), config_.max_file_bytes, config_.max_snippet_chars);
	}

	if (model_engine_client_ == nullptr)
	{
		model_engine_client_ = &OllamaEngineService::Instance().Client();
		model_engine_client_->SetModelFolder(model_folder_);
		model_engine_client_->SetEmbeddingDimensions(config_.vector_dimensions);
		model_engine_client_->SetEmbeddingMaxTokens(config_.vector_max_tokens);
	}

	if (!Trim(config_.vector_model_id).empty() && loaded_vector_model_id_ != config_.vector_model_id)
	{
		std::string load_error;

		if (!model_engine_client_->Load(config_.vector_model_id, &load_error))
		{
			if (error_out != nullptr)
			{
				*error_out = load_error.empty() ? "Failed to load configured vector model." : load_error;
			}

			return snippets;
		}

		loaded_vector_model_id_ = config_.vector_model_id;
	}

	std::string setup_error;

	if (!ConfigureWorkspaceDatabase(workspace_root, &setup_error))
	{
		return RetrieveLexicalFallback(workspace_root, trimmed_query, std::max<std::size_t>(1, max_results), config_.max_file_bytes, config_.max_snippet_chars);
	}

	const std::string workspace_key = NormalizeWorkspaceKey(workspace_root);
	const std::string database_name = WorkspaceDatabaseName(workspace_key);

	std::string load_error;

	if (!model_engine_client_->LoadRagDatabases({database_name}, &load_error))
	{
		const RagScanState state = FetchState();
		const bool running_same_workspace = (state.lifecycle == RagScanLifecycleState::Running && workspace_key == active_workspace_key_);

		if (!running_same_workspace)
		{
			if (error_out != nullptr)
			{
				*error_out = load_error.empty() ? "Failed to load RAG database." : load_error;
			}

			return RetrieveLexicalFallback(workspace_root, trimmed_query, std::max<std::size_t>(1, max_results), config_.max_file_bytes, config_.max_snippet_chars);
		}
	}

	const std::size_t clamped_max = std::max<std::size_t>(1, max_results);
	const std::size_t clamped_min = std::min(clamped_max, std::max<std::size_t>(1, min_results));

	const std::vector<std::string> raw_snippets = model_engine_client_->FetchRelevantInfo(trimmed_query, clamped_max, clamped_min);
	snippets.reserve(raw_snippets.size());

	for (const std::string& raw_snippet : raw_snippets)
	{
		snippets.push_back(ParseSnippet(raw_snippet, config_.max_snippet_chars));
	}

	if (snippets.empty())
	{
		return RetrieveLexicalFallback(workspace_root, trimmed_query, std::max<std::size_t>(1, max_results), config_.max_file_bytes, config_.max_snippet_chars);
	}

	return snippets;
}

RagScanState RagIndexService::FetchState()
{
	RagScanState state;

	if (model_engine_client_ == nullptr)
	{
		state.error = last_scan_error_;
		return state;
	}

	const ollama_engine::VectorisationStateResponse engine_state = model_engine_client_->FetchVectorisationState();

	switch (engine_state.pVectorisationLifecycleState)
	{
	case ollama_engine::VectorisationLifecycleState::Running:
		state.lifecycle = RagScanLifecycleState::Running;
		break;
	case ollama_engine::VectorisationLifecycleState::Finished:
		state.lifecycle = RagScanLifecycleState::Finished;
		break;
	case ollama_engine::VectorisationLifecycleState::Stopped:
	default:
		state.lifecycle = RagScanLifecycleState::Stopped;
		break;
	}

	state.vector_database_size = engine_state.piVectorDatabaseSize;
	state.files_processed = engine_state.piFilesProcessed;
	state.total_files = engine_state.piTotalFiles;

	if (state.lifecycle == RagScanLifecycleState::Stopped)
	{
		state.error = last_scan_error_;
	}
	else
	{
		state.error.clear();
	}

	return state;
}

bool RagIndexService::ConfigureWorkspaceDatabase(const std::filesystem::path& workspace_root, std::string* error_out)
{
	if (model_engine_client_ == nullptr)
	{
		model_engine_client_ = &OllamaEngineService::Instance().Client();
		model_engine_client_->SetModelFolder(model_folder_);
		model_engine_client_->SetEmbeddingDimensions(config_.vector_dimensions);
		model_engine_client_->SetEmbeddingMaxTokens(config_.vector_max_tokens);
	}

	const std::string workspace_key = NormalizeWorkspaceKey(workspace_root);

	if (workspace_key.empty())
	{
		if (error_out != nullptr)
		{
			*error_out = "Workspace root is empty.";
		}

		return false;
	}

	const std::string database_name = WorkspaceDatabaseName(workspace_key);

	if (!model_engine_client_->SetRagOutputDatabase(database_name, error_out))
	{
		return false;
	}

	active_workspace_key_ = workspace_key;
	return true;
}

std::string RagIndexService::WorkspaceDatabaseName(const std::string& workspace_key)
{
	if (!Trim(config_.vector_database_name_override).empty())
	{
		return config_.vector_database_name_override;
	}

	const auto it = database_name_by_workspace_.find(workspace_key);

	if (it != database_name_by_workspace_.end())
	{
		return it->second;
	}

	const std::string database_name = "uam_" + Hex64(Fnv1a64(workspace_key));
	database_name_by_workspace_[workspace_key] = database_name;
	return database_name;
}

RagSnippet RagIndexService::ParseSnippet(const std::string& snippet_text, const std::size_t max_snippet_chars)
{
	RagSnippet snippet;

	if (snippet_text.empty())
	{
		return snippet;
	}

	const std::size_t newline = snippet_text.find('\n');
	const std::string header = (newline == std::string::npos) ? snippet_text : snippet_text.substr(0, newline);
	const std::string body = (newline == std::string::npos) ? std::string{} : snippet_text.substr(newline + 1);

	std::string location = header;

	if (!location.empty() && location.front() == '[')
	{
		const std::size_t close = location.find("] ");

		if (close != std::string::npos)
		{
			location = location.substr(close + 2);
		}
	}

	if (const std::size_t metadata = location.find(" ["); metadata != std::string::npos)
	{
		location = location.substr(0, metadata);
	}

	location = Trim(location);

	const std::size_t dash = location.rfind('-');
	const std::size_t colon = (dash == std::string::npos) ? std::string::npos : location.rfind(':', dash);

	if (colon != std::string::npos && dash != std::string::npos && dash > colon + 1)
	{
		snippet.relative_path = Trim(location.substr(0, colon));
		snippet.start_line = ParsePositiveLineNumber(location.substr(colon + 1, dash - colon - 1), 1);
		snippet.end_line = ParsePositiveLineNumber(location.substr(dash + 1), snippet.start_line);

		if (snippet.end_line < snippet.start_line)
		{
			snippet.end_line = snippet.start_line;
		}
	}
	else
	{
		snippet.relative_path = location;
		snippet.start_line = 1;
		snippet.end_line = 1;
	}

	snippet.text = TruncateSnippet(body.empty() ? snippet_text : body, max_snippet_chars);
	return snippet;
}

std::string RagIndexService::NormalizeWorkspaceKey(const std::filesystem::path& workspace_root)
{
	std::error_code ec;
	const fs::path absolute = fs::absolute(workspace_root, ec);
	const fs::path normalized = ec ? workspace_root.lexically_normal() : absolute.lexically_normal();
	return normalized.generic_string();
}
#endif

#include "common/rag/rag_index_service_common.inl"
#include "common/rag/rag_index_service_snippet.inl"
#include "common/rag/rag_index_service_lexical.inl"
#include "common/rag/rag_index_service_scan.inl"
#include "common/rag/rag_index_service_retrieval.inl"

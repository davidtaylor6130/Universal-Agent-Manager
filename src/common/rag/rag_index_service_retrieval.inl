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

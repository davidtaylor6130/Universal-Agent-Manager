std::string RagIndexService::NormalizeWorkspaceKey(const std::filesystem::path& workspace_root)
{
	std::error_code ec;
	const fs::path absolute = fs::absolute(workspace_root, ec);
	const fs::path normalized = ec ? workspace_root.lexically_normal() : absolute.lexically_normal();
	return normalized.generic_string();
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

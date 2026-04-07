namespace
{

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

	bool IsLikelyBinaryBlob(const std::string& content)
	{
		return content.find('\0') != std::string::npos;
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

namespace
{

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

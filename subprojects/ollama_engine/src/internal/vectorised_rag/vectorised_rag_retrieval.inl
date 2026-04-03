namespace fs = std::filesystem;

bool LoadRagDatabases(Context& pContext, const std::vector<std::string>& pVecSDatabaseInputs, const RuntimeOptions& pRuntimeOptions, std::string* pSErrorOut)
{
	{
		std::lock_guard<std::mutex> lGuard(pContext.pMutex);

		if (pContext.pbThreadRunning)
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Cannot load databases while scan is running.";
			}

			return false;
		}
	}

	if (pVecSDatabaseInputs.empty())
	{
		std::lock_guard<std::mutex> lGuard(pContext.pMutex);
		pContext.pVecPathLoadedDatabases.clear();
		return true;
	}

	std::vector<fs::path> lVecPathLoadedDatabases;

	for (const std::string& lSDatabaseInput : pVecSDatabaseInputs)
	{
		if (!ResolveDatabaseInput(lSDatabaseInput, pRuntimeOptions, &lVecPathLoadedDatabases, pSErrorOut))
		{
			return false;
		}
	}

	if (lVecPathLoadedDatabases.empty())
	{
		if (pSErrorOut != nullptr)
		{
			*pSErrorOut = "No databases resolved from LoadRagDatabases inputs.";
		}

		return false;
	}

	std::vector<fs::path> lVecPathUniqueDatabases;
	std::unordered_set<std::string> lSetSSeenKeys;

	for (const fs::path& lPathDatabaseCandidate : lVecPathLoadedDatabases)
	{
		std::error_code lErrorCode;
		const fs::path lPathAbsolute = fs::absolute(lPathDatabaseCandidate, lErrorCode);
		const fs::path lPathNormalized = (lErrorCode ? lPathDatabaseCandidate : lPathAbsolute).lexically_normal();
		const std::string lSKey = lPathNormalized.generic_string();

		if (lSetSSeenKeys.insert(lSKey).second)
		{
			lVecPathUniqueDatabases.push_back(lPathNormalized);
		}
	}

	for (const fs::path& lPathDatabase : lVecPathUniqueDatabases)
	{
		sqlite3* lPtrDatabase = nullptr;

		if (sqlite3_open(lPathDatabase.string().c_str(), &lPtrDatabase) != SQLITE_OK)
		{
			if (lPtrDatabase != nullptr)
			{
				sqlite3_close(lPtrDatabase);
			}

			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Failed to open database: " + lPathDatabase.string();
			}

			return false;
		}

		const auto lDatabaseCloser = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>(lPtrDatabase, &sqlite3_close);

		if (!HasChunksTable(lPtrDatabase))
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Database is missing required chunks table: " + lPathDatabase.string();
			}

			return false;
		}
	}

	std::lock_guard<std::mutex> lGuard(pContext.pMutex);
	pContext.pVecPathLoadedDatabases = std::move(lVecPathUniqueDatabases);
	return true;
}

std::vector<std::string> Fetch_Relevant_Info(Context& pContext, const std::string& pSPrompt, std::size_t piMax, std::size_t piMin, const RuntimeOptions& pRuntimeOptions)
{
	std::vector<std::string> lVecSSnippets;

	if (pSPrompt.empty())
	{
		return lVecSSnippets;
	}

	const std::optional<std::vector<float>> lOptVecfPromptEmbedding = BuildEmbedding(pContext, pRuntimeOptions, pSPrompt, nullptr);

	if (!lOptVecfPromptEmbedding.has_value() || lOptVecfPromptEmbedding->empty())
	{
		return lVecSSnippets;
	}

	const std::vector<float>& lVecfPromptEmbedding = *lOptVecfPromptEmbedding;

	std::vector<fs::path> lVecPathDatabases;
	{
		std::lock_guard<std::mutex> lGuard(pContext.pMutex);
		lVecPathDatabases = pContext.pVecPathLoadedDatabases;

		if (lVecPathDatabases.empty() && !pContext.pPathVectorDatabaseFile.empty())
		{
			lVecPathDatabases.push_back(pContext.pPathVectorDatabaseFile);
		}
	}

	if (lVecPathDatabases.empty())
	{
		return lVecSSnippets;
	}

	std::vector<fs::path> lVecPathUniqueDatabases;
	std::unordered_set<std::string> lSetSSeenDatabases;

	for (const fs::path& lPathDatabaseCandidate : lVecPathDatabases)
	{
		std::error_code lErrorCode;
		const fs::path lPathAbsolute = fs::absolute(lPathDatabaseCandidate, lErrorCode);
		const fs::path lPathNormalized = (lErrorCode ? lPathDatabaseCandidate : lPathAbsolute).lexically_normal();
		const std::string lSKey = lPathNormalized.generic_string();

		if (lSetSSeenDatabases.insert(lSKey).second)
		{
			lVecPathUniqueDatabases.push_back(lPathNormalized);
		}
	}

	static constexpr const char* kSql = "SELECT source_id, file_path, chunk_type, symbol_name, parent_symbol, start_line, end_line, raw_text, "
	                                    "embedding FROM chunks;";

	struct ScoredSnippet
	{
		std::string pSRendered;
		double pdScore = 0.0;
		bool pbLowInformation = false;
	};

	std::vector<ScoredSnippet> lVecScored;

	for (const fs::path& lPathDatabase : lVecPathUniqueDatabases)
	{
		sqlite3* lPtrDatabase = nullptr;

		if (sqlite3_open(lPathDatabase.string().c_str(), &lPtrDatabase) != SQLITE_OK)
		{
			if (lPtrDatabase != nullptr)
			{
				sqlite3_close(lPtrDatabase);
			}

			continue;
		}

		const auto lDatabaseCloser = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>(lPtrDatabase, &sqlite3_close);

		sqlite3_stmt* lPtrStatement = nullptr;

		if (sqlite3_prepare_v2(lPtrDatabase, kSql, -1, &lPtrStatement, nullptr) != SQLITE_OK)
		{
			continue;
		}

		while (sqlite3_step(lPtrStatement) == SQLITE_ROW)
		{
			const char* lPtrSourceId = reinterpret_cast<const char*>(sqlite3_column_text(lPtrStatement, 0));
			const char* lPtrFilePath = reinterpret_cast<const char*>(sqlite3_column_text(lPtrStatement, 1));
			const char* lPtrChunkType = reinterpret_cast<const char*>(sqlite3_column_text(lPtrStatement, 2));
			const char* lPtrSymbolName = reinterpret_cast<const char*>(sqlite3_column_text(lPtrStatement, 3));
			const char* lPtrParentSymbol = reinterpret_cast<const char*>(sqlite3_column_text(lPtrStatement, 4));
			const int liStartLine = sqlite3_column_int(lPtrStatement, 5);
			const int liEndLine = sqlite3_column_int(lPtrStatement, 6);
			const char* lPtrRawText = reinterpret_cast<const char*>(sqlite3_column_text(lPtrStatement, 7));
			const void* lPtrBlob = sqlite3_column_blob(lPtrStatement, 8);
			const int liBlobBytes = sqlite3_column_bytes(lPtrStatement, 8);

			if (lPtrFilePath == nullptr || lPtrChunkType == nullptr || lPtrRawText == nullptr)
			{
				continue;
			}

			const std::vector<float> lVecfChunkEmbedding = DeserializeEmbedding(lPtrBlob, liBlobBytes);

			if (lVecfChunkEmbedding.size() != lVecfPromptEmbedding.size())
			{
				continue;
			}

			const double ldCosine = CosineSimilarity(lVecfPromptEmbedding, lVecfChunkEmbedding);

			if (ldCosine <= 0.0)
			{
				continue;
			}

			double ldScore = ldCosine;
			const std::string lSChunkType = lPtrChunkType;
			const std::string lSRawText = lPtrRawText;
			ldScore += ChunkTypeScoreBias(lSChunkType);
			ldScore += RichnessScoreBonus(lSRawText);

			if (ldScore <= 0.0)
			{
				continue;
			}

			ScoredSnippet lScoredSnippet;
			lScoredSnippet.pdScore = ldScore;
			lScoredSnippet.pSRendered = FormatSnippet(lPtrSourceId == nullptr ? "" : lPtrSourceId, lPtrFilePath, liStartLine, liEndLine, lSChunkType, lPtrSymbolName == nullptr ? "" : lPtrSymbolName, lPtrParentSymbol == nullptr ? "" : lPtrParentSymbol, lSRawText);
			lScoredSnippet.pbLowInformation = IsLowInformationChunk(lSChunkType, lSRawText);
			lVecScored.push_back(std::move(lScoredSnippet));
		}

		sqlite3_finalize(lPtrStatement);
	}

	std::sort(lVecScored.begin(), lVecScored.end(), [](const ScoredSnippet& pLhs, const ScoredSnippet& pRhs) { return pLhs.pdScore > pRhs.pdScore; });

	const std::size_t liMaxCount = std::max<std::size_t>(1, piMax);
	const std::size_t liMinCount = std::min(liMaxCount, std::max<std::size_t>(1, piMin));
	std::unordered_set<std::string> lSetSRendered;

	for (const ScoredSnippet& lScoredSnippet : lVecScored)
	{
		if (lVecSSnippets.size() >= liMaxCount)
		{
			break;
		}

		if (lScoredSnippet.pbLowInformation)
		{
			continue;
		}

		if (lScoredSnippet.pdScore < 0.1 && lVecSSnippets.size() >= liMinCount)
		{
			break;
		}

		if (!lSetSRendered.insert(lScoredSnippet.pSRendered).second)
		{
			continue;
		}

		lVecSSnippets.push_back(lScoredSnippet.pSRendered);
	}

	for (std::size_t liIndex = 0; liIndex < lVecScored.size() && lVecSSnippets.size() < liMinCount; ++liIndex)
	{
		const std::string& lSRendered = lVecScored[liIndex].pSRendered;

		if (!lSetSRendered.insert(lSRendered).second)
		{
			continue;
		}

		lVecSSnippets.push_back(lSRendered);
	}

	return lVecSSnippets;
}

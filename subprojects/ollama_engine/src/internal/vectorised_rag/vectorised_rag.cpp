#include "vectorised_rag.h"

#include "../embedding_utils.h"
#include "../determanistic_hash/determanistic_hash.h"
#include "vectorised_rag_chunk_extraction.h"
#include "vectorised_rag_source_resolution.h"
#include "vectorised_rag_embedding.h"
#include "vectorised_rag_sqlite_persistence.h"
#include "vectorised_rag_retrieval.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sqlite3.h>
#include <tree_sitter/api.h>

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
// Real feature gate for the llama.cpp-backed embedding path.
#include "llama.h"
#endif

extern "C" const TSLanguage* tree_sitter_cpp(void);

namespace ollama_engine::internal::vectorised_rag
{

	namespace
	{
		namespace fs = std::filesystem;

		constexpr std::size_t kiDefaultChunkCharLimit = 4000;

		void UpdateRunningState(Context& pContext, const std::size_t piFilesProcessed, const std::size_t piTotalFiles, const std::size_t piVectorDatabaseSize)
		{
			std::lock_guard<std::mutex> lGuard(pContext.pMutex);
			pContext.pScanState.pState = StateValue::Running;
			pContext.pScanState.piFilesProcessed = piFilesProcessed;
			pContext.pScanState.piTotalFiles = piTotalFiles;
			pContext.pScanState.piVectorDatabaseSize = piVectorDatabaseSize;
		}

		void MarkScanStopped(Context& pContext, const std::string& pSError, const bool pbFinishedPending)
		{
			std::lock_guard<std::mutex> lGuard(pContext.pMutex);
			pContext.pbThreadRunning = false;
			pContext.pbFinishedPending = pbFinishedPending;
			pContext.pScanState.pState = StateValue::Stopped;
			pContext.pSError = pSError;
		}

		void RunScanWorker(Context& pContext, const SourceSpec pSourceSpec, const RuntimeOptions pRuntimeOptions)
		{
			sqlite3* lPtrDatabase = nullptr;
			fs::path lPathDatabase;

			if (!OpenDatabaseForSource(pSourceSpec.pSSourceId, pRuntimeOptions, &lPtrDatabase, &lPathDatabase))
			{
				MarkScanStopped(pContext, "Failed to open vector database.", false);
				return;
			}

			const auto lDatabaseCloser = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>(lPtrDatabase, &sqlite3_close);

			std::string lSError;

			if (!EnsureSchema(lPtrDatabase, &lSError))
			{
				MarkScanStopped(pContext, "Failed to initialize vector database schema: " + lSError, false);
				return;
			}

			{
				std::lock_guard<std::mutex> lGuard(pContext.pMutex);
				pContext.pSActiveSourceId = pSourceSpec.pSSourceId;
				pContext.pPathSourceRoot = pSourceSpec.pPathRoot;
				pContext.pPathVectorDatabaseFile = lPathDatabase;
			}

			const std::vector<fs::path> lVecPathFiles = CollectIndexableFiles(pSourceSpec.pPathRoot);
			const std::size_t liTotalFiles = lVecPathFiles.size();
			std::size_t liFilesProcessed = 0;
			std::size_t liVectorDatabaseSize = CountVectorRows(lPtrDatabase, pSourceSpec.pSSourceId);
			UpdateRunningState(pContext, liFilesProcessed, liTotalFiles, liVectorDatabaseSize);

			ExecSql(lPtrDatabase, "BEGIN IMMEDIATE;", nullptr);

			std::unordered_set<std::string> lSetSSeenPaths;
			std::size_t liIndexedChunks = 0;
			const std::size_t liChunkCharLimit = std::max<std::size_t>(pRuntimeOptions.piChunkCharLimit, kiDefaultChunkCharLimit);

			for (const fs::path& lPathFile : lVecPathFiles)
			{
				std::error_code lErrorCode;
				const fs::path lPathRelative = fs::relative(lPathFile, pSourceSpec.pPathRoot, lErrorCode);

				if (lErrorCode)
				{
					++liFilesProcessed;
					UpdateRunningState(pContext, liFilesProcessed, liTotalFiles, liVectorDatabaseSize);
					continue;
				}

				const std::string lSRelativePath = lPathRelative.generic_string();
				lSetSSeenPaths.insert(lSRelativePath);

				const std::optional<std::string> lOptSContent = ReadFileText(lPathFile);

				if (!lOptSContent.has_value() || IsLikelyBinary(*lOptSContent))
				{
					DeleteFileAndChunks(lPtrDatabase, pSourceSpec.pSSourceId, lSRelativePath);
					++liFilesProcessed;
					liVectorDatabaseSize = CountVectorRows(lPtrDatabase, pSourceSpec.pSSourceId);
					UpdateRunningState(pContext, liFilesProcessed, liTotalFiles, liVectorDatabaseSize);
					continue;
				}

				const std::string lSContentHash = determanistic_hash::HashTextHex(*lOptSContent);
				std::string lSPreviousHash;
				const bool lbHasPreviousHash = SelectFileHash(lPtrDatabase, pSourceSpec.pSSourceId, lSRelativePath, &lSPreviousHash);

				if (lbHasPreviousHash && lSPreviousHash == lSContentHash)
				{
					++liFilesProcessed;
					UpdateRunningState(pContext, liFilesProcessed, liTotalFiles, liVectorDatabaseSize);
					continue;
				}

				std::vector<ChunkRecord> lVecChunks;

				if (IsSupportedCppFile(lPathFile))
				{
					lVecChunks = ExtractChunksFromCppFile(pSourceSpec.pSSourceId, lSRelativePath, *lOptSContent, liChunkCharLimit);
				}
				else
				{
					lVecChunks = ExtractChunksFromDocumentationFile(pSourceSpec.pSSourceId, lSRelativePath, *lOptSContent, liChunkCharLimit);
				}

				DeleteChunksForFile(lPtrDatabase, pSourceSpec.pSSourceId, lSRelativePath);

				for (ChunkRecord& lChunk : lVecChunks)
				{
					std::string lSEmbeddingError;
					const std::optional<std::vector<float>> lOptVecfEmbedding = BuildEmbedding(pContext, pRuntimeOptions, lChunk.pSRawText, &lSEmbeddingError);

					if (!lOptVecfEmbedding.has_value() || lOptVecfEmbedding->empty())
					{
						continue;
					}

					lChunk.pVecfEmbedding = *lOptVecfEmbedding;

					if (InsertChunk(lPtrDatabase, lChunk))
					{
						++liIndexedChunks;
					}
				}

				const std::uint64_t liMtimeTicks = static_cast<std::uint64_t>(fs::last_write_time(lPathFile, lErrorCode).time_since_epoch().count());
				const std::uintmax_t liFileSize = fs::file_size(lPathFile, lErrorCode);
				UpsertFileRow(lPtrDatabase, pSourceSpec.pSSourceId, lSRelativePath, lSContentHash, liMtimeTicks, liFileSize);

				++liFilesProcessed;
				liVectorDatabaseSize = CountVectorRows(lPtrDatabase, pSourceSpec.pSSourceId);
				UpdateRunningState(pContext, liFilesProcessed, liTotalFiles, liVectorDatabaseSize);
			}

			const std::vector<std::string> lVecSIndexedFiles = SelectAllIndexedFiles(lPtrDatabase, pSourceSpec.pSSourceId);

			for (const std::string& lSIndexedFile : lVecSIndexedFiles)
			{
				if (lSetSSeenPaths.find(lSIndexedFile) != lSetSSeenPaths.end())
				{
					continue;
				}

				DeleteFileAndChunks(lPtrDatabase, pSourceSpec.pSSourceId, lSIndexedFile);
			}

			ExecSql(lPtrDatabase, "COMMIT;", nullptr);

			liVectorDatabaseSize = CountVectorRows(lPtrDatabase, pSourceSpec.pSSourceId);
			UpdateRunningState(pContext, liFilesProcessed, liTotalFiles, liVectorDatabaseSize);

			if (liIndexedChunks == 0 && liVectorDatabaseSize == 0)
			{
				MarkScanStopped(pContext, "No chunks were indexed. Ensure a local GGUF embedding model is available.", false);
				return;
			}

			MarkScanStopped(pContext, "", true);
		}
	} // namespace

	
#include "vectorised_rag_lifecycle.inl"
#include "vectorised_rag_retrieval.inl"


	ScanStateSnapshot Fetch_state(Context& pContext)
	{
		if (pContext.pScanThread.joinable())
		{
			bool lbShouldJoin = false;
			{
				std::lock_guard<std::mutex> lGuard(pContext.pMutex);
				lbShouldJoin = !pContext.pbThreadRunning;
			}

			if (lbShouldJoin)
			{
				pContext.pScanThread.join();
			}
		}

		std::lock_guard<std::mutex> lGuard(pContext.pMutex);

		if (pContext.pbThreadRunning)
		{
			ScanStateSnapshot lStateSnapshot = pContext.pScanState;
			lStateSnapshot.pState = StateValue::Running;
			return lStateSnapshot;
		}

		if (pContext.pbFinishedPending)
		{
			pContext.pbFinishedPending = false;
			ScanStateSnapshot lStateSnapshot = pContext.pScanState;
			lStateSnapshot.pState = StateValue::Finished;
			return lStateSnapshot;
		}

		ScanStateSnapshot lStateSnapshot;
		lStateSnapshot.pState = StateValue::Stopped;
		return lStateSnapshot;
	}

} // namespace ollama_engine::internal::vectorised_rag

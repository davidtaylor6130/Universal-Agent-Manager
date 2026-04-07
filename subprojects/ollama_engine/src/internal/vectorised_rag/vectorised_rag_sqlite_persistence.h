#pragma once

#include "vectorised_rag_chunk_extraction.h"
#include "vectorised_rag_source_resolution.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#include <sqlite3.h>

namespace ollama_engine::internal::vectorised_rag
{
	namespace fs = std::filesystem;

	inline bool ExecSql(sqlite3* pPtrDatabase, const char* pSSql, std::string* pSErrorOut)
	{
		char* lPtrSError = nullptr;
		const int liStatus = sqlite3_exec(pPtrDatabase, pSSql, nullptr, nullptr, &lPtrSError);

		if (liStatus == SQLITE_OK)
		{
			return true;
		}

		if (pSErrorOut != nullptr)
		{
			*pSErrorOut = (lPtrSError != nullptr) ? lPtrSError : "sqlite error";
		}

		sqlite3_free(lPtrSError);
		return false;
	}

	inline fs::path BuildStorageDirectory(const RuntimeOptions& pRuntimeOptions)
	{
		const std::string lSFolderName = pRuntimeOptions.pSStorageFolderName.empty() ? ".vectorised_rag" : pRuntimeOptions.pSStorageFolderName;
		const fs::path lPathDatabaseDirectory = fs::current_path() / lSFolderName;
		std::error_code lErrorCode;
		fs::create_directories(lPathDatabaseDirectory, lErrorCode);
		return lPathDatabaseDirectory;
	}

	inline fs::path BuildNamedDatabasePath(const RuntimeOptions& pRuntimeOptions, const std::string& pSDatabaseName)
	{
		const std::string lSDatabaseName = SanitizeDatabaseName(pSDatabaseName);

		if (lSDatabaseName.empty())
		{
			return {};
		}

		return BuildStorageDirectory(pRuntimeOptions) / (lSDatabaseName + ".sqlite3");
	}

	inline fs::path BuildDatabasePath(const std::string& pSSourceId, const RuntimeOptions& pRuntimeOptions)
	{
		const fs::path lPathNamedDatabase = BuildNamedDatabasePath(pRuntimeOptions, pRuntimeOptions.pSDatabaseName);

		if (!lPathNamedDatabase.empty())
		{
			return lPathNamedDatabase;
		}

		const fs::path lPathDatabaseDirectory = BuildStorageDirectory(pRuntimeOptions);
		return lPathDatabaseDirectory / (determanistic_hash::HashTextHex(pSSourceId) + ".sqlite3");
	}

	inline std::vector<fs::path> CollectSqliteFilesInDirectory(const fs::path& pPathDirectory)
	{
		std::vector<fs::path> lVecPathDatabases;
		std::error_code lErrorCode;

		if (!fs::exists(pPathDirectory, lErrorCode) || !fs::is_directory(pPathDirectory, lErrorCode))
		{
			return lVecPathDatabases;
		}

		for (const fs::directory_entry& lEntry : fs::directory_iterator(pPathDirectory, lErrorCode))
		{
			if (lErrorCode)
			{
				lErrorCode.clear();
				continue;
			}

			if (!lEntry.is_regular_file(lErrorCode) || lErrorCode)
			{
				lErrorCode.clear();
				continue;
			}

			if (ToLowerAscii(lEntry.path().extension().string()) != ".sqlite3")
			{
				continue;
			}

			lVecPathDatabases.push_back(lEntry.path());
		}

		std::sort(lVecPathDatabases.begin(), lVecPathDatabases.end());
		return lVecPathDatabases;
	}

	inline bool ResolveDatabaseInput(const std::string& pSDatabaseInput, const RuntimeOptions& pRuntimeOptions, std::vector<fs::path>* pPtrVecPathDatabasesOut, std::string* pSErrorOut)
	{
		if (pPtrVecPathDatabasesOut == nullptr)
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Database path output was null.";
			}

			return false;
		}

		const std::string lSInput = TrimAscii(pSDatabaseInput);

		if (lSInput.empty())
		{
			return true;
		}

		std::error_code lErrorCode;
		const fs::path lPathInput = fs::path(lSInput);

		if (fs::exists(lPathInput, lErrorCode))
		{
			if (fs::is_directory(lPathInput, lErrorCode))
			{
				const std::vector<fs::path> lVecPathDirectoryDatabases = CollectSqliteFilesInDirectory(lPathInput);

				if (lVecPathDirectoryDatabases.empty())
				{
					if (pSErrorOut != nullptr)
					{
						*pSErrorOut = "No .sqlite3 databases found in directory: " + lPathInput.string();
					}

					return false;
				}

				pPtrVecPathDatabasesOut->insert(pPtrVecPathDatabasesOut->end(), lVecPathDirectoryDatabases.begin(), lVecPathDirectoryDatabases.end());
				return true;
			}

			if (fs::is_regular_file(lPathInput, lErrorCode))
			{
				if (ToLowerAscii(lPathInput.extension().string()) != ".sqlite3")
				{
					if (pSErrorOut != nullptr)
					{
						*pSErrorOut = "Expected .sqlite3 database file: " + lPathInput.string();
					}

					return false;
				}

				pPtrVecPathDatabasesOut->push_back(lPathInput);
				return true;
			}

			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Database input is not a regular file or directory: " + lPathInput.string();
			}

			return false;
		}

		if (HasPathSeparator(lSInput))
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Database path was not found: " + lSInput;
			}

			return false;
		}

		const fs::path lPathNamedDatabase = BuildNamedDatabasePath(pRuntimeOptions, lSInput);

		if (lPathNamedDatabase.empty())
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Invalid logical database name: " + lSInput;
			}

			return false;
		}

		if (!fs::exists(lPathNamedDatabase, lErrorCode) || !fs::is_regular_file(lPathNamedDatabase, lErrorCode))
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Named database was not found: " + lPathNamedDatabase.string();
			}

			return false;
		}

		pPtrVecPathDatabasesOut->push_back(lPathNamedDatabase);
		return true;
	}

	inline bool HasChunksTable(sqlite3* pPtrDatabase)
	{
		static constexpr const char* kSql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name='chunks' LIMIT 1;";
		sqlite3_stmt* lPtrStatement = nullptr;

		if (sqlite3_prepare_v2(pPtrDatabase, kSql, -1, &lPtrStatement, nullptr) != SQLITE_OK)
		{
			return false;
		}

		const bool lbHasTable = (sqlite3_step(lPtrStatement) == SQLITE_ROW);
		sqlite3_finalize(lPtrStatement);
		return lbHasTable;
	}

	inline bool OpenDatabaseForSource(const std::string& pSSourceId, const RuntimeOptions& pRuntimeOptions, sqlite3** pPtrDatabaseOut, fs::path* pPathDatabaseOut)
	{
		if (pPtrDatabaseOut == nullptr || pPathDatabaseOut == nullptr)
		{
			return false;
		}

		*pPtrDatabaseOut = nullptr;
		*pPathDatabaseOut = BuildDatabasePath(pSSourceId, pRuntimeOptions);

		if (sqlite3_open((*pPathDatabaseOut).string().c_str(), pPtrDatabaseOut) != SQLITE_OK)
		{
			if (*pPtrDatabaseOut != nullptr)
			{
				sqlite3_close(*pPtrDatabaseOut);
				*pPtrDatabaseOut = nullptr;
			}

			return false;
		}

		sqlite3_busy_timeout(*pPtrDatabaseOut, 5000);
		return true;
	}

	inline bool EnsureSchema(sqlite3* pPtrDatabase, std::string* pSErrorOut)
	{
		static constexpr const char* kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS files (
  source_id TEXT NOT NULL,
  file_path TEXT NOT NULL,
  content_hash TEXT NOT NULL,
  mtime_ticks INTEGER NOT NULL,
  file_size INTEGER NOT NULL,
  PRIMARY KEY (source_id, file_path)
);

CREATE TABLE IF NOT EXISTS chunks (
  id TEXT PRIMARY KEY,
  source_id TEXT NOT NULL,
  file_path TEXT NOT NULL,
  chunk_type TEXT NOT NULL,
  symbol_name TEXT,
  parent_symbol TEXT,
  start_line INTEGER NOT NULL,
  end_line INTEGER NOT NULL,
  hash TEXT NOT NULL,
  raw_text TEXT NOT NULL,
  embedding BLOB NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_chunks_source ON chunks(source_id);
CREATE INDEX IF NOT EXISTS idx_chunks_source_file ON chunks(source_id, file_path);
)SQL";
		return ExecSql(pPtrDatabase, kSchemaSql, pSErrorOut);
	}

	inline std::string SerializeEmbedding(const std::vector<float>& pVecfEmbedding)
	{
		if (pVecfEmbedding.empty())
		{
			return {};
		}

		const char* lPtrBytes = reinterpret_cast<const char*>(pVecfEmbedding.data());
		return std::string(lPtrBytes, lPtrBytes + (pVecfEmbedding.size() * sizeof(float)));
	}

	inline std::vector<float> DeserializeEmbedding(const void* pPtrBlobData, const int piBlobBytes)
	{
		if (pPtrBlobData == nullptr || piBlobBytes <= 0 || (piBlobBytes % static_cast<int>(sizeof(float))) != 0)
		{
			return {};
		}

		const std::size_t liCount = static_cast<std::size_t>(piBlobBytes / static_cast<int>(sizeof(float)));
		std::vector<float> lVecfEmbedding(liCount, 0.0f);
		std::memcpy(lVecfEmbedding.data(), pPtrBlobData, liCount * sizeof(float));
		return lVecfEmbedding;
	}

	inline bool SelectFileHash(sqlite3* pPtrDatabase, const std::string& pSSourceId, const std::string& pSFilePath, std::string* pPtrHashOut)
	{
		static constexpr const char* kSql = "SELECT content_hash FROM files WHERE source_id = ?1 AND file_path = ?2;";
		sqlite3_stmt* lPtrStatement = nullptr;

		if (sqlite3_prepare_v2(pPtrDatabase, kSql, -1, &lPtrStatement, nullptr) != SQLITE_OK)
		{
			return false;
		}

		sqlite3_bind_text(lPtrStatement, 1, pSSourceId.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(lPtrStatement, 2, pSFilePath.c_str(), -1, SQLITE_TRANSIENT);
		bool lbFound = false;

		if (sqlite3_step(lPtrStatement) == SQLITE_ROW)
		{
			const unsigned char* lPtrHash = sqlite3_column_text(lPtrStatement, 0);

			if (lPtrHash != nullptr && pPtrHashOut != nullptr)
			{
				*pPtrHashOut = reinterpret_cast<const char*>(lPtrHash);
				lbFound = true;
			}
		}

		sqlite3_finalize(lPtrStatement);
		return lbFound;
	}

	inline bool UpsertFileRow(sqlite3* pPtrDatabase, const std::string& pSSourceId, const std::string& pSFilePath, const std::string& pSContentHash, const std::uint64_t piMtimeTicks, const std::uintmax_t piFileSize)
	{
		static constexpr const char* kSql = "INSERT INTO files(source_id, file_path, content_hash, mtime_ticks, file_size) "
		                                    "VALUES(?1, ?2, ?3, ?4, ?5) "
		                                    "ON CONFLICT(source_id, file_path) DO UPDATE SET "
		                                    "content_hash = excluded.content_hash, "
		                                    "mtime_ticks = excluded.mtime_ticks, "
		                                    "file_size = excluded.file_size;";
		sqlite3_stmt* lPtrStatement = nullptr;

		if (sqlite3_prepare_v2(pPtrDatabase, kSql, -1, &lPtrStatement, nullptr) != SQLITE_OK)
		{
			return false;
		}

		sqlite3_bind_text(lPtrStatement, 1, pSSourceId.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(lPtrStatement, 2, pSFilePath.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(lPtrStatement, 3, pSContentHash.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(lPtrStatement, 4, static_cast<sqlite3_int64>(piMtimeTicks));
		sqlite3_bind_int64(lPtrStatement, 5, static_cast<sqlite3_int64>(piFileSize));
		const bool lbOk = (sqlite3_step(lPtrStatement) == SQLITE_DONE);
		sqlite3_finalize(lPtrStatement);
		return lbOk;
	}

	inline bool DeleteChunksForFile(sqlite3* pPtrDatabase, const std::string& pSSourceId, const std::string& pSFilePath)
	{
		static constexpr const char* kSql = "DELETE FROM chunks WHERE source_id = ?1 AND file_path = ?2;";
		sqlite3_stmt* lPtrStatement = nullptr;

		if (sqlite3_prepare_v2(pPtrDatabase, kSql, -1, &lPtrStatement, nullptr) != SQLITE_OK)
		{
			return false;
		}

		sqlite3_bind_text(lPtrStatement, 1, pSSourceId.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(lPtrStatement, 2, pSFilePath.c_str(), -1, SQLITE_TRANSIENT);
		const bool lbOk = (sqlite3_step(lPtrStatement) == SQLITE_DONE);
		sqlite3_finalize(lPtrStatement);
		return lbOk;
	}

	inline bool InsertChunk(sqlite3* pPtrDatabase, const ChunkRecord& pChunkRecord)
	{
		static constexpr const char* kSql = "INSERT OR REPLACE INTO chunks("
		                                    "id, source_id, file_path, chunk_type, symbol_name, parent_symbol, "
		                                    "start_line, end_line, hash, raw_text, embedding) "
		                                    "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11);";
		sqlite3_stmt* lPtrStatement = nullptr;

		if (sqlite3_prepare_v2(pPtrDatabase, kSql, -1, &lPtrStatement, nullptr) != SQLITE_OK)
		{
			return false;
		}

		const std::string lSEmbeddingBlob = SerializeEmbedding(pChunkRecord.pVecfEmbedding);
		sqlite3_bind_text(lPtrStatement, 1, pChunkRecord.pSId.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(lPtrStatement, 2, pChunkRecord.pSSourceId.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(lPtrStatement, 3, pChunkRecord.pSFilePath.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(lPtrStatement, 4, pChunkRecord.pSChunkType.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(lPtrStatement, 5, pChunkRecord.pSSymbolName.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(lPtrStatement, 6, pChunkRecord.pSParentSymbol.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(lPtrStatement, 7, pChunkRecord.piStartLine);
		sqlite3_bind_int(lPtrStatement, 8, pChunkRecord.piEndLine);
		sqlite3_bind_text(lPtrStatement, 9, pChunkRecord.pSHash.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(lPtrStatement, 10, pChunkRecord.pSRawText.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_blob(lPtrStatement, 11, lSEmbeddingBlob.data(), static_cast<int>(lSEmbeddingBlob.size()), SQLITE_TRANSIENT);
		const bool lbOk = (sqlite3_step(lPtrStatement) == SQLITE_DONE);
		sqlite3_finalize(lPtrStatement);
		return lbOk;
	}

	inline std::vector<std::string> SelectAllIndexedFiles(sqlite3* pPtrDatabase, const std::string& pSSourceId)
	{
		std::vector<std::string> lVecSFiles;
		static constexpr const char* kSql = "SELECT file_path FROM files WHERE source_id = ?1;";
		sqlite3_stmt* lPtrStatement = nullptr;

		if (sqlite3_prepare_v2(pPtrDatabase, kSql, -1, &lPtrStatement, nullptr) != SQLITE_OK)
		{
			return lVecSFiles;
		}

		sqlite3_bind_text(lPtrStatement, 1, pSSourceId.c_str(), -1, SQLITE_TRANSIENT);

		while (sqlite3_step(lPtrStatement) == SQLITE_ROW)
		{
			const unsigned char* lPtrFilePath = sqlite3_column_text(lPtrStatement, 0);

			if (lPtrFilePath != nullptr)
			{
				lVecSFiles.emplace_back(reinterpret_cast<const char*>(lPtrFilePath));
			}
		}

		sqlite3_finalize(lPtrStatement);
		return lVecSFiles;
	}

	inline void DeleteFileAndChunks(sqlite3* pPtrDatabase, const std::string& pSSourceId, const std::string& pSFilePath)
	{
		DeleteChunksForFile(pPtrDatabase, pSSourceId, pSFilePath);
		static constexpr const char* kSql = "DELETE FROM files WHERE source_id = ?1 AND file_path = ?2;";
		sqlite3_stmt* lPtrStatement = nullptr;

		if (sqlite3_prepare_v2(pPtrDatabase, kSql, -1, &lPtrStatement, nullptr) != SQLITE_OK)
		{
			return;
		}

		sqlite3_bind_text(lPtrStatement, 1, pSSourceId.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(lPtrStatement, 2, pSFilePath.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_step(lPtrStatement);
		sqlite3_finalize(lPtrStatement);
	}

	inline std::size_t CountVectorRows(sqlite3* pPtrDatabase, const std::string& pSSourceId)
	{
		static constexpr const char* kSql = "SELECT COUNT(*) FROM chunks WHERE source_id = ?1;";
		sqlite3_stmt* lPtrStatement = nullptr;

		if (sqlite3_prepare_v2(pPtrDatabase, kSql, -1, &lPtrStatement, nullptr) != SQLITE_OK)
		{
			return 0;
		}

		sqlite3_bind_text(lPtrStatement, 1, pSSourceId.c_str(), -1, SQLITE_TRANSIENT);
		std::size_t liCount = 0;

		if (sqlite3_step(lPtrStatement) == SQLITE_ROW)
		{
			liCount = static_cast<std::size_t>(sqlite3_column_int64(lPtrStatement, 0));
		}

		sqlite3_finalize(lPtrStatement);
		return liCount;
	}

} // namespace ollama_engine::internal::vectorised_rag

#pragma once

/// <summary>Public abstraction for Ollama-compatible engine implementations.</summary>

#include "ollama_engine/engine_structures.h"

#include <string>
#include <vector>

namespace ollama_engine
{

	/// <summary>
	/// Contract implemented by concrete engine backends.
	/// </summary>
	class EngineInterface
	{
	  public:
		/// <summary>Virtual destructor for interface cleanup.</summary>
		virtual ~EngineInterface() = default;

		/// <summary>Enumerates model files available to the engine.</summary>
		/// <returns>Sorted model file names.</returns>
		virtual std::vector<std::string> ListModels() = 0;

		/// <summary>Loads a model by file name.</summary>
		/// <param name="pSModelName">File name in the configured model folder.</param>
		/// <param name="pSErrorOut">Optional output pointer for human-readable errors.</param>
		/// <returns>True if loading completed successfully.</returns>
		virtual bool Load(const std::string& pSModelName, std::string* pSErrorOut = nullptr) = 0;

		/// <summary>Sends a prompt to the currently loaded model.</summary>
		/// <param name="pSPrompt">Prompt text.</param>
		/// <returns>Response payload with status, text, and embedding.</returns>
		virtual SendMessageResponse SendMessage(const std::string& pSPrompt) = 0;

		/// <summary>Applies generation/sampling settings for future prompt calls.</summary>
		/// <param name="pGenerationSettings">Settings to apply.</param>
		/// <param name="pSErrorOut">Optional output pointer for human-readable errors.</param>
		/// <returns>True if settings were accepted and applied.</returns>
		virtual bool SetGenerationSettings(const GenerationSettings& pGenerationSettings, std::string* pSErrorOut = nullptr) = 0;

		/// <summary>Reads the currently active generation settings.</summary>
		/// <returns>Current generation settings.</returns>
		virtual GenerationSettings GetGenerationSettings() const = 0;

		/// <summary>Reads the latest state snapshot for lifecycle and progress polling.</summary>
		/// <returns>Current state response.</returns>
		virtual CurrentStateResponse QueryCurrentState() const = 0;

		/// <summary>
		/// Starts asynchronous scan + vectorisation for a repository source.
		/// </summary>
		/// <param name="pOptSVectorFile">
		/// Optional scan target (local path or open-source remote repository URL/branch).
		/// Nullopt reuses the last known source.
		/// </param>
		/// <param name="pSErrorOut">Optional output pointer for human-readable errors.</param>
		/// <returns>True when the scan worker started.</returns>
		virtual bool Scan(const std::optional<std::string>& pOptSVectorFile, std::string* pSErrorOut = nullptr) = 0;

		/// <summary>
		/// Sets the output database name used by future Scan calls.
		/// </summary>
		/// <param name="pSDatabaseName">
		/// Logical database name without extension. Empty string resets to source-derived naming.
		/// </param>
		/// <param name="pSErrorOut">Optional output pointer for human-readable errors.</param>
		/// <returns>True when the name was accepted.</returns>
		virtual bool SetRagOutputDatabase(const std::string& pSDatabaseName, std::string* pSErrorOut = nullptr) = 0;

		/// <summary>
		/// Loads one or more local RAG databases for retrieval queries.
		/// </summary>
		/// <param name="pVecSDatabaseInputs">Database selectors (file paths, directories, or logical names).</param>
		/// <param name="pSErrorOut">Optional output pointer for human-readable errors.</param>
		/// <returns>True when inputs were resolved and loaded.</returns>
		virtual bool LoadRagDatabases(const std::vector<std::string>& pVecSDatabaseInputs, std::string* pSErrorOut = nullptr) = 0;

		/// <summary>
		/// Retrieves semantically relevant indexed snippets from vector search.
		/// </summary>
		/// <param name="pSPrompt">Prompt/query text.</param>
		/// <param name="piMax">Maximum amount of material to return.</param>
		/// <param name="piMin">Minimum amount of material to return.</param>
		/// <returns>Snippet list.</returns>
		virtual std::vector<std::string> Fetch_Relevant_Info(const std::string& pSPrompt, std::size_t piMax, std::size_t piMin) = 0;

		/// <summary>
		/// Returns current vectorisation state snapshot.
		/// </summary>
		/// <returns>Vectorisation state response.</returns>
		virtual VectorisationStateResponse Fetch_state() = 0;
	};

} // namespace ollama_engine

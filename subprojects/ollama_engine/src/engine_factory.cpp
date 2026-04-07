#include "ollama_engine/engine_factory.h"

#include "internal/local_ollama_engine.h"

namespace ollama_engine
{

	/// <summary>Creates the default local Ollama engine implementation.</summary>
	/// <param name="pEngineOptions">Runtime engine options.</param>
	/// <returns>Owned engine interface instance.</returns>
	std::unique_ptr<EngineInterface> CreateEngine(const EngineOptions& pEngineOptions)
	{
		return std::make_unique<internal::LocalOllamaCppEngine>(pEngineOptions);
	}

} // namespace ollama_engine

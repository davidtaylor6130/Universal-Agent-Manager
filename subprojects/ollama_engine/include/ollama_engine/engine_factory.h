#pragma once

/// <summary>Factory methods for creating Ollama engine implementations.</summary>

#include "ollama_engine/engine_interface.h"

#include <memory>

namespace ollama_engine
{

	/// <summary>Creates the default local engine implementation.</summary>
	/// <param name="pEngineOptions">Runtime configuration options.</param>
	/// <returns>Owned engine instance.</returns>
	std::unique_ptr<EngineInterface> CreateEngine(const EngineOptions& pEngineOptions);

} // namespace ollama_engine

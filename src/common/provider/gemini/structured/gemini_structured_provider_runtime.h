#pragma once

#include "common/provider/gemini/base/gemini_base_provider_runtime.h"

class GeminiStructuredProviderRuntime final : public GeminiBaseProviderRuntime
{
  public:
	GeminiStructuredProviderRuntime();
};

const IProviderRuntime& GetGeminiStructuredProviderRuntime();

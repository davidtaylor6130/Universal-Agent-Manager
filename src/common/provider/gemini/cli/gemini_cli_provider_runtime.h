#pragma once

#include "common/provider/gemini/base/gemini_base_provider_runtime.h"

class GeminiCliProviderRuntime final : public GeminiBaseProviderRuntime
{
  public:
	GeminiCliProviderRuntime();
};

const IProviderRuntime& GetGeminiCliProviderRuntime();

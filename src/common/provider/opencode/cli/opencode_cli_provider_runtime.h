#pragma once

#include "common/provider/opencode/base/opencode_base_provider_runtime.h"

class OpenCodeCliProviderRuntime final : public OpenCodeBaseProviderRuntime
{
  public:
	OpenCodeCliProviderRuntime();
};

const IProviderRuntime& GetOpenCodeCliProviderRuntime();

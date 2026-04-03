#pragma once

#include "common/provider/opencode/base/opencode_base_provider_runtime.h"

class OpenCodeLocalProviderRuntime final : public OpenCodeBaseProviderRuntime
{
  public:
	OpenCodeLocalProviderRuntime();
};

const IProviderRuntime& GetOpenCodeLocalProviderRuntime();

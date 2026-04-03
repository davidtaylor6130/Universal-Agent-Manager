void Shutdown(Context& pContext)
{
	{
		std::lock_guard<std::mutex> lGuard(pContext.pMutex);
		pContext.pbThreadRunning = false;
	}

	if (pContext.pScanThread.joinable())
	{
		pContext.pScanThread.join();
	}

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
	std::lock_guard<std::mutex> lGuard(pContext.pMutex);
	ReleaseEmbeddingRuntimeLocked(pContext);
#endif
}

bool Scan(Context& pContext, const std::optional<std::string>& pOptSVectorFile, const RuntimeOptions& pRuntimeOptions, std::string* pSErrorOut)
{
	if (pContext.pScanThread.joinable())
	{
		bool lbCanJoin = false;
		{
			std::lock_guard<std::mutex> lGuard(pContext.pMutex);
			lbCanJoin = !pContext.pbThreadRunning;
		}

		if (lbCanJoin)
		{
			pContext.pScanThread.join();
		}
	}

	{
		std::lock_guard<std::mutex> lGuard(pContext.pMutex);

		if (pContext.pbThreadRunning)
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Scan is already running.";
			}

			return false;
		}
	}

	SourceSpec lSourceSpec;

	if (!ResolveSourceSpec(pContext, pOptSVectorFile, &lSourceSpec, pSErrorOut))
	{
		return false;
	}

	{
		std::lock_guard<std::mutex> lGuard(pContext.pMutex);
		pContext.pbThreadRunning = true;
		pContext.pbFinishedPending = false;
		pContext.pScanState.pState = StateValue::Running;
		pContext.pScanState.piFilesProcessed = 0;
		pContext.pScanState.piTotalFiles = 0;
		pContext.pScanState.piVectorDatabaseSize = 0;
		pContext.pSError.clear();
	}

	pContext.pScanThread = std::thread([&pContext, lSourceSpec, pRuntimeOptions]() { RunScanWorker(pContext, lSourceSpec, pRuntimeOptions); });
	return true;
}

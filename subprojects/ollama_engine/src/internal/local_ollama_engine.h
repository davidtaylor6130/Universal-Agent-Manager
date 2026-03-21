#pragma once

#include "ollama_engine/engine_interface.h"

#include <mutex>
#include <string>

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
struct llama_model;
struct llama_context;
struct llama_sampler;
#endif

namespace ollama_engine::internal {

/// <summary>
/// Local file-backed engine implementation used by the factory.
/// </summary>
class LocalOllamaCppEngine final : public EngineInterface {
 public:
  /// <summary>Constructs the local engine with runtime options.</summary>
  /// <param name="pEngineOptions">Engine runtime options.</param>
  explicit LocalOllamaCppEngine(EngineOptions pEngineOptions);
  /// <summary>Releases runtime resources for loaded model/context objects.</summary>
  ~LocalOllamaCppEngine() override;

  /// <inheritdoc />
  std::vector<std::string> ListModels() override;
  /// <inheritdoc />
  bool Load(const std::string& pSModelName, std::string* pSErrorOut = nullptr) override;
  /// <inheritdoc />
  SendMessageResponse SendMessage(const std::string& pSPrompt) override;
  /// <inheritdoc />
  bool SetGenerationSettings(const GenerationSettings& pGenerationSettings, std::string* pSErrorOut = nullptr) override;
  /// <inheritdoc />
  GenerationSettings GetGenerationSettings() const override;
  /// <inheritdoc />
  CurrentStateResponse QueryCurrentState() const override;

 private:
  /// <summary>Frees currently loaded llama runtime objects when present.</summary>
  void ReleaseRuntimeLocked();

  EngineOptions mEngineOptions;
  GenerationSettings mGenerationSettings;
  mutable std::mutex mMutexEngineRuntime;
  mutable std::mutex mMutexCurrentState;
  std::string ms_LoadedModelName;
  CurrentStateResponse mCurrentStateResponse;
#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
  std::filesystem::path mPathLoadedModelFile;
  llama_model* mPtrModel = nullptr;
  llama_context* mPtrContext = nullptr;
  llama_sampler* mPtrSampler = nullptr;
#endif
};

}  // namespace ollama_engine::internal

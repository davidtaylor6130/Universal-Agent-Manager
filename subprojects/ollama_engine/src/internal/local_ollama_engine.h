#pragma once

#include "ollama_engine/engine_interface.h"

#include <memory>
#include <optional>
#include <mutex>
#include <string>

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
struct llama_model;
struct llama_context;
struct llama_sampler;
#endif

namespace ollama_engine::internal {

struct RagRuntimeInterface;

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
  /// <inheritdoc />
  bool Scan(const std::optional<std::string>& pOptSVectorFile, std::string* pSErrorOut = nullptr) override;
  /// <inheritdoc />
  bool SetRagOutputDatabase(const std::string& pSDatabaseName, std::string* pSErrorOut = nullptr) override;
  /// <inheritdoc />
  bool LoadRagDatabases(const std::vector<std::string>& pVecSDatabaseInputs,
                        std::string* pSErrorOut = nullptr) override;
  /// <inheritdoc />
  std::vector<std::string> Fetch_Relevant_Info(const std::string& pSPrompt,
                                               std::size_t piMax,
                                               std::size_t piMin) override;
  /// <inheritdoc />
  VectorisationStateResponse Fetch_state() override;

 private:
  /// <summary>Frees currently loaded llama runtime objects when present.</summary>
  void ReleaseRuntimeLocked();

  EngineOptions mEngineOptions;
  GenerationSettings mGenerationSettings;
  mutable std::mutex mMutexEngineRuntime;
  mutable std::mutex mMutexCurrentState;
  std::string ms_LoadedModelName;
  std::string ms_RagOutputDatabaseName;
  CurrentStateResponse mCurrentStateResponse;
  std::unique_ptr<RagRuntimeInterface> mPtrRagRuntime;
#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
  std::filesystem::path mPathLoadedModelFile;
  llama_model* mPtrModel = nullptr;
  llama_context* mPtrContext = nullptr;
  llama_sampler* mPtrSampler = nullptr;
#endif
};

}  // namespace ollama_engine::internal

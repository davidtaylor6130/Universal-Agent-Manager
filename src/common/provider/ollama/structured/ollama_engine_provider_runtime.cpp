#include "common/provider/ollama/structured/ollama_engine_provider_runtime.h"

#include "common/provider/runtime/provider_runtime_internal.h"

using namespace provider_runtime_internal;

const char* OllamaEngineProviderRuntime::RuntimeId() const
{
	return "ollama-engine";
}

bool OllamaEngineProviderRuntime::IsEnabled() const
{
	return UAM_ENABLE_RUNTIME_OLLAMA_ENGINE != 0;
}

const char* OllamaEngineProviderRuntime::DisabledReason() const
{
	return "Runtime 'ollama-engine' is disabled in this build (UAM_ENABLE_RUNTIME_OLLAMA_ENGINE=OFF).";
}

std::string OllamaEngineProviderRuntime::BuildPrompt(const ProviderProfile&, const std::string& user_prompt, const std::vector<std::string>& files) const
{
	return provider_runtime_internal::BuildPrompt(user_prompt, files);
}

std::string OllamaEngineProviderRuntime::BuildCommand(const ProviderProfile&, const AppSettings&, const std::string&, const std::vector<std::string>&, const std::string&) const
{
	return "";
}

std::vector<std::string> OllamaEngineProviderRuntime::BuildInteractiveArgv(const ProviderProfile&, const ChatSession&, const AppSettings&) const
{
	return {};
}

MessageRole OllamaEngineProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type) const
{
	return provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> OllamaEngineProviderRuntime::LoadHistory(const ProviderProfile&, const std::filesystem::path& data_root, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const
{
	return LoadLocalChats(data_root);
}

bool OllamaEngineProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return SaveLocalChat(data_root, chat);
}

bool OllamaEngineProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile&) const
{
	return false;
}

bool OllamaEngineProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile&) const
{
	return false;
}

bool OllamaEngineProviderRuntime::UsesLocalHistory(const ProviderProfile&) const
{
	return true;
}

bool OllamaEngineProviderRuntime::UsesInternalEngine(const ProviderProfile&) const
{
	return true;
}

bool OllamaEngineProviderRuntime::UsesCliOutput(const ProviderProfile&) const
{
	return false;
}

bool OllamaEngineProviderRuntime::UsesStructuredOutput(const ProviderProfile&) const
{
	return true;
}

bool OllamaEngineProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile&) const
{
	return false;
}

const IProviderRuntime& GetOllamaEngineProviderRuntime()
{
	static const OllamaEngineProviderRuntime runtime;
	return runtime;
}

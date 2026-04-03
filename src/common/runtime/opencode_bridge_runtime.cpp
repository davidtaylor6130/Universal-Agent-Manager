#include "opencode_bridge_runtime.h"

#include "common/platform/platform_services.h"
#include "common/platform/sdl_includes.h"
#include "common/runtime/json_runtime.h"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <thread>

namespace
{
	namespace fs = std::filesystem;
	using uam::AppState;
	using uam::OpenCodeBridgeState;

	std::string Trim(const std::string& value)
	{
		const auto start = value.find_first_not_of(" \t\r\n");

		if (start == std::string::npos)
		{
			return "";
		}

		const auto end = value.find_last_not_of(" \t\r\n");
		return value.substr(start, end - start + 1);
	}

	std::string CompactPreview(const std::string& text, const std::size_t max_len)
	{
		std::string compact;
		compact.reserve(text.size());

		for (const char ch : text)
		{
			compact.push_back((ch == '\n' || ch == '\r') ? ' ' : ch);
		}

		const std::string trimmed = Trim(compact);

		if (trimmed.size() <= max_len)
		{
			return trimmed;
		}

		if (max_len <= 3)
		{
			return trimmed.substr(0, max_len);
		}

		return trimmed.substr(0, max_len - 3) + "...";
	}

	std::string ReadTextFile(const fs::path& path)
	{
		std::ifstream in(path, std::ios::in | std::ios::binary);

		if (!in)
		{
			return "";
		}

		std::ostringstream out;
		out << in.rdbuf();
		return out.str();
	}

	bool WriteTextFile(const fs::path& path, const std::string& content)
	{
		std::error_code ec;
		fs::create_directories(path.parent_path(), ec);

		std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);

		if (!out)
		{
			return false;
		}

		out.write(content.data(), static_cast<std::streamsize>(content.size()));
		out.flush();
		return out.good();
	}

	fs::path NormalizeAbsolutePath(const fs::path& path)
	{
		if (path.empty())
		{
			return {};
		}

		std::error_code ec;
		const fs::path absolute_path = fs::absolute(path, ec);

		if (ec)
		{
			return path;
		}

		const fs::path canonical_path = fs::weakly_canonical(absolute_path, ec);
		return ec ? absolute_path : canonical_path;
	}

	std::string OpenCodeBridgeRandomHex(const std::size_t length)
	{
		static thread_local std::mt19937 rng(std::random_device{}());
		std::uniform_int_distribution<int> nibble(0, 15);
		std::string value;
		value.reserve(length);

		for (std::size_t i = 0; i < length; ++i)
		{
			const int n = nibble(rng);
			value.push_back(static_cast<char>((n < 10) ? ('0' + n) : ('a' + (n - 10))));
		}

		return value;
	}

	std::string OpenCodeBridgeTimestampStamp()
	{
		const auto now = std::chrono::system_clock::now();
		const std::time_t tt = std::chrono::system_clock::to_time_t(now);
		std::tm snapshot{};

		if (!PlatformServicesFactory::Instance().process_service.PopulateLocalTime(tt, &snapshot))
		{
			return "";
		}

		std::ostringstream out;
		out << std::put_time(&snapshot, "%Y%m%d-%H%M%S");
		return out.str();
	}

	fs::path ResolveCurrentExecutablePathForBridge()
	{
		return PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath();
	}

	fs::path ResolveOpenCodeBridgeExecutablePath()
	{
		const std::string bridge_binary_name = PlatformServicesFactory::Instance().process_service.OpenCodeBridgeBinaryName();
		std::vector<fs::path> candidates;

		if (const fs::path executable = ResolveCurrentExecutablePathForBridge(); !executable.empty())
		{
			candidates.push_back(executable.parent_path() / bridge_binary_name);
		}

		std::unique_ptr<char, decltype(&SDL_free)> base_path(SDL_GetBasePath(), SDL_free);

		if (base_path != nullptr)
		{
			candidates.push_back(fs::path(base_path.get()) / bridge_binary_name);
		}

		std::error_code cwd_ec;
		const fs::path cwd = fs::current_path(cwd_ec);

		if (!cwd_ec)
		{
			candidates.push_back(cwd / bridge_binary_name);
			candidates.push_back(cwd / "Builds" / bridge_binary_name);
			candidates.push_back(cwd / "Builds" / "ollama_engine" / bridge_binary_name);
			candidates.push_back(cwd / "Builds" / "Release" / bridge_binary_name);
			candidates.push_back(cwd / "Builds" / "Release" / "ollama_engine" / bridge_binary_name);
			candidates.push_back(cwd / "build" / bridge_binary_name);
			candidates.push_back(cwd / "build" / "ollama_engine" / bridge_binary_name);
			candidates.push_back(cwd / "build-release" / bridge_binary_name);
			candidates.push_back(cwd / "build-release" / "ollama_engine" / bridge_binary_name);
		}

		for (const fs::path& candidate : candidates)
		{
			std::error_code exists_ec;

			if (!candidate.empty() && fs::exists(candidate, exists_ec) && !exists_ec)
			{
				return candidate;
			}
		}

		return fs::path(bridge_binary_name);
	}

	fs::path ResolveOpenCodeConfigPath()
	{
		return PlatformServicesFactory::Instance().path_service.ResolveOpenCodeConfigPath();
	}

	fs::path BuildOpenCodeBridgeReadyFilePath(const AppState& app)
	{
		const fs::path runtime_dir = app.data_root / "runtime";
		std::error_code ec;
		fs::create_directories(runtime_dir, ec);
		return runtime_dir / ("opencode_bridge_ready_" + OpenCodeBridgeTimestampStamp() + "_" + OpenCodeBridgeRandomHex(8) + ".json");
	}

	JsonValue JsonObjectValue()
	{
		JsonValue value;
		value.type = JsonValue::Type::Object;
		return value;
	}

	JsonValue JsonStringValue(const std::string& text)
	{
		JsonValue value;
		value.type = JsonValue::Type::String;
		value.string_value = text;
		return value;
	}

	JsonValue* EnsureJsonObjectEntry(JsonValue& root, const std::string& key, bool* changed_out = nullptr)
	{
		if (root.type != JsonValue::Type::Object)
		{
			root = JsonObjectValue();

			if (changed_out != nullptr)
			{
				*changed_out = true;
			}
		}

		auto it = root.object_value.find(key);

		if (it == root.object_value.end() || it->second.type != JsonValue::Type::Object)
		{
			root.object_value[key] = JsonObjectValue();

			if (changed_out != nullptr)
			{
				*changed_out = true;
			}
		}

		return &root.object_value[key];
	}

	bool SetJsonStringEntry(JsonValue& root, const std::string& key, const std::string& value)
	{
		auto it = root.object_value.find(key);

		if (it != root.object_value.end() && it->second.type == JsonValue::Type::String && it->second.string_value == value)
		{
			return false;
		}

		root.object_value[key] = JsonStringValue(value);
		return true;
	}

	bool RemoveJsonEntry(JsonValue& root, const std::string& key)
	{
		return root.object_value.erase(key) > 0;
	}

	std::string JsonErrorStringMessage(const JsonValue* root_error)
	{
		if (root_error == nullptr || root_error->type != JsonValue::Type::Object)
		{
			return "";
		}

		return JsonStringOrEmpty(root_error->Find("error"));
	}

	struct OpenCodeBridgeReadyInfo
	{
		std::string endpoint;
		std::string api_base;
		std::string model;
		std::string error;
		bool ok = false;
	};

	std::optional<OpenCodeBridgeReadyInfo> ParseOpenCodeBridgeReadyInfo(const std::string& file_text)
	{
		const std::optional<JsonValue> root_opt = ParseJson(file_text);

		if (!root_opt.has_value() || root_opt->type != JsonValue::Type::Object)
		{
			return std::nullopt;
		}

		OpenCodeBridgeReadyInfo info;
		const JsonValue& root = root_opt.value();

		if (const JsonValue* ok_value = root.Find("ok"); ok_value != nullptr && ok_value->type == JsonValue::Type::Bool)
		{
			info.ok = ok_value->bool_value;
		}

		info.endpoint = Trim(JsonStringOrEmpty(root.Find("endpoint")));
		info.api_base = Trim(JsonStringOrEmpty(root.Find("api_base")));
		info.model = Trim(JsonStringOrEmpty(root.Find("model")));
		info.error = Trim(JsonStringOrEmpty(root.Find("error")));

		if (!info.ok && info.error.empty())
		{
			info.error = JsonErrorStringMessage(&root);
		}

		return info;
	}

	size_t CurlAppendToStringCallback(void* ptr, const size_t size, const size_t nmemb, void* userdata)
	{
		if (ptr == nullptr || userdata == nullptr || size == 0 || nmemb == 0)
		{
			return 0;
		}

		const size_t total = size * nmemb;
		auto* output = static_cast<std::string*>(userdata);
		output->append(static_cast<const char*>(ptr), total);
		return total;
	}

	bool CurlHttpGet(const std::string& url, const std::string& bearer_token, long* status_code_out, std::string* body_out, std::string* error_out)
	{
		if (status_code_out != nullptr)
		{
			*status_code_out = 0;
		}

		if (body_out != nullptr)
		{
			body_out->clear();
		}

		std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);

		if (curl == nullptr)
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to initialize libcurl.";
			}

			return false;
		}

		std::string body;
		curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
		curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 1500L);
		curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 1000L);
		curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, CurlAppendToStringCallback);
		curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);

		std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr, curl_slist_free_all);

		if (!bearer_token.empty())
		{
			const std::string auth = "Authorization: Bearer " + bearer_token;
			curl_slist* header_list = curl_slist_append(nullptr, auth.c_str());

			if (header_list == nullptr)
			{
				if (error_out != nullptr)
				{
					*error_out = "Failed to build HTTP authorization header.";
				}

				return false;
			}

			headers.reset(header_list);
			curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
		}

		const CURLcode code = curl_easy_perform(curl.get());
		long status_code = 0;
		curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status_code);

		if (body_out != nullptr)
		{
			*body_out = body;
		}

		if (status_code_out != nullptr)
		{
			*status_code_out = status_code;
		}

		if (code != CURLE_OK)
		{
			if (error_out != nullptr)
			{
				*error_out = std::string("HTTP probe failed: ") + curl_easy_strerror(code);
			}

			return false;
		}

		return true;
	}

	bool ProbeOpenCodeBridgeHealth(const AppState& app, std::string* error_out = nullptr)
	{
		const std::string endpoint = Trim(app.opencode_bridge.endpoint);

		if (endpoint.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = "OpenCode bridge endpoint is empty.";
			}

			return false;
		}

		long status_code = 0;
		std::string body;
		std::string curl_error;

		if (!CurlHttpGet(endpoint + "/healthz", "", &status_code, &body, &curl_error))
		{
			if (error_out != nullptr)
			{
				*error_out = curl_error;
			}

			return false;
		}

		if (status_code != 200)
		{
			if (error_out != nullptr)
			{
				std::ostringstream out;
				out << "OpenCode bridge health check failed (status " << status_code << ").";
				const std::string trimmed_body = Trim(body);

				if (!trimmed_body.empty())
				{
					out << " Body: " << CompactPreview(trimmed_body, 180);
				}

				*error_out = out.str();
			}

			return false;
		}

		return true;
	}

	bool StartOpenCodeBridgeProcess(AppState& app, const std::vector<std::string>& argv, std::string* error_out = nullptr)
	{
		const bool started = PlatformServicesFactory::Instance().process_service.StartOpenCodeBridgeProcess(argv, app.opencode_bridge, error_out);
		app.opencode_bridge.running = started;
		return started;
	}

	bool IsOpenCodeBridgeProcessRunning(AppState& app)
	{
		const bool running = PlatformServicesFactory::Instance().process_service.IsOpenCodeBridgeProcessRunning(app.opencode_bridge);
		app.opencode_bridge.running = running;
		return running;
	}

	void ResetOpenCodeBridgeRuntimeFields(AppState& app, const bool keep_token = true)
	{
		const std::string preserved_token = keep_token ? app.opencode_bridge.token : "";
		app.opencode_bridge.running = false;
		app.opencode_bridge.healthy = false;
		app.opencode_bridge.endpoint.clear();
		app.opencode_bridge.api_base.clear();
		app.opencode_bridge.selected_model.clear();
		app.opencode_bridge.requested_model.clear();
		app.opencode_bridge.model_folder.clear();
		app.opencode_bridge.ready_file.clear();
		app.opencode_bridge.last_error.clear();

		if (keep_token)
		{
			app.opencode_bridge.token = preserved_token;
		}
		else
		{
			app.opencode_bridge.token.clear();
		}
	}

	bool WaitForOpenCodeBridgeReadyFile(AppState& app, const fs::path& ready_file, OpenCodeBridgeReadyInfo* info_out, std::string* error_out = nullptr)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);

		while (std::chrono::steady_clock::now() < deadline)
		{
			std::error_code ec;

			if (fs::exists(ready_file, ec) && !ec)
			{
				const std::string text = Trim(ReadTextFile(ready_file));

				if (!text.empty())
				{
					const std::optional<OpenCodeBridgeReadyInfo> info = ParseOpenCodeBridgeReadyInfo(text);

					if (info.has_value())
					{
						if (!info->ok && !info->error.empty())
						{
							if (error_out != nullptr)
							{
								*error_out = info->error;
							}

							return false;
						}

						if (!info->endpoint.empty())
						{
							if (info_out != nullptr)
							{
								*info_out = info.value();
							}

							return true;
						}
					}
				}
			}

			if (!IsOpenCodeBridgeProcessRunning(app))
			{
				if (error_out != nullptr)
				{
					*error_out = "OpenCode bridge process exited before readiness handshake.";
				}

				return false;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(40));
		}

		if (error_out != nullptr)
		{
			*error_out = "Timed out waiting for OpenCode bridge ready file.";
		}

		return false;
	}

	bool EnsureOpenCodeConfigProvisioned(AppState& app, std::string* error_out = nullptr)
	{
		const std::string api_base = Trim(app.opencode_bridge.api_base);
		std::string model_id = Trim(app.opencode_bridge.selected_model);

		if (model_id.empty())
		{
			model_id = Trim(app.settings.selected_model_id);
		}

		if (api_base.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = "OpenCode bridge API base URL is empty.";
			}

			return false;
		}

		if (model_id.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = "OpenCode bridge has no selected model.";
			}

			return false;
		}

		const fs::path config_path = ResolveOpenCodeConfigPath();
		std::error_code ec;
		fs::create_directories(config_path.parent_path(), ec);

		if (ec)
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to create OpenCode config directory: " + ec.message();
			}

			return false;
		}

		JsonValue root = JsonObjectValue();
		bool changed = false;
		bool parse_failed = false;
		const std::string existing_text = ReadTextFile(config_path);

		if (!Trim(existing_text).empty())
		{
			const std::optional<JsonValue> parsed = ParseJson(existing_text);

			if (!parsed.has_value() || parsed->type != JsonValue::Type::Object)
			{
				parse_failed = true;
				const fs::path backup_path = config_path.parent_path() / (config_path.stem().string() + ".backup-" + OpenCodeBridgeTimestampStamp() + config_path.extension().string());
				std::error_code copy_ec;
				fs::copy_file(config_path, backup_path, fs::copy_options::overwrite_existing, copy_ec);

				if (copy_ec)
				{
					app.status_line = "OpenCode config parse failed; backup copy also failed: " + copy_ec.message();
				}
				else
				{
					app.status_line = "OpenCode config parse failed; created backup at " + backup_path.string() + ".";
				}

				changed = true;
			}
			else
			{
				root = parsed.value();
			}
		}

		JsonValue* provider = EnsureJsonObjectEntry(root, "provider", &changed);
		JsonValue* uam_local = EnsureJsonObjectEntry(*provider, "uam_local", &changed);
		changed = SetJsonStringEntry(*uam_local, "npm", "@ai-sdk/openai-compatible") || changed;
		changed = SetJsonStringEntry(*uam_local, "name", "UAM Local (Ollama Engine)") || changed;
		changed = SetJsonStringEntry(*uam_local, "api", api_base) || changed;
		JsonValue* options = EnsureJsonObjectEntry(*uam_local, "options", &changed);
		changed = SetJsonStringEntry(*options, "baseURL", api_base) || changed;

		if (!app.opencode_bridge.token.empty())
		{
			changed = SetJsonStringEntry(*options, "apiKey", app.opencode_bridge.token) || changed;
		}
		else
		{
			changed = RemoveJsonEntry(*options, "apiKey") || changed;
		}

		JsonValue* models = EnsureJsonObjectEntry(*uam_local, "models", &changed);
		JsonValue* model_entry = EnsureJsonObjectEntry(*models, model_id, &changed);
		fs::path model_path(model_id);
		std::string model_display_name = model_path.filename().string();

		if (model_display_name.empty())
		{
			model_display_name = model_id;
		}

		changed = SetJsonStringEntry(*model_entry, "name", model_display_name) || changed;
		changed = SetJsonStringEntry(root, "model", "uam_local/" + model_id) || changed;

		if (!changed && !parse_failed)
		{
			return true;
		}

		if (!WriteTextFile(config_path, SerializeJson(root)))
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to write OpenCode config file: " + config_path.string();
			}

			return false;
		}

		return true;
	}

	std::vector<std::string> BuildOpenCodeBridgeArgv(const fs::path& bridge_executable, const fs::path& model_folder, const std::string& requested_model, const std::string& token, const fs::path& ready_file)
	{
		std::vector<std::string> argv;
		argv.push_back(bridge_executable.string());
		argv.push_back("--host");
		argv.push_back("127.0.0.1");
		argv.push_back("--port");
		argv.push_back("0");
		argv.push_back("--model-folder");
		argv.push_back(model_folder.string());

		if (!Trim(requested_model).empty())
		{
			argv.push_back("--default-model");
			argv.push_back(Trim(requested_model));
		}

		if (!Trim(token).empty())
		{
			argv.push_back("--token");
			argv.push_back(token);
		}

		argv.push_back("--ready-file");
		argv.push_back(ready_file.string());
		return argv;
	}

	void StopOpenCodeBridge(AppState& app, const bool keep_token)
	{
		LocalBridgeRuntimeService bridge_service;
		bridge_service.Stop(app, keep_token);
	}

	bool StartOpenCodeBridge(AppState& app, const fs::path& model_folder, const std::string& requested_model, std::string* error_out = nullptr)
	{
		const fs::path normalized_model_folder = NormalizeAbsolutePath(model_folder).empty() ? model_folder : NormalizeAbsolutePath(model_folder);
		const fs::path bridge_executable = ResolveOpenCodeBridgeExecutablePath();

		if (bridge_executable.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = "Could not resolve uam_ollama_engine_bridge executable.";
			}

			return false;
		}

		if (app.opencode_bridge.token.empty())
		{
			app.opencode_bridge.token = OpenCodeBridgeRandomHex(48);
		}

		const fs::path ready_file = BuildOpenCodeBridgeReadyFilePath(app);
		std::error_code rm_ec;
		fs::remove(ready_file, rm_ec);

		const std::vector<std::string> argv = BuildOpenCodeBridgeArgv(bridge_executable, normalized_model_folder, requested_model, app.opencode_bridge.token, ready_file);

		if (!StartOpenCodeBridgeProcess(app, argv, error_out))
		{
			app.opencode_bridge.last_error = (error_out != nullptr) ? *error_out : "OpenCode bridge process launch failed.";
			return false;
		}

		OpenCodeBridgeReadyInfo ready_info;
		std::string ready_error;

		if (!WaitForOpenCodeBridgeReadyFile(app, ready_file, &ready_info, &ready_error))
		{
			StopOpenCodeBridge(app, true);

			if (error_out != nullptr)
			{
				*error_out = ready_error.empty() ? "OpenCode bridge did not become ready." : ready_error;
			}

			app.opencode_bridge.last_error = (error_out != nullptr) ? *error_out : "OpenCode bridge startup failed.";
			return false;
		}

		app.opencode_bridge.endpoint = ready_info.endpoint;
		app.opencode_bridge.api_base = ready_info.api_base.empty() ? (ready_info.endpoint + "/v1") : ready_info.api_base;
		app.opencode_bridge.selected_model = ready_info.model.empty() ? Trim(requested_model) : ready_info.model;
		app.opencode_bridge.requested_model = Trim(requested_model);
		app.opencode_bridge.model_folder = normalized_model_folder.string();
		app.opencode_bridge.ready_file = ready_file.string();
		app.opencode_bridge.running = true;
		app.opencode_bridge.healthy = false;

		std::string health_error;

		if (!ProbeOpenCodeBridgeHealth(app, &health_error))
		{
			StopOpenCodeBridge(app, true);

			if (error_out != nullptr)
			{
				*error_out = health_error.empty() ? "OpenCode bridge health check failed." : health_error;
			}

			app.opencode_bridge.last_error = (error_out != nullptr) ? *error_out : "OpenCode bridge health check failed.";
			return false;
		}

		app.opencode_bridge.healthy = true;
		std::string config_error;

		if (!EnsureOpenCodeConfigProvisioned(app, &config_error))
		{
			StopOpenCodeBridge(app, true);

			if (error_out != nullptr)
			{
				*error_out = config_error.empty() ? "OpenCode config provisioning failed." : config_error;
			}

			app.opencode_bridge.last_error = (error_out != nullptr) ? *error_out : "OpenCode config provisioning failed.";
			return false;
		}

		app.opencode_bridge.last_error.clear();
		return true;
	}

	bool RestartLocalBridgeIfModelChanged(AppState& app, const fs::path& desired_model_folder, const std::string& desired_requested_model, std::string* error_out)
	{
		const bool process_running = IsOpenCodeBridgeProcessRunning(app);
		const bool signature_matches = process_running && app.opencode_bridge.model_folder == desired_model_folder.string() && app.opencode_bridge.requested_model == desired_requested_model && !Trim(app.opencode_bridge.endpoint).empty() && !Trim(app.opencode_bridge.api_base).empty();

		if (signature_matches)
		{
			std::string health_error;

			if (!ProbeOpenCodeBridgeHealth(app, &health_error))
			{
				StopOpenCodeBridge(app, true);

				if (!StartOpenCodeBridge(app, desired_model_folder, desired_requested_model, error_out))
				{
					return false;
				}

				return true;
			}

			app.opencode_bridge.healthy = true;
			std::string config_error;

			if (!EnsureOpenCodeConfigProvisioned(app, &config_error))
			{
				if (error_out != nullptr)
				{
					*error_out = config_error;
				}

				app.opencode_bridge.last_error = config_error;
				return false;
			}

			return true;
		}

		StopOpenCodeBridge(app, true);
		return StartOpenCodeBridge(app, desired_model_folder, desired_requested_model, error_out);
	}

} // namespace

bool LocalBridgeRuntimeService::EnsureRunning(AppState& app,
                                              const std::filesystem::path& model_folder,
                                              const std::string& requested_model,
                                              std::string* error_out) const
{
	const fs::path desired_model_folder = NormalizeAbsolutePath(model_folder).empty() ? model_folder : NormalizeAbsolutePath(model_folder);
	const std::string desired_requested_model = Trim(requested_model);
	const bool ok = RestartLocalBridgeIfModelChanged(app, desired_model_folder, desired_requested_model, error_out);

	if (!ok && error_out != nullptr && error_out->empty())
	{
		*error_out = "Failed to ensure OpenCode bridge is running.";
	}

	if (!ok)
	{
		app.opencode_bridge.healthy = false;
	}

	return ok;
}

void LocalBridgeRuntimeService::Stop(AppState& app, const bool keep_token) const
{
	PlatformServicesFactory::Instance().process_service.StopLocalBridgeProcess(app.opencode_bridge);

	const std::string preserved_token = app.opencode_bridge.token;
	ResetOpenCodeBridgeRuntimeFields(app, keep_token);

	if (keep_token)
	{
		app.opencode_bridge.token = preserved_token;
	}
}

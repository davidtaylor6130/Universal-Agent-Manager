#pragma once

#include "bridge_cli_args.h"
#include "bridge_json.h"
#include "bridge_runtime.h"
#include "bridge_tool_envelope.h"

#include <httplib.h>

#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <utility>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace bridge
{
	inline JsonValue BuildErrorObject(const std::string& message, const std::string& type, const std::string& code, const std::string& param = "")
	{
		JsonValue payload = MakeJsonObject();
		JsonValue error = MakeJsonObject();
		error.object_value["message"] = MakeJsonString(message);
		error.object_value["type"] = MakeJsonString(type);

		if (param.empty())
		{
			error.object_value["param"] = MakeJsonNull();
		}
		else
		{
			error.object_value["param"] = MakeJsonString(param);
		}

		if (code.empty())
		{
			error.object_value["code"] = MakeJsonNull();
		}
		else
		{
			error.object_value["code"] = MakeJsonString(code);
		}

		payload.object_value["error"] = std::move(error);
		return payload;
	}

	inline void SetJsonError(httplib::Response& response, const int status, const std::string& message, const std::string& type, const std::string& code, const std::string& param = "")
	{
		response.status = status;
		response.set_content(SerializeJsonCompact(BuildErrorObject(message, type, code, param)), "application/json");
	}

	inline bool AuthorizeRequest(const httplib::Request& request, const std::string& token)
	{
		if (token.empty())
		{
			return true;
		}

		const std::string auth_header = request.get_header_value("Authorization");

		if (auth_header.size() <= 7)
		{
			return false;
		}

		if (!EqualsIgnoreCase(auth_header.substr(0, 7), "Bearer "))
		{
			return false;
		}

		return auth_header.substr(7) == token;
	}

	inline JsonValue BuildModelsResponse(const std::vector<std::string>& models)
	{
		JsonValue root = MakeJsonObject();
		root.object_value["object"] = MakeJsonString("list");
		JsonValue data = MakeJsonArray();
		const std::int64_t now = UnixEpochSecondsNow();

		for (const std::string& model : models)
		{
			JsonValue item = MakeJsonObject();
			item.object_value["id"] = MakeJsonString(model);
			item.object_value["object"] = MakeJsonString("model");
			item.object_value["created"] = MakeJsonNumber(static_cast<double>(now));
			item.object_value["owned_by"] = MakeJsonString("uam-local");
			data.array_value.push_back(std::move(item));
		}

		root.object_value["data"] = std::move(data);
		return root;
	}

	inline JsonValue BuildNonStreamingChatResponse(const CompletionResult& completion)
	{
		JsonValue root = MakeJsonObject();
		root.object_value["id"] = MakeJsonString(completion.request_id);
		root.object_value["object"] = MakeJsonString("chat.completion");
		root.object_value["created"] = MakeJsonNumber(static_cast<double>(completion.created_unix));
		root.object_value["model"] = MakeJsonString(completion.model);

		JsonValue choices = MakeJsonArray();
		JsonValue choice = MakeJsonObject();
		choice.object_value["index"] = MakeJsonNumber(0.0);
		JsonValue message = MakeJsonObject();
		message.object_value["role"] = MakeJsonString("assistant");

		if (completion.tool_call.has_value() && completion.tool_call->is_tool_call)
		{
			message.object_value["content"] = MakeJsonNull();
			JsonValue tool_calls = MakeJsonArray();
			JsonValue call = MakeJsonObject();
			call.object_value["id"] = MakeJsonString(completion.tool_call_id);
			call.object_value["type"] = MakeJsonString("function");
			JsonValue function = MakeJsonObject();
			function.object_value["name"] = MakeJsonString(completion.tool_call->tool_name);
			function.object_value["arguments"] = MakeJsonString(completion.tool_call->tool_arguments_json);
			call.object_value["function"] = std::move(function);
			tool_calls.array_value.push_back(std::move(call));
			message.object_value["tool_calls"] = std::move(tool_calls);
			choice.object_value["finish_reason"] = MakeJsonString("tool_calls");
		}
		else
		{
			message.object_value["content"] = MakeJsonString(completion.assistant_text);
			choice.object_value["finish_reason"] = MakeJsonString("stop");
		}

		choice.object_value["message"] = std::move(message);
		choices.array_value.push_back(std::move(choice));
		root.object_value["choices"] = std::move(choices);

		JsonValue usage = MakeJsonObject();
		usage.object_value["prompt_tokens"] = MakeJsonNumber(static_cast<double>(completion.prompt_tokens));
		usage.object_value["completion_tokens"] = MakeJsonNumber(static_cast<double>(completion.completion_tokens));
		usage.object_value["total_tokens"] = MakeJsonNumber(static_cast<double>(completion.prompt_tokens + completion.completion_tokens));
		root.object_value["usage"] = std::move(usage);

		return root;
	}

	inline JsonValue BuildChunkEnvelope(const CompletionResult& completion, JsonValue delta, const std::optional<std::string>& finish_reason)
	{
		JsonValue root = MakeJsonObject();
		root.object_value["id"] = MakeJsonString(completion.request_id);
		root.object_value["object"] = MakeJsonString("chat.completion.chunk");
		root.object_value["created"] = MakeJsonNumber(static_cast<double>(completion.created_unix));
		root.object_value["model"] = MakeJsonString(completion.model);

		JsonValue choices = MakeJsonArray();
		JsonValue choice = MakeJsonObject();
		choice.object_value["index"] = MakeJsonNumber(0.0);
		choice.object_value["delta"] = std::move(delta);

		if (finish_reason.has_value())
		{
			choice.object_value["finish_reason"] = MakeJsonString(*finish_reason);
		}
		else
		{
			choice.object_value["finish_reason"] = MakeJsonNull();
		}

		choices.array_value.push_back(std::move(choice));
		root.object_value["choices"] = std::move(choices);
		return root;
	}

	inline std::string BuildStreamingSsePayload(const CompletionResult& completion)
	{
		std::ostringstream out;

		{
			JsonValue delta = MakeJsonObject();
			delta.object_value["role"] = MakeJsonString("assistant");
			out << "data: " << SerializeJsonCompact(BuildChunkEnvelope(completion, std::move(delta), std::nullopt)) << "\n\n";
		}

		if (completion.tool_call.has_value() && completion.tool_call->is_tool_call)
		{
			JsonValue delta = MakeJsonObject();
			JsonValue tool_calls = MakeJsonArray();
			JsonValue tool_call = MakeJsonObject();
			tool_call.object_value["index"] = MakeJsonNumber(0.0);
			tool_call.object_value["id"] = MakeJsonString(completion.tool_call_id);
			tool_call.object_value["type"] = MakeJsonString("function");
			JsonValue function = MakeJsonObject();
			function.object_value["name"] = MakeJsonString(completion.tool_call->tool_name);
			function.object_value["arguments"] = MakeJsonString(completion.tool_call->tool_arguments_json);
			tool_call.object_value["function"] = std::move(function);
			tool_calls.array_value.push_back(std::move(tool_call));
			delta.object_value["tool_calls"] = std::move(tool_calls);
			out << "data: " << SerializeJsonCompact(BuildChunkEnvelope(completion, std::move(delta), std::nullopt)) << "\n\n";

			JsonValue done_delta = MakeJsonObject();
			out << "data: " << SerializeJsonCompact(BuildChunkEnvelope(completion, std::move(done_delta), std::string("tool_calls"))) << "\n\n";
			out << "data: [DONE]\n\n";
			return out.str();
		}

		static constexpr std::size_t kChunkSize = 160;
		const std::string text = completion.assistant_text;

		if (text.empty())
		{
			JsonValue empty_delta = MakeJsonObject();
			empty_delta.object_value["content"] = MakeJsonString("");
			out << "data: " << SerializeJsonCompact(BuildChunkEnvelope(completion, std::move(empty_delta), std::nullopt)) << "\n\n";
		}
		else
		{
			for (std::size_t offset = 0; offset < text.size(); offset += kChunkSize)
			{
				const std::string chunk = text.substr(offset, std::min(kChunkSize, text.size() - offset));
				JsonValue delta = MakeJsonObject();
				delta.object_value["content"] = MakeJsonString(chunk);
				out << "data: " << SerializeJsonCompact(BuildChunkEnvelope(completion, std::move(delta), std::nullopt)) << "\n\n";
			}
		}

		JsonValue done_delta = MakeJsonObject();
		out << "data: " << SerializeJsonCompact(BuildChunkEnvelope(completion, std::move(done_delta), std::string("stop"))) << "\n\n";
		out << "data: [DONE]\n\n";
		return out.str();
	}

	inline JsonValue BuildHealthPayload(const BridgeRuntime& runtime)
	{
		JsonValue root = MakeJsonObject();
		root.object_value["ok"] = MakeJsonBool(true);
		root.object_value["status"] = MakeJsonString("ready");
		root.object_value["model"] = MakeJsonString(runtime.LoadedModel());
		root.object_value["timestamp"] = MakeJsonNumber(static_cast<double>(UnixEpochSecondsNow()));
		return root;
	}

	inline bool WriteReadyFileSuccess(const BridgeArgs& args, const std::string& endpoint, const std::string& api_base, const int port, const std::string& model, std::string* error_out)
	{
		if (args.ready_file.empty())
		{
			return true;
		}

		JsonValue payload = MakeJsonObject();
		payload.object_value["ok"] = MakeJsonBool(true);
		payload.object_value["endpoint"] = MakeJsonString(endpoint);
		payload.object_value["api_base"] = MakeJsonString(api_base);
		payload.object_value["host"] = MakeJsonString(args.host);
		payload.object_value["port"] = MakeJsonNumber(static_cast<double>(port));
		payload.object_value["model"] = MakeJsonString(model);
		payload.object_value["token_set"] = MakeJsonBool(!args.token.empty());
#if defined(_WIN32)
		const int process_id = _getpid();
#else
		const int process_id = getpid();
#endif
		payload.object_value["pid"] = MakeJsonNumber(static_cast<double>(process_id));
		return WriteTextFile(args.ready_file, SerializeJsonCompact(payload) + "\n", error_out);
	}

	inline void WriteReadyFileFailure(const BridgeArgs& args, const std::string& error_message)
	{
		if (args.ready_file.empty())
		{
			return;
		}

		JsonValue payload = MakeJsonObject();
		payload.object_value["ok"] = MakeJsonBool(false);
		payload.object_value["error"] = MakeJsonString(error_message);
		std::string ignored;
		WriteTextFile(args.ready_file, SerializeJsonCompact(payload) + "\n", &ignored);
	}

	inline void AttachRoutes(httplib::Server& server, BridgeRuntime& runtime, const BridgeArgs& args)
	{
		const auto lFnHandleHealth = [&](const httplib::Request&, httplib::Response& pResponse) { pResponse.set_content(SerializeJsonCompact(BuildHealthPayload(runtime)), "application/json"); };
		server.Get("/healthz", lFnHandleHealth);

		const auto lFnHandleListModels = [&](const httplib::Request& pRequest, httplib::Response& pResponse)
		{
			if (!AuthorizeRequest(pRequest, args.token))
			{
				SetJsonError(pResponse, 401, "Missing or invalid bearer token.", "invalid_request_error", "unauthorized");
				return;
			}

			std::string lSListError;
			const std::vector<std::string> lVecSModels = runtime.ListModels(&lSListError);

			if (!lSListError.empty())
			{
				SetJsonError(pResponse, 500, lSListError, "server_error", "list_models_failed");
				return;
			}

			pResponse.set_content(SerializeJsonCompact(BuildModelsResponse(lVecSModels)), "application/json");
		};
		server.Get("/v1/models", lFnHandleListModels);

		const auto lFnHandleChatCompletions = [&](const httplib::Request& pRequest, httplib::Response& pResponse)
		{
			if (!AuthorizeRequest(pRequest, args.token))
			{
				SetJsonError(pResponse, 401, "Missing or invalid bearer token.", "invalid_request_error", "unauthorized");
				return;
			}

			const std::optional<JsonValue> lOptParsed = ParseJson(pRequest.body);

			if (!lOptParsed.has_value() || lOptParsed->type != JsonType::Object)
			{
				SetJsonError(pResponse, 400, "Request body must be valid JSON object.", "invalid_request_error", "invalid_json");
				return;
			}

			const bool lbStream = JsonBoolOrDefault(lOptParsed->Find("stream"), false);
			CompletionResult lCompletion;
			std::string lSGenerateError;
			int liStatus = 500;

			if (!runtime.GenerateCompletion(lOptParsed.value(), &lCompletion, &lSGenerateError, &liStatus))
			{
				const std::string lSCode = (liStatus >= 500) ? "engine_failure" : "invalid_request";
				SetJsonError(pResponse, liStatus, lSGenerateError.empty() ? std::string("Unknown bridge generation failure.") : lSGenerateError, (liStatus >= 500) ? "server_error" : "invalid_request_error", lSCode);
				return;
			}

			if (lbStream)
			{
				pResponse.set_header("Cache-Control", "no-cache");
				pResponse.set_header("Connection", "keep-alive");
				pResponse.set_content(BuildStreamingSsePayload(lCompletion), "text/event-stream");
				return;
			}

			pResponse.set_content(SerializeJsonCompact(BuildNonStreamingChatResponse(lCompletion)), "application/json");
		};
		server.Post("/v1/chat/completions", lFnHandleChatCompletions);
	}

	inline int RunBridgeServer(const BridgeArgs& args)
	{
		ollama_engine::EngineOptions engine_options;
		engine_options.pPathModelFolder = args.model_folder;
		engine_options.piEmbeddingDimensions = 256;

		BridgeRuntime runtime(engine_options);
		std::string init_error;

		if (!runtime.Initialize(args.default_model, &init_error))
		{
			WriteReadyFileFailure(args, init_error);
			std::cerr << init_error << "\n";
			return 1;
		}

		if (!args.ready_file.empty())
		{
			std::error_code ec;
			std::filesystem::remove(args.ready_file, ec);
		}

		const bool use_tls = !args.tls_cert.empty() && !args.tls_key.empty();
		const std::string scheme = use_tls ? "https" : "http";

		if (use_tls)
		{
			httplib::SSLServer server(args.tls_cert.string().c_str(), args.tls_key.string().c_str());

			if (!server.is_valid())
			{
				const std::string error = "Failed to initialize HTTPS bridge server (invalid cert/key).";
				WriteReadyFileFailure(args, error);
				std::cerr << error << "\n";
				return 1;
			}

			AttachRoutes(server, runtime, args);
			int bound_port = -1;

			if (args.port == 0)
			{
				bound_port = server.bind_to_any_port(args.host.c_str());
			}
			else if (server.bind_to_port(args.host.c_str(), args.port))
			{
				bound_port = args.port;
			}

			if (bound_port <= 0)
			{
				const std::string error = "Failed to bind HTTPS bridge server to " + args.host + ":" + std::to_string(args.port);
				WriteReadyFileFailure(args, error);
				std::cerr << error << "\n";
				return 1;
			}

			const std::string endpoint = scheme + "://" + args.host + ":" + std::to_string(bound_port);
			const std::string api_base = endpoint + "/v1";
			std::string ready_error;

			if (!WriteReadyFileSuccess(args, endpoint, api_base, bound_port, runtime.LoadedModel(), &ready_error))
			{
				std::cerr << ready_error << "\n";
				return 1;
			}

			if (!server.listen_after_bind())
			{
				std::cerr << "HTTPS bridge server exited unexpectedly.\n";
				return 1;
			}

			return 0;
		}

		httplib::Server server;
		AttachRoutes(server, runtime, args);
		int bound_port = -1;

		if (args.port == 0)
		{
			bound_port = server.bind_to_any_port(args.host.c_str());
		}
		else if (server.bind_to_port(args.host.c_str(), args.port))
		{
			bound_port = args.port;
		}

		if (bound_port <= 0)
		{
			const std::string error = "Failed to bind HTTP bridge server to " + args.host + ":" + std::to_string(args.port);
			WriteReadyFileFailure(args, error);
			std::cerr << error << "\n";
			return 1;
		}

		const std::string endpoint = scheme + "://" + args.host + ":" + std::to_string(bound_port);
		const std::string api_base = endpoint + "/v1";
		std::string ready_error;

		if (!WriteReadyFileSuccess(args, endpoint, api_base, bound_port, runtime.LoadedModel(), &ready_error))
		{
			std::cerr << ready_error << "\n";
			return 1;
		}

		if (!server.listen_after_bind())
		{
			std::cerr << "HTTP bridge server exited unexpectedly.\n";
			return 1;
		}

		return 0;
	}
} // namespace bridge

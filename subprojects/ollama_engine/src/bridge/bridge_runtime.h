#pragma once

#include "bridge_common.h"
#include "bridge_json.h"
#include "bridge_tool_envelope.h"

#include "ollama_engine/engine_factory.h"
#include "ollama_engine/engine_interface.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <utility>

namespace bridge
{
	inline std::string NormalizeRequestedModel(const std::string& raw_model)
	{
		return Trim(raw_model);
	}

	inline std::string ModelBasename(const std::string& model_id)
	{
		if (model_id.empty())
		{
			return "";
		}

		const std::size_t slash = model_id.find_last_of("/\\");

		if (slash == std::string::npos || slash + 1 >= model_id.size())
		{
			return model_id;
		}

		return model_id.substr(slash + 1);
	}

	inline std::string ResolveRequestedModelAgainstAvailable(const std::string& raw_model, const std::vector<std::string>& available_models)
	{
		const std::string requested = Trim(raw_model);

		if (requested.empty())
		{
			return "";
		}

		if (std::find(available_models.begin(), available_models.end(), requested) != available_models.end())
		{
			return requested;
		}

		const std::size_t first_slash = requested.find('/');

		if (first_slash != std::string::npos && first_slash + 1 < requested.size())
		{
			const std::string without_provider = Trim(requested.substr(first_slash + 1));

			if (std::find(available_models.begin(), available_models.end(), without_provider) != available_models.end())
			{
				return without_provider;
			}
		}

		const std::string requested_basename = ModelBasename(requested);

		if (!requested_basename.empty())
		{
			for (const std::string& candidate : available_models)
			{
				if (ModelBasename(candidate) == requested_basename)
				{
					return candidate;
				}
			}
		}

		return requested;
	}

	struct CompletionResult
	{
		std::string request_id;
		std::string model;
		std::string assistant_text;
		std::optional<ParsedToolEnvelope> tool_call;
		std::string tool_call_id;
		std::int64_t created_unix = 0;
		int prompt_tokens = 0;
		int completion_tokens = 0;
	};

	class BridgeRuntime
	{
	  public:
		explicit BridgeRuntime(ollama_engine::EngineOptions options) : options_(std::move(options))
		{
		}

		bool Initialize(const std::string& preferred_model, std::string* error_out)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			engine_ = ollama_engine::CreateEngine(options_);

			if (!engine_)
			{
				if (error_out != nullptr)
				{
					*error_out = "Failed to create ollama_engine runtime.";
				}

				return false;
			}

			const std::vector<std::string> models = engine_->ListModels();

			if (models.empty())
			{
				if (error_out != nullptr)
				{
					*error_out = "No models found in folder: " + options_.pPathModelFolder.string();
				}

				return false;
			}

			std::string model_to_load = preferred_model;

			if (model_to_load.empty())
			{
				model_to_load = models.front();
			}
			else if (std::find(models.begin(), models.end(), model_to_load) == models.end())
			{
				if (error_out != nullptr)
				{
					*error_out = "Default model not found in model folder: " + model_to_load;
				}

				return false;
			}

			std::string load_error;

			if (!engine_->Load(model_to_load, &load_error))
			{
				if (error_out != nullptr)
				{
					*error_out = "Failed to load model '" + model_to_load + "': " + (load_error.empty() ? std::string("unknown error") : load_error);
				}

				return false;
			}

			loaded_model_ = model_to_load;
			return true;
		}

		std::vector<std::string> ListModels(std::string* error_out)
		{
			std::lock_guard<std::mutex> lock(mutex_);

			if (!engine_)
			{
				if (error_out != nullptr)
				{
					*error_out = "Bridge runtime is not initialized.";
				}

				return {};
			}

			return engine_->ListModels();
		}

		std::string LoadedModel() const
		{
			std::lock_guard<std::mutex> lock(mutex_);
			return loaded_model_;
		}

		bool GenerateCompletion(const JsonValue& request, CompletionResult* result_out, std::string* error_out, int* status_out)
		{
			if (result_out == nullptr)
			{
				if (error_out != nullptr)
				{
					*error_out = "Internal output state is null.";
				}

				if (status_out != nullptr)
				{
					*status_out = 500;
				}

				return false;
			}

			const JsonValue* messages = request.Find("messages");

			if (messages == nullptr || messages->type != JsonType::Array || messages->array_value.empty())
			{
				if (error_out != nullptr)
				{
					*error_out = "Request must include a non-empty messages array.";
				}

				if (status_out != nullptr)
				{
					*status_out = 400;
				}

				return false;
			}

			const std::vector<ToolDefinition> tools = ParseToolDefinitions(request.Find("tools"));
			const bool has_tools = !tools.empty();
			const bool require_tool_call = has_tools && RequestLikelyNeedsToolCall(*messages, tools);
			const std::string latest_user_message = LatestUserMessageText(*messages);

			const std::string conversation_transcript = BuildTranscriptPrompt(*messages);

			if (conversation_transcript.empty())
			{
				if (error_out != nullptr)
				{
					*error_out = "Could not derive prompt text from messages.";
				}

				if (status_out != nullptr)
				{
					*status_out = 400;
				}

				return false;
			}

			std::string generation_prompt = conversation_transcript;

			if (has_tools)
			{
				generation_prompt = BuildToolEnvelopePrompt(conversation_transcript, tools);
			}

			const std::string requested_model = NormalizeRequestedModel(JsonStringOrEmpty(request.Find("model")));

			ollama_engine::SendMessageResponse engine_response;
			std::string effective_model;
			{
				std::lock_guard<std::mutex> lock(mutex_);

				if (!engine_)
				{
					if (error_out != nullptr)
					{
						*error_out = "Bridge runtime is not initialized.";
					}

					if (status_out != nullptr)
					{
						*status_out = 500;
					}

					return false;
				}

				std::string load_error;

				if (!requested_model.empty())
				{
					if (!EnsureModelLoadedLocked(requested_model, &load_error))
					{
						if (error_out != nullptr)
						{
							*error_out = load_error.empty() ? "Failed to load requested model." : load_error;
						}

						if (status_out != nullptr)
						{
							*status_out = 400;
						}

						return false;
					}
				}

				effective_model = loaded_model_;
				engine_response = engine_->SendMessage(generation_prompt);
			}

			if (!engine_response.pbOk)
			{
				if (error_out != nullptr)
				{
					*error_out = engine_response.pSError.empty() ? "Local engine request failed." : engine_response.pSError;
				}

				if (status_out != nullptr)
				{
					*status_out = 500;
				}

				return false;
			}

			CompletionResult completion;
			completion.request_id = BuildId("chatcmpl");
			completion.model = effective_model;
			completion.created_unix = UnixEpochSecondsNow();
			completion.prompt_tokens = CountApproxTokens(generation_prompt);

			std::string raw_text = Trim(engine_response.pSText);

			if (has_tools)
			{
				std::set<std::string> allowed_tool_names;

				for (const ToolDefinition& tool : tools)
				{
					allowed_tool_names.insert(tool.name);
				}

				std::optional<ParsedToolEnvelope> envelope = ParseToolEnvelopeResponse(raw_text, allowed_tool_names);

				if (!envelope.has_value() && LooksLikeToolEnvelopeCandidate(raw_text))
				{
					const std::string repair_prompt = BuildToolEnvelopeRepairPrompt(conversation_transcript, tools, raw_text);
					ollama_engine::SendMessageResponse repair_response;
					{
						std::lock_guard<std::mutex> lock(mutex_);

						if (engine_)
						{
							repair_response = engine_->SendMessage(repair_prompt);
						}
					}

					if (repair_response.pbOk)
					{
						const std::string repaired_text = Trim(repair_response.pSText);

						if (!repaired_text.empty())
						{
							raw_text = repaired_text;
							completion.prompt_tokens += CountApproxTokens(repair_prompt);
							envelope = ParseToolEnvelopeResponse(raw_text, allowed_tool_names);
						}
					}
				}

				if (require_tool_call && (!envelope.has_value() || !envelope->is_tool_call))
				{
					const std::string enforce_prompt = BuildToolCallOnlyRepairPrompt(conversation_transcript, tools, raw_text);
					ollama_engine::SendMessageResponse enforce_response;
					{
						std::lock_guard<std::mutex> lock(mutex_);

						if (engine_)
						{
							enforce_response = engine_->SendMessage(enforce_prompt);
						}
					}

					if (enforce_response.pbOk)
					{
						const std::string enforced_text = Trim(enforce_response.pSText);

						if (!enforced_text.empty())
						{
							raw_text = enforced_text;
							completion.prompt_tokens += CountApproxTokens(enforce_prompt);
							envelope = ParseToolEnvelopeResponse(raw_text, allowed_tool_names);
						}
					}
				}

				if (!envelope.has_value() && LooksLikeToolEnvelopeCandidate(raw_text))
				{
					const std::string fallback_prompt = conversation_transcript + "\n\nReply to the user directly in plain text. Do not output JSON.";
					ollama_engine::SendMessageResponse fallback_response;
					{
						std::lock_guard<std::mutex> lock(mutex_);

						if (engine_)
						{
							fallback_response = engine_->SendMessage(fallback_prompt);
						}
					}

					if (fallback_response.pbOk)
					{
						const std::string fallback_text = Trim(fallback_response.pSText);

						if (!fallback_text.empty())
						{
							raw_text = fallback_text;
							completion.prompt_tokens += CountApproxTokens(fallback_prompt);
						}
					}
				}

				if (require_tool_call && (!envelope.has_value() || !envelope->is_tool_call) && allowed_tool_names.find("bash") != allowed_tool_names.end())
				{
					const std::string user_request_for_rescue = latest_user_message.empty() ? conversation_transcript : latest_user_message;
					const std::string rescue_prompt = BuildBashRescuePrompt(user_request_for_rescue, raw_text);
					ollama_engine::SendMessageResponse rescue_response;
					{
						std::lock_guard<std::mutex> lock(mutex_);

						if (engine_)
						{
							rescue_response = engine_->SendMessage(rescue_prompt);
						}
					}

					if (rescue_response.pbOk)
					{
						const std::string rescued_text = Trim(rescue_response.pSText);

						if (!rescued_text.empty())
						{
							completion.prompt_tokens += CountApproxTokens(rescue_prompt);

							if (const std::optional<ParsedToolEnvelope> rescued_envelope = ParseToolEnvelopeResponse(rescued_text, allowed_tool_names); rescued_envelope.has_value() && rescued_envelope->is_tool_call)
							{
								raw_text = rescued_text;
								envelope = rescued_envelope;
							}
							else
							{
								const std::string bash_command = ExtractBashCommandFromText(rescued_text);

								if (!bash_command.empty())
								{
									ParsedToolEnvelope synthesized;
									synthesized.is_tool_call = true;
									synthesized.tool_name = "bash";
									JsonValue args = MakeJsonObject();
									args.object_value["description"] = MakeJsonString("Execute requested shell command.");
									args.object_value["command"] = MakeJsonString(bash_command);
									synthesized.tool_arguments_json = SerializeJsonCompact(args);
									raw_text = rescued_text;
									envelope = std::move(synthesized);
								}
							}
						}
					}
				}

				if (!envelope.has_value())
				{
					const std::string candidate = Trim(StripMarkdownCodeFence(raw_text));

					if (!candidate.empty() && (candidate == "{" || candidate == "{\"" || candidate == "[" || candidate.size() <= 3))
					{
						raw_text = "I could not generate a valid tool call. Please retry with a shorter request.";
					}
				}

				if (require_tool_call && (!envelope.has_value() || !envelope->is_tool_call))
				{
					const std::string synthesis_seed = latest_user_message.empty() ? raw_text : latest_user_message;

					if (const std::optional<ParsedToolEnvelope> synthesized = SynthesizeFallbackToolCall(allowed_tool_names, synthesis_seed); synthesized.has_value())
					{
						envelope = synthesized;
					}
				}

				if (require_tool_call && (!envelope.has_value() || !envelope->is_tool_call))
				{
					raw_text = "I could not produce a valid tool call for this action request. "
					           "Please retry with a simpler command or use a stronger local model.";
				}

				if (envelope.has_value() && envelope->is_tool_call)
				{
					completion.tool_call = envelope;
					completion.tool_call_id = BuildId("call");
					completion.completion_tokens = CountApproxTokens(envelope->tool_name + " " + envelope->tool_arguments_json);
				}
				else if (envelope.has_value() && !envelope->is_tool_call)
				{
					completion.assistant_text = envelope->text;
					completion.completion_tokens = CountApproxTokens(completion.assistant_text);
				}
				else
				{
					completion.assistant_text = raw_text;
					completion.completion_tokens = CountApproxTokens(completion.assistant_text);
				}
			}
			else
			{
				completion.assistant_text = raw_text;
				completion.completion_tokens = CountApproxTokens(completion.assistant_text);
			}

			*result_out = std::move(completion);
			return true;
		}

	  private:
		bool EnsureModelLoadedLocked(const std::string& model_name, std::string* error_out)
		{
			if (!engine_)
			{
				if (error_out != nullptr)
				{
					*error_out = "Bridge runtime is not initialized.";
				}

				return false;
			}

			const std::vector<std::string> models = engine_->ListModels();
			const std::string resolved_model = ResolveRequestedModelAgainstAvailable(model_name, models);

			if (loaded_model_ == resolved_model)
			{
				return true;
			}

			if (std::find(models.begin(), models.end(), resolved_model) == models.end())
			{
				if (error_out != nullptr)
				{
					*error_out = "Requested model was not found: " + model_name;
				}

				return false;
			}

			std::string load_error;

			if (!engine_->Load(resolved_model, &load_error))
			{
				if (error_out != nullptr)
				{
					*error_out = "Failed to load model '" + resolved_model + "': " + (load_error.empty() ? std::string("unknown error") : load_error);
				}

				return false;
			}

			loaded_model_ = resolved_model;
			return true;
		}

		ollama_engine::EngineOptions options_;
		mutable std::mutex mutex_;
		std::unique_ptr<ollama_engine::EngineInterface> engine_;
		std::string loaded_model_;
	};
} // namespace bridge

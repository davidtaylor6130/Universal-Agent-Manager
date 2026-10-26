#include "common/provider/gemini/base/gemini_history_loader.h"

#include "common/paths/path_utils.h"
#include "common/paths/app_paths.h"
#include "common/provider/provider_runtime.h"
#include "common/provider/gemini_history_journal.h"
#include "common/provider/gemini_history_paths.h"
#include "common/provider/native_agent_history.h"
#include "common/utils/uuid.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"
#include "runtime/json_runtime.h"
#include "core/native_import_content.h"

#include <sstream>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <unordered_map>
#include "common/chat/native_attachment_assets.h"

namespace fs = std::filesystem;

namespace
{
	struct MappingState
	{
		std::shared_ptr<GeminiHistoryParseContext> context;
		std::stop_token cancel;
		std::string error;
		std::size_t visited = 0;
		bool source_has_conversation = false;
		bool Check(std::size_t depth = 0)
		{
			if (!error.empty()) return false;
			if (cancel.stop_requested()) { error = "Gemini native history mapping cancelled."; return false; }
			if (depth > 64 || ++visited > 4000000) { error = "Gemini native history exceeds the bounded mapping structure limit."; return false; }
			return true;
		}
		bool Fail(const char* reason) { if (error.empty()) error = reason; return false; }
	};

	JsonValue ConvertJournalValue(nlohmann::json& value, MappingState& state, std::size_t depth = 0)
	{
		JsonValue result;
		if (!state.Check(depth)) return result;
		if (value.is_object())
		{
			result.type = JsonValue::Type::Object;
			for (auto item = value.begin(); item != value.end(); ++item)
			{
				result.object_value.emplace(item.key(), ConvertJournalValue(item.value(), state, depth + 1));
				if (!state.error.empty()) break;
			}
		}
		else if (value.is_array())
		{
			result.type = JsonValue::Type::Array;
			result.array_value.reserve(value.size());
			for (auto& item : value)
			{
				result.array_value.push_back(ConvertJournalValue(item, state, depth + 1));
				if (!state.error.empty()) break;
			}
		}
		else if (value.is_string()) { result.type = JsonValue::Type::String; result.string_value = std::move(value.get_ref<std::string&>()); }
		else if (value.is_boolean()) { result.type = JsonValue::Type::Bool; result.bool_value = value.get<bool>(); }
		else if (value.is_number()) { result.type = JsonValue::Type::Number; result.number_value = value.get<double>(); }
		return result;
	}

	void AppendGeminiText(std::string& output, const std::string& text)
	{
		if (text.empty()) return;
		if (!output.empty()) output += "\n";
		output += text;
	}

	std::string GeminiThoughtText(const JsonValue& thought)
	{
		if (thought.type != JsonValue::Type::Object) return ExtractGeminiContentText(&thought);
		std::string text;
		AppendGeminiText(text, JsonStringOrEmpty(thought.Find("subject")));
		AppendGeminiText(text, JsonStringOrEmpty(thought.Find("description")));
		return text.empty() ? ExtractGeminiContentText(&thought) : text;
	}

	std::string IdentityComponent(std::string_view value)
	{
		return std::to_string(value.size()) + ":" + std::string(value);
	}

	bool BoundedIdentity(std::string_view value)
	{
		return value.size() <= 1024 && value.find('\0') == std::string_view::npos;
	}

	struct RawMedia
	{
		JsonValue* blob = nullptr;
		bool inline_data = false;
		std::string location;
		std::string tool_id;
		bool display = false;
		std::string association_identity;
	};

	bool HasMediaShape(const JsonValue& value, MappingState& state, std::size_t depth = 0)
	{
		if (!state.Check(depth)) return true;
		if (value.Find("inlineData") || value.Find("fileData") || value.Find("fileUri") ||
		    (value.Find("mimeType") && value.Find("data"))) return true;
		for (const auto& [key, child] : value.object_value) if (HasMediaShape(child, state, depth + 1)) return true;
		for (const auto& child : value.array_value) if (HasMediaShape(child, state, depth + 1)) return true;
		return false;
	}

	class PartMapper
	{
	  public:
		PartMapper(MappingState& state, std::string attempt) : state_(state), attempt_(std::move(attempt)) {}
		std::vector<RawMedia> media;

		// First pass counts media-bearing response IDs independently in canonical
		// and display sources. One occurrence in each is a possible correspondence;
		// repeated IDs in either source are ambiguous and never cross-snapshot keys.
		bool IndexResponseIds(const JsonValue* value, bool display = false, std::size_t depth = 0)
		{
			if (!state_.Check(depth)) return false;
			if (!value) return true;
			if (value->type == JsonValue::Type::Array)
			{
				for (const auto& child : value->array_value) if (!IndexResponseIds(&child, display, depth + 1)) return false;
			}
			else if (const auto response = value->Find("functionResponse"); response && response->type == JsonValue::Type::Object)
			{
				const auto* id = response->Find("id");
				if (id && id->type == JsonValue::Type::String && BoundedIdentity(id->string_value) && !id->string_value.empty() && HasMediaShape(*response, state_))
				{
					if (!response_ids_.contains(id->string_value) && response_ids_.size() >= GeminiHistoryParseContext::kMaxAttachments * 2)
						return state_.Fail("Gemini native history exceeds the bounded response identity collection.");
					auto& counts = response_ids_[id->string_value];
					++(display ? counts.display : counts.canonical);
				}
				if (const auto body = response->Find("response"); body && body->type == JsonValue::Type::Object)
					for (const auto* field : {"output", "error"}) if (!IndexResponseIds(body->Find(field), display, depth + 1)) return false;
			}
			return state_.error.empty();
		}

		bool Walk(JsonValue* value, std::string& text, const std::string& location, const std::string& tool_id = {}, bool display = false,
		    std::size_t depth = 0, const std::string& association_identity = {}, bool trim_text_parts = false)
		{
			if (!state_.Check(depth)) return false;
			if (!value || value->type == JsonValue::Type::Null) return true;
			if (value->type == JsonValue::Type::String) { AppendGeminiText(text, trim_text_parts ? uam::strings::Trim(value->string_value) : value->string_value); return true; }
			if (value->type == JsonValue::Type::Array)
			{
				for (std::size_t index = 0; index < value->array_value.size(); ++index)
					if (!Walk(&value->array_value[index], text, location + "/" + std::to_string(index), tool_id, display, depth + 1, association_identity, trim_text_parts)) return false;
				return true;
			}
			if (value->type != JsonValue::Type::Object) return state_.Fail("Gemini native history contains an unsupported PartListUnion value.");
			if (value->Find("inlineData") && value->Find("fileData")) return state_.Fail("Gemini native history contains conflicting media alternatives in one part.");
			for (auto& [key, child] : value->object_value)
			{
				if (key == "inlineData" || key == "fileData")
				{
					if (!AddMedia(child, key == "inlineData", location, tool_id, display, association_identity)) return false;
				}
				else if (key == "text")
				{
					if (child.type != JsonValue::Type::String) return state_.Fail("Gemini native history contains a malformed text part.");
					AppendGeminiText(text, trim_text_parts ? uam::strings::Trim(child.string_value) : child.string_value);
				}
				else if (key == "functionResponse")
				{
					if (child.type != JsonValue::Type::Object) return state_.Fail("Gemini native history contains a malformed function response.");
					for (const auto& [field, extra] : child.object_value)
						if (field != "id" && field != "response" && HasMediaShape(extra, state_))
							return state_.Fail("Gemini function response contains an unsupported binary field.");
					const auto* recorded_id = child.Find("id");
					if (recorded_id && (recorded_id->type != JsonValue::Type::String || recorded_id->string_value.empty() || !BoundedIdentity(recorded_id->string_value)))
						return state_.Fail("Gemini native function response has an invalid bounded tool identity.");
					if (recorded_id && !tool_id.empty() && recorded_id->string_value != tool_id)
						return state_.Fail("Gemini native function response conflicts with its inherited tool identity.");
					const std::string response_tool_id = recorded_id ? recorded_id->string_value : tool_id;
					const auto counts = response_ids_.find(response_tool_id);
					const bool duplicate = counts != response_ids_.end() && (counts->second.canonical > 1 || counts->second.display > 1);
					const std::string response_identity = response_tool_id.empty() || duplicate ?
					    "snapshot:" + IdentityComponent(attempt_) + IdentityComponent(location) : "id:" + IdentityComponent(response_tool_id);
					const auto response_location = location + "/functionResponse/" + IdentityComponent(response_identity) + "/response/";
					if (auto response = child.Find("response"); response && response->type != JsonValue::Type::Null)
					{
						if (response->type != JsonValue::Type::Object) return state_.Fail("Gemini native history contains a malformed function response body.");
						for (const auto& [field, extra] : response->object_value)
							if (field != "output" && field != "error" && HasMediaShape(extra, state_)) return state_.Fail("Gemini function response contains an unsupported binary location.");
						for (const auto* field : {"output", "error"})
							if (!Response(response->Find(field), text, response_location + field, response_tool_id, display, depth + 1, response_identity)) return false;
					}
				}
				else if (key == "executableCode" || key == "codeExecutionResult")
				{
					if (child.type != JsonValue::Type::Object || HasMediaShape(child, state_)) return state_.Fail("Gemini native history contains an unsupported code part.");
					AppendGeminiText(text, JsonStringOrEmpty(child.Find(key == "executableCode" ? "code" : "output")));
				}
				else if (key == "functionCall")
				{
					if (child.type != JsonValue::Type::Object || HasMediaShape(child, state_)) return state_.Fail("Gemini native history contains an unsupported function-call part.");
				}
				else if (key == "thought" || key == "thoughtSignature" || key == "videoMetadata")
				{
					// These annotations are not attachment locations. Keep ordinary
					// metadata compatible, but never accept an unvisited original.
					if (HasMediaShape(child, state_)) return state_.Fail("Gemini native part metadata contains an unsupported binary location.");
				}
				else return state_.Fail("Gemini native history contains an unsupported part shape; retry with a supported mapper.");
			}
			return state_.error.empty();
		}

		bool Materialize(Message& message, const std::string& row_key, const std::string& host)
		{
			// Compare the still-owned strings before moving any original. Match each
			// display occurrence once; identical parts in the same source stay distinct.
			std::vector<std::size_t> canonical(media.size());
			std::vector<bool> matched(media.size(), false);
			for (std::size_t index = 0; index < media.size(); ++index)
			{
				canonical[index] = index;
				if (!media[index].display) continue;
				for (std::size_t prior = 0; prior < index; ++prior)
				{
					if (!state_.Check()) return false;
					if (media[prior].display || matched[prior] || media[prior].tool_id != media[index].tool_id || media[prior].association_identity != media[index].association_identity || media[prior].inline_data != media[index].inline_data) continue;
					const auto* first = media[prior].blob;
					const auto* second = media[index].blob;
					const auto* data_key = media[index].inline_data ? "data" : "fileUri";
					if (first->Find("mimeType")->string_value == second->Find("mimeType")->string_value &&
					    first->Find(data_key)->string_value == second->Find(data_key)->string_value)
					{ canonical[index] = prior; matched[prior] = true; break; }
				}
			}
			for (std::size_t index = 0; index < media.size(); ++index)
			{
				if (!state_.Check()) return false;
				if (canonical[index] != index) continue;
				auto& raw = media[index];
				MessageAttachment attachment;
				attachment.mime_type = raw.blob->Find("mimeType")->string_value;
				attachment.origin_execution_host_id = host;
				attachment.native_tool_call_id = raw.tool_id;
				attachment.native_source_field = raw.location;
				for (std::size_t duplicate = index + 1; duplicate < media.size(); ++duplicate)
					if (canonical[duplicate] == index) attachment.native_source_field += "|" + media[duplicate].location;
				attachment.native_part_key = row_key + IdentityComponent(raw.location);
				attachment.id = attachment.native_part_key;
				attachment.kind = attachment.mime_type.starts_with("image/") ? "image" : "file";
				attachment.name = "Native attachment " + std::to_string(message.attachments.size() + 1);
				std::size_t metadata = attachment.id.size() + attachment.native_part_key.size() + attachment.native_source_field.size() +
				    attachment.mime_type.size() + host.size() + raw.tool_id.size() + attachment.name.size();
				if (!raw.inline_data) metadata += raw.blob->Find("fileUri")->string_value.size();
				if (attachment.native_part_key.size() > 16384) return state_.Fail("Gemini native part provenance exceeds its bound.");
				if (!state_.context->AdmitMetadata(metadata, state_.error)) return false;
				if (raw.inline_data)
				{
					attachment.source = AttachmentSource::NativeInlinePending;
					attachment.native_inline_payload = state_.context->MakePayload(std::move(raw.blob->Find("data")->string_value), attachment.mime_type, state_.error, state_.cancel);
					if (!attachment.native_inline_payload) return false;
					attachment.content_digest = attachment.native_inline_payload->Digest();
					attachment.size_bytes = attachment.native_inline_payload->DecodedSize();
				}
				else
				{
					attachment.source = AttachmentSource::ProviderUri;
					attachment.source_uri = std::move(raw.blob->Find("fileUri")->string_value);
				}
				message.attachments.push_back(std::move(attachment));
			}
			return true;
		}

	  private:
		MappingState& state_;
		std::string attempt_;
		struct ResponseCounts { std::size_t canonical = 0; std::size_t display = 0; };
		std::unordered_map<std::string, ResponseCounts> response_ids_;
		bool AddMedia(JsonValue& blob, bool inline_data, const std::string& location, const std::string& tool_id, bool display, const std::string& association_identity)
		{
			const auto* mime = blob.Find("mimeType");
			const auto* data = blob.Find(inline_data ? "data" : "fileUri");
			if (blob.type != JsonValue::Type::Object || !mime || mime->type != JsonValue::Type::String || !data || data->type != JsonValue::Type::String ||
			    mime->string_value.empty() || mime->string_value.size() > 255 || mime->string_value.find('/') == std::string::npos ||
			    std::any_of(mime->string_value.begin(), mime->string_value.end(), [](unsigned char c) { return c < 32 || c >= 127; }))
				return state_.Fail("Gemini native history contains malformed attachment fields.");
			for (const auto& [key, child] : blob.object_value)
				if (key != "mimeType" && key != (inline_data ? "data" : "fileUri")) return state_.Fail("Gemini native history contains unsupported attachment fields.");
			if (!inline_data && (data->string_value.empty() || data->string_value.size() > GeminiHistoryParseContext::kMaxUriBytes ||
			    std::any_of(data->string_value.begin(), data->string_value.end(), [](unsigned char c) { return c < 32 || c == 127; })))
				return state_.Fail("Gemini provider URI is invalid or exceeds the 16 KiB metadata bound.");
			if (media.size() >= GeminiHistoryParseContext::kMaxAttachments * 2) return state_.Fail("Gemini native history exceeds the bounded part collection limit.");
			media.push_back({&blob, inline_data, location, tool_id, display, association_identity});
			return true;
		}

		bool Response(JsonValue* value, std::string& text, const std::string& location, const std::string& tool_id, bool display, std::size_t depth, const std::string& association_identity)
		{
			if (!state_.Check(depth)) return false;
			if (!value || value->type == JsonValue::Type::Null) return true;
			if (value->type == JsonValue::Type::Number || value->type == JsonValue::Type::Bool)
			{
				AppendGeminiText(text, SerializeJson(*value, GeminiHistoryParseContext::kMaxMetadataBytes));
				return true;
			}
			if (value->type != JsonValue::Type::Object || value->Find("text") || value->Find("inlineData") || value->Find("fileData") || value->Find("functionResponse"))
				return Walk(value, text, location, tool_id, display, depth + 1, association_identity);
			// An arbitrary tool JSON object is text only after ruling out nested
			// media shapes. Never dump a binary-bearing object into result_text.
			if (HasMediaShape(*value, state_)) return state_.Fail("Gemini tool response contains an unsupported nested binary shape.");
			const auto message = JsonStringOrEmpty(value->Find("message"));
			AppendGeminiText(text, message.empty() ? SerializeJson(*value, GeminiHistoryParseContext::kMaxMetadataBytes) : message);
			return true;
		}
	};

	std::optional<ChatSession> MapGeminiDocument(JsonValue& root, const ProviderProfile& provider, const GeminiJsonHistoryStoreOptions& options, MappingState& state)
	{
		const std::string session_id = uam::strings::Trim(JsonStringOrEmpty(root.Find("sessionId")));

		if (session_id.empty() || !BoundedIdentity(session_id) || !BoundedIdentity(provider.id) || !BoundedIdentity(options.execution_host_id))
		{
			state.Fail("Gemini native history has an invalid bounded session/provider/host identity.");
			return std::nullopt;
		}
		std::string scope = "gemini-part-v1:" + IdentityComponent(provider.id) + IdentityComponent(session_id) + IdentityComponent(options.execution_host_id);
		for (const auto* field : {"projectHash", "parentSessionId", "agentId", "workspace"})
		{
			const auto value = JsonStringOrEmpty(root.Find(field));
			if (!BoundedIdentity(value)) { state.Fail("Gemini native history provenance exceeds its bound."); return std::nullopt; }
			scope += IdentityComponent(value);
		}
		if (const auto directories = root.Find("directories"); directories && directories->type == JsonValue::Type::Array)
		{
			if (directories->array_value.size() > 16) { state.Fail("Gemini native history directory provenance exceeds its bound."); return std::nullopt; }
			for (const auto& directory : directories->array_value)
			{
				if (directory.type != JsonValue::Type::String || !BoundedIdentity(directory.string_value)) { state.Fail("Gemini native history has invalid directory provenance."); return std::nullopt; }
				scope += IdentityComponent(directory.string_value);
			}
		}
		if (scope.size() > 8192) { state.Fail("Gemini native history provenance exceeds its bound."); return std::nullopt; }
		static std::atomic<std::uint64_t> next_attempt{0};
		const std::string attempt = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()) + ":" + std::to_string(next_attempt.fetch_add(1));

		ChatSession chat;
		chat.id = session_id;
		chat.provider_id = provider.id;
		chat.native_session_id = session_id;
		chat.parent_chat_id.clear();
		chat.branch_root_chat_id = session_id;
		chat.branch_from_message_index = -1;
		chat.created_at = JsonStringOrEmpty(root.Find("startTime"));
		chat.updated_at = JsonStringOrEmpty(root.Find("lastUpdated"));
		chat.last_opened_at = chat.updated_at;

		if (chat.created_at.empty())
		{
			chat.created_at = uam::time::TimestampNow();
		}

		if (chat.updated_at.empty())
		{
			chat.updated_at = chat.created_at;
		}

		if (chat.last_opened_at.empty())
		{
			chat.last_opened_at = chat.updated_at;
		}

		JsonValue* messages = root.Find("messages");
		if (!messages || messages->type != JsonValue::Type::Array) { state.Fail("Gemini native history lacks a valid current message collection."); return std::nullopt; }
		std::unordered_map<std::string, std::size_t> message_ids;
		for (const auto& row : messages->array_value) ++message_ids[JsonStringOrEmpty(row.Find("id"))];
		std::size_t row_index = 0;
		if (messages != nullptr && messages->type == JsonValue::Type::Array)
		{
			for (JsonValue& raw_message : messages->array_value)
			{
				if (!state.Check()) return std::nullopt;
				const auto ordinal = row_index++;
				if (raw_message.type != JsonValue::Type::Object) { state.Fail("Gemini native history contains a malformed current message."); return std::nullopt; }
				const auto native_id = JsonStringOrEmpty(raw_message.Find("id"));
				if (!BoundedIdentity(native_id)) { state.Fail("Gemini native message identity exceeds its bound."); return std::nullopt; }
				const auto row_identity = !native_id.empty() && message_ids[native_id] == 1 ? "id:" + IdentityComponent(native_id) : "snapshot:" + attempt + ":" + std::to_string(ordinal);
				const auto row_key = scope + IdentityComponent(row_identity);
				PartMapper parts(state, attempt);
				if (!parts.IndexResponseIds(raw_message.Find("content")) || !parts.IndexResponseIds(raw_message.Find("displayContent"), true)) return std::nullopt;
				if (const auto tools = raw_message.Find("toolCalls"); tools && tools->type == JsonValue::Type::Array)
					for (const auto& tool : tools->array_value) if (!parts.IndexResponseIds(tool.Find("result"))) return std::nullopt;

				const std::string type = JsonStringOrEmpty(raw_message.Find("type"));
				const std::string timestamp = JsonStringOrEmpty(raw_message.Find("timestamp"));
				std::string content, display_content;
				if (!parts.Walk(raw_message.Find("content"), content, "content", {}, false, 0, {}, true) ||
				    !parts.Walk(raw_message.Find("displayContent"), display_content, "displayContent", {}, true, 0, {}, true)) return std::nullopt;
				content = uam::strings::Trim(content.empty() ? display_content : content);

				Message message;
				message.role = ProviderRuntime::RoleFromNativeType(provider, type);
				message.content = content;
				message.model_id = JsonStringOrEmpty(raw_message.Find("model"));
				if (!message.model_id.empty()) chat.model_id = message.model_id;
				message.created_at = uam::strings::NonEmptyOrFallback(timestamp, chat.updated_at);

				JsonValue* tool_calls = raw_message.Find("toolCalls");
				if (tool_calls && tool_calls->type != JsonValue::Type::Null && tool_calls->type != JsonValue::Type::Array)
				{ state.Fail("Gemini native history contains malformed tools."); return std::nullopt; }
				if (tool_calls != nullptr && tool_calls->type == JsonValue::Type::Array)
				{
					std::unordered_map<std::string, std::size_t> tool_ids;
					for (const auto& tool : tool_calls->array_value) ++tool_ids[JsonStringOrEmpty(tool.Find("id"))];
					std::size_t tool_index = 0;
					for (JsonValue& tc : tool_calls->array_value)
					{
						if (!state.Check()) return std::nullopt;
						const auto tool_ordinal = tool_index++;
						if (tc.type != JsonValue::Type::Object) { state.Fail("Gemini native history contains a malformed tool."); return std::nullopt; }

						ToolCall tool_call;
						tool_call.id = JsonStringOrEmpty(tc.Find("id"));
						tool_call.name = JsonStringOrEmpty(tc.Find("name"));
						tool_call.status = JsonStringOrEmpty(tc.Find("status"));

						if (!BoundedIdentity(tool_call.id)) { state.Fail("Gemini native tool identity exceeds its bound."); return std::nullopt; }
						const auto tool_identity = !tool_call.id.empty() && tool_ids[tool_call.id] == 1 ? "id:" + IdentityComponent(tool_call.id) : "snapshot:" + attempt + ":" + std::to_string(tool_ordinal);
						const JsonValue* args = tc.Find("args");
						if (args && args->type == JsonValue::Type::Object)
						{
							if (HasMediaShape(*args, state)) { state.Fail("Gemini tool arguments contain an unsupported binary shape."); return std::nullopt; }
							tool_call.args_json = SerializeJson(*args);
						}

						if (!parts.Walk(tc.Find("result"), tool_call.result_text, "toolCalls/" + IdentityComponent(tool_identity) + "/result", tool_call.id, false, 0, tool_identity)) return std::nullopt;

						const std::string child_id = JsonStringOrEmpty(tc.Find("agentId"));
						if (uam::uuid::IsCanonicalUuid(session_id) && uam::uuid::IsCanonicalUuid(child_id) && child_id != session_id)
						{
							const JsonValue* display = tc.Find("resultDisplay");
							tool_call.sub_agent_id = uam::native_agents::BuildHistoryId(provider.id, session_id, child_id, options.execution_host_id);
							if (!tool_call.sub_agent_id.empty())
							{
								tool_call.is_sub_agent = true;
								tool_call.sub_agent_title = display ? JsonStringOrEmpty(display->Find("agentName")) : "";
								tool_call.sub_agent_children = {{tool_call.sub_agent_id, tool_call.sub_agent_title, display ? JsonStringOrEmpty(display->Find("state")) : ""}};
								tool_call.sub_agent_children_authoritative = true;
							}
						}
						message.tool_calls.push_back(std::move(tool_call));
					}
				}

				const JsonValue* thoughts = raw_message.Find("thoughts");
				if (thoughts != nullptr && thoughts->type == JsonValue::Type::Array)
				{
					std::ostringstream thought_out;
					bool has_thought_text = false;

					for (const JsonValue& thought : thoughts->array_value)
					{
						const std::string thought_text = GeminiThoughtText(thought);
						if (!thought_text.empty())
						{
							if (has_thought_text)
							{
								thought_out << "\n";
							}

							thought_out << thought_text;
							has_thought_text = true;
						}
					}

					message.thoughts = thought_out.str();
				}

				if (!parts.Materialize(message, row_key, options.execution_host_id)) return std::nullopt;
				// Eligibility describes the whole validated current source, not a
				// requested prefix. Later rows still validate before final disposition.
				if (uam::HasImportedConversationContent(message)) state.source_has_conversation = true;
				if (message.content.empty() && uam::strings::IsBlank(message.thoughts) && message.tool_calls.empty() && message.attachments.empty())
				{
					continue;
				}

				// A requested projection cap never skips validation of later native rows.
				if (options.max_messages == 0 || chat.messages.size() < options.max_messages) chat.messages.push_back(std::move(message));
			}
		}

		// Empty Gemini sessions are placeholder artifacts, not user-visible chats.
		// Importing or restoring them causes undeletable ghost entries that contain
		// no user messages.
		if (chat.messages.empty())
		{
			return std::nullopt;
		}

		chat.title = "Session " + chat.created_at;

		for (const Message& message : chat.messages)
		{
			if (message.role == MessageRole::User)
			{
				const std::string title = uam::strings::TrimAndElide(message.content, 48);

				if (!title.empty())
				{
					chat.title = title;
				}

				break;
			}
		}

		return chat;
	}
}

GeminiHistoryParseContext::GeminiHistoryParseContext() : payload_budget(std::make_shared<uam::native_assets::PayloadBudget>()) {}

std::shared_ptr<const uam::native_assets::InlinePayload> GeminiHistoryParseContext::MakePayload(std::string&& encoded, std::string mime, std::string& error, std::stop_token stop)
{
	if (!payload_retirement)
		return uam::native_assets::MakeInlinePayload(std::move(encoded), std::move(mime), payload_budget, error, stop);
	const std::lock_guard lock(mutex_);
	if (payload_retirement->count_ == payload_retirement->originals_.size())
	{
		error = "Gemini native history operation exceeds 256 retained originals.";
		if (retirement_error_.empty()) retirement_error_ = error;
		return {};
	}
	// The slot exists before acquiring the original. Assignment and aliasing
	// shared_ptr construction are nonthrowing; later mapping failures leave the
	// raw handle here for worker disposal, never in a caller-owned temporary.
	auto& original = payload_retirement->originals_[payload_retirement->count_];
	original = uam::native_assets::MakeInlinePayload(std::move(encoded), std::move(mime), payload_budget, error, stop);
	if (!original)
	{
		if (retirement_error_.empty()) retirement_error_ = error;
		return {};
	}
	++payload_retirement->count_;
	return {payload_retirement, original.get()};
}

std::string GeminiHistoryParseContext::RetirementError()
{
	const std::lock_guard lock(mutex_);
	return retirement_error_;
}

bool GeminiHistoryParseContext::AdmitMetadata(std::size_t bytes, std::string& error)
{
	const std::lock_guard lock(mutex_);
	const auto fail = [&](const char* reason)
	{
		error = reason;
		if (payload_retirement && retirement_error_.empty()) retirement_error_ = error;
		return false;
	};
	if (cancel.stop_requested()) return fail("Gemini native history mapping cancelled.");
	if (attachments_ >= kMaxAttachments) return fail("Gemini native history operation exceeds 256 attachment admissions.");
	if (bytes > kMaxMetadataBytes - metadata_bytes_) return fail("Gemini native history operation exceeds 1 MiB attachment metadata.");
	++attachments_;
	metadata_bytes_ += bytes;
	return true;
}

namespace
{
	GeminiHistoryParseResult MappingFailure(std::string error, std::stop_token cancel)
	{
		return {cancel.stop_requested() ? GeminiHistoryParseStatus::Cancelled : GeminiHistoryParseStatus::UnsupportedOrInvalid,
		    std::nullopt, cancel.stop_requested() ? "Gemini native history mapping cancelled." : std::move(error)};
	}

	enum class MappingEligibility { Conversation, LegacyRows };

	GeminiHistoryParseResult ParseOwnedDocument(nlohmann::json document, const ProviderProfile& provider,
	    const GeminiJsonHistoryStoreOptions& options, std::stop_token external_cancel = {}, MappingEligibility eligibility = MappingEligibility::Conversation)
	{
		auto context = options.context ? options.context : std::make_shared<GeminiHistoryParseContext>();
		std::stop_source combined;
		std::stop_callback external(external_cancel, [&] { combined.request_stop(); });
		std::stop_callback contextual(context->cancel, [&] { combined.request_stop(); });
		MappingState state{context, combined.get_token(), {}, 0};
		try
		{
			if (!state.Check()) return MappingFailure(state.error, state.cancel);
			if (!document.is_object()) return MappingFailure("Gemini native history is not a document.", state.cancel);
			auto root = ConvertJournalValue(document, state);
			if (!state.error.empty()) return MappingFailure(state.error, state.cancel);
			auto chat = MapGeminiDocument(root, provider, options, state);
			if (!state.error.empty() || state.cancel.stop_requested()) return MappingFailure(state.error, state.cancel);
			// Only after all current rows validate may a rich result classify its
			// projection. A cap hiding every eligible row is incomplete, never empty.
			if (eligibility == MappingEligibility::Conversation && state.source_has_conversation &&
			    (!chat || !uam::HasImportedConversationContent(chat->messages)))
				return MappingFailure("Gemini native history projection omits eligible conversation; retry without the message cap.", state.cancel);
			// Compatibility readers preserve meaningful system rows. Rich mutation
			// results retain the shared conversation-eligibility agreement.
			if (!chat || (eligibility == MappingEligibility::Conversation && !state.source_has_conversation))
				return {GeminiHistoryParseStatus::ConfirmedEmpty, std::nullopt, {}};
			return {GeminiHistoryParseStatus::Content, std::move(chat), {}};
		}
		catch (...) { return MappingFailure("Gemini native history could not be mapped completely within its supported bounds.", state.cancel); }
	}
}

GeminiHistoryParseResult GeminiJsonHistoryStore::ParseDocumentResult(nlohmann::json document, const ProviderProfile& provider, const GeminiJsonHistoryStoreOptions& options)
{
	return ParseOwnedDocument(std::move(document), provider, options);
}

namespace
{
	GeminiHistoryParseResult ParseHistoryFile(const std::filesystem::path& file_path, const ProviderProfile& provider,
		const GeminiJsonHistoryStoreOptions& options, std::stop_token stop_token, MappingEligibility eligibility)
	{
		auto context = options.context ? options.context : std::make_shared<GeminiHistoryParseContext>();
		GeminiJsonHistoryStoreOptions effective = options;
		effective.context = context;
		std::stop_source combined;
		std::stop_callback external(stop_token, [&] { combined.request_stop(); });
		std::stop_callback contextual(context->cancel, [&] { combined.request_stop(); });
		const auto cancel = combined.get_token();
		if (cancel.stop_requested()) return MappingFailure({}, cancel);
		try
		{
			auto document = uam::gemini::ReadHistoryDocumentFile(file_path, options.max_file_bytes, true, cancel);
			if (!document || cancel.stop_requested()) return MappingFailure("Gemini native history is unavailable, malformed or exceeds its read bound.", cancel);
			return ParseOwnedDocument(std::move(*document), provider, effective, cancel, eligibility);
		}
		catch (...) { return MappingFailure("Gemini native history could not be read completely.", cancel); }
	}

	GeminiHistoryParseResult ParseHistoryText(std::string_view file_text, const ProviderProfile& provider,
		const GeminiJsonHistoryStoreOptions& options, MappingEligibility eligibility)
	{
		auto context = options.context ? options.context : std::make_shared<GeminiHistoryParseContext>();
		GeminiJsonHistoryStoreOptions effective = options;
		effective.context = context;
		if (context->cancel.stop_requested()) return MappingFailure({}, context->cancel);
		if (file_text.empty() || (options.max_file_bytes > 0 && file_text.size() > options.max_file_bytes))
			return MappingFailure("Gemini native history text is unavailable or exceeds its read bound.", context->cancel);
		try
		{
			// Parse into an owned tree once. Do not parse a second binary-bearing
			// JsonValue tree or serialize a document just to remap it.
			auto document = nlohmann::json::parse(file_text.begin(), file_text.end(), [&](int depth, nlohmann::json::parse_event_t, nlohmann::json&)
			{
				if (depth > 64 || context->cancel.stop_requested()) throw std::runtime_error("Gemini bounded parse interrupted");
				return true;
			}, false);
			if (document.is_object()) return ParseOwnedDocument(std::move(document), provider, effective, {}, eligibility);
			document = nullptr;
			auto folded = uam::gemini::FoldHistoryJournal(file_text);
			if (!folded || context->cancel.stop_requested()) return MappingFailure("Gemini native history journal is malformed.", context->cancel);
			return ParseOwnedDocument(std::move(*folded), provider, effective, {}, eligibility);
		}
		catch (...) { return MappingFailure("Gemini native history text could not be mapped completely within its supported bounds.", context->cancel); }
	}

}

GeminiHistoryParseResult GeminiJsonHistoryStore::ParseFileResult(const std::filesystem::path& file_path, const ProviderProfile& provider,
	const GeminiJsonHistoryStoreOptions& options, std::stop_token stop_token)
{
	return ParseHistoryFile(file_path, provider, options, stop_token, MappingEligibility::Conversation);
}

GeminiHistoryParseResult GeminiJsonHistoryStore::ParseTextResult(std::string_view file_text, const ProviderProfile& provider, const GeminiJsonHistoryStoreOptions& options)
{
	return ParseHistoryText(file_text, provider, options, MappingEligibility::Conversation);
}

std::optional<ChatSession> GeminiJsonHistoryStore::ParseDocument(nlohmann::json document, const ProviderProfile& provider, const GeminiJsonHistoryStoreOptions& options)
{
	return ParseOwnedDocument(std::move(document), provider, options, {}, MappingEligibility::LegacyRows).chat;
}

std::optional<ChatSession> GeminiJsonHistoryStore::ParseFile(const std::filesystem::path& file_path, const ProviderProfile& provider, const GeminiJsonHistoryStoreOptions& options, std::stop_token stop_token)
{
	return ParseHistoryFile(file_path, provider, options, stop_token, MappingEligibility::LegacyRows).chat;
}

std::optional<ChatSession> GeminiJsonHistoryStore::ParseText(std::string_view file_text, const ProviderProfile& provider, const GeminiJsonHistoryStoreOptions& options)
{
	return ParseHistoryText(file_text, provider, options, MappingEligibility::LegacyRows).chat;
}

std::vector<ChatSession> GeminiJsonHistoryStore::Load(const std::filesystem::path& chats_dir, const ProviderProfile& provider, const GeminiJsonHistoryStoreOptions& options, std::stop_token stop_token, const uam::gemini::HistoryProject* selected_project)
{
	std::vector<ChatSession> chats;
	GeminiJsonHistoryStoreOptions effective = options;
	if (!effective.context) effective.context = std::make_shared<GeminiHistoryParseContext>();

	if (chats_dir.empty() || !uam::paths::IsDirectoryNoThrow(chats_dir))
	{
		return chats;
	}

	std::error_code ec;
	fs::path enumeration_dir = chats_dir;
	if (selected_project)
	{
		const auto owned = uam::gemini::HistoryDirectoryInProject(selected_project->root, selected_project->relative / "chats");
		const auto supplied = fs::canonical(chats_dir, ec);
		if (!owned || ec || !uam::gemini::SameHistoryWorkspace(uam::paths::Utf8PathString(*owned), uam::paths::Utf8PathString(supplied))) return chats;
		enumeration_dir = *owned;
	}

	for (fs::directory_iterator it(enumeration_dir, ec), end; !ec && it != end; it.increment(ec))
	{
		const fs::directory_entry& item = *it;
		if (stop_token.stop_requested())
		{
			break;
		}

		if (!uam::paths::IsRegularFileWithExtensionNoThrow(item, ".json") && !uam::paths::IsRegularFileWithExtensionNoThrow(item, ".jsonl"))
		{
			continue;
		}

		fs::path file = item.path();
		if (selected_project)
		{
			const auto owned = uam::gemini::HistoryFileInProject(selected_project->root, selected_project->relative / "chats" / item.path().filename());
			if (!owned) continue;
			file = *owned;
		}

		// Native migration keeps the old JSON file beside its current JSONL successor.
		fs::path current_sibling = file;
		current_sibling += "l";
		if (item.path().extension() == ".json" && uam::paths::PathExistsNoThrow(current_sibling)) continue;

		if (options.max_file_bytes > 0)
		{
			const std::optional<std::uintmax_t> file_size = uam::paths::FileSizeNoThrow(file);
			if (file_size && *file_size > options.max_file_bytes)
			{
				continue;
			}
		}

		std::optional<ChatSession> parsed = ParseFile(file, provider, effective, stop_token);

		if (parsed)
		{
			if (selected_project) parsed->workspace_directory = selected_project->workspace;
			chats.push_back(std::move(*parsed));
		}
	}

	return stop_token.stop_requested() ? std::vector<ChatSession>{} : chats;
}

bool GeminiJsonHistoryStore::SaveFile(const std::filesystem::path& file_path, const ChatSession& chat)
{
	JsonValue root = uam::json::Object();

	const std::string session_id = uam::strings::Trim(uam::strings::NonEmptyOrFallback(chat.native_session_id, chat.id));

	if (session_id.empty())
	{
		return false;
	}

	uam::json::SetString(root, "sessionId", session_id);
	uam::json::SetString(root, "startTime", chat.created_at.empty() ? uam::time::TimestampNow() : chat.created_at);
	uam::json::SetString(root, "lastUpdated", chat.updated_at.empty() ? uam::time::TimestampNow() : chat.updated_at);

	JsonValue messages_arr = uam::json::Array();

	for (const Message& msg : chat.messages)
	{
		JsonValue msg_obj = uam::json::Object();
			uam::json::SetString(msg_obj, "type", msg.role == MessageRole::User ? "user" : msg.role == MessageRole::System ? "info" : "model");
		uam::json::SetString(msg_obj, "timestamp", msg.created_at.empty() ? uam::time::TimestampNow() : msg.created_at);
		uam::json::SetString(msg_obj, "content", msg.content);
		uam::json::PushValue(messages_arr, std::move(msg_obj));
	}

	uam::json::SetValue(root, "messages", std::move(messages_arr));

	return uam::io::WriteTextFile(file_path, SerializeJson(root));
}

std::vector<ChatSession> LoadGeminiJsonHistoryForRuntime(const std::filesystem::path& chats_dir, const ProviderProfile& profile, const ProviderRuntimeHistoryLoadOptions& options, std::stop_token stop_token, const std::shared_ptr<GeminiHistoryParseContext>& context)
{
	GeminiJsonHistoryStoreOptions native_options;
	native_options.max_file_bytes = options.native_max_file_bytes;
	native_options.max_messages = options.native_max_messages;
	native_options.context = context;
	if (chats_dir.empty() || stop_token.stop_requested()) return {};
	auto directory = uam::paths::AbsolutePathNoThrow(chats_dir);
	if (directory.filename().empty()) directory = directory.parent_path();
	const auto supplied_root = directory.parent_path().parent_path().parent_path();
	const auto provider_root = uam::gemini::HistoryHomePath();
	const auto same_root = [&](const fs::path& first, const fs::path& second)
	{
		return uam::gemini::SameHistoryWorkspace(uam::paths::Utf8PathString(first), uam::paths::Utf8PathString(second));
	};
	// Resolve only the provider root here, retaining the selected lexical project
	// even when a chats directory or individual file aliases a different project.
	const bool provider_owned = same_root(directory.filename(), "chats") && same_root(directory.parent_path().parent_path().filename(), "tmp") &&
	    (same_root(supplied_root, provider_root) || same_root(uam::paths::NormalizeExistingOrAbsolutePath(supplied_root), uam::paths::NormalizeExistingOrAbsolutePath(provider_root)));
	if (provider_owned)
	{
		const auto discovery = uam::gemini::DiscoverHistoryProjectsWithDiagnostics(stop_token);
		if (discovery.Fatal() || stop_token.stop_requested()) return {};
		const auto relative = fs::path("tmp") / directory.parent_path().filename();
		for (const auto& project : discovery.projects)
			if (project.relative == relative && discovery.CanReadWorkspace(project.workspace))
				return GeminiJsonHistoryStore::Load(directory, profile, native_options, stop_token, &project);
		return {};
	}
	// Custom directories remain an explicit caller-supplied source, independent
	// of the native profile registry and its project fences.
	return GeminiJsonHistoryStore::Load(chats_dir, profile, native_options, stop_token);
}

std::vector<ProviderChatSource> DiscoverGeminiTmpChatSources(std::string* error_out, std::stop_token stop_token)
{
	std::vector<ProviderChatSource> sources;
	for (const auto& project : uam::gemini::DiscoverHistoryProjects(error_out, stop_token))
	{
		if (stop_token.stop_requested()) return {};
		const auto chats = project.root / project.relative / "chats";
		if (!uam::paths::IsDirectoryNoThrow(chats)) continue;
		if (!uam::gemini::HistoryDirectoryInProject(project.root, project.relative / "chats"))
		{
			if (error_out && error_out->empty()) *error_out = "Gemini history directory crosses its selected project ownership.";
			continue;
		}
		ProviderChatSource source;
		source.folder_title = uam::paths::Utf8PathString(project.relative.filename());
		source.folder_directory = project.workspace;
		source.chats_dir = chats;
		sources.push_back(std::move(source));
	}
	return stop_token.stop_requested() ? std::vector<ProviderChatSource>{} : sources;
}

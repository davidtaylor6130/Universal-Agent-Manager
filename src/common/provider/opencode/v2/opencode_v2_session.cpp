#include "common/provider/opencode_v2_session.h"
#include <stdexcept>
#include <utility>

namespace uam::opencode_v2
{
namespace
{
Json Rules(bool read_only)
{
	// Remove prior session allowances without overriding native agent denials.
	// Force-ask is imposed after saved grants by the process-local hook.
	Json rules = Json::array({{{"action", "subagent"}, {"resource", "*"}, {"effect", "deny"}}, {{"action", "task"}, {"resource", "*"}, {"effect", "deny"}}});
	if (read_only)
		rules.push_back({{"action", "edit"}, {"resource", "*"}, {"effect", "deny"}});
	return rules;
}
Json Data(const Json& response)
{
	if (!response.is_object() || !response.contains("data")) throw std::runtime_error("OpenCode V2 returned an invalid native response.");
	return response.at("data");
}
bool Id(const std::string& id)
{
	return !id.empty() && id.size() <= 512 && id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos;
}
std::string Kind(const std::string& name)
{
	if (name == "shell" || name == "bash") return "execute";
	if (name == "edit" || name == "write" || name == "patch") return "edit";
	if (name == "read") return "read";
	if (name == "glob" || name == "grep" || name == "websearch") return "search";
	if (name == "webfetch") return "fetch";
	if (name == "task") return "other";
	return "other";
}
}

std::string Encode(std::string_view value)
{
	std::string result;
	constexpr char digits[] = "0123456789ABCDEF";
	for (unsigned char c : value)
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') result += static_cast<char>(c);
		else { result += '%'; result += digits[c >> 4]; result += digits[c & 15]; }
	return result;
}

std::string ToolKey(std::string_view message_id, std::string_view tool_id)
{
	if (message_id.empty() || message_id.size() > 512 || tool_id.empty() || tool_id.size() > 512) throw std::runtime_error("OpenCode V2 returned an invalid native tool identity.");
	return "v2:" + Encode(message_id) + ":" + Encode(tool_id);
}

Session::Session(Query query, PublishPolicy publish_policy, Emit emit)
	: query_(std::move(query)), publish_policy_(std::move(publish_policy)), emit_(std::move(emit)) {}

void Session::Reply(const Json& id, const Json& result)
{
	if (!id.is_null()) emit_({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}
void Session::Update(Json update)
{
	emit_({{"jsonrpc", "2.0"}, {"method", "session/update"}, {"params", {{"sessionId", session_id_}, {"update", std::move(update)}}}});
}
std::string Session::Route(const std::string& suffix) const
{
	if (!Id(session_id_)) throw std::runtime_error("OpenCode V2 has no owned native session.");
	return "/api/session/" + Encode(session_id_) + suffix;
}
void Session::RequireEmptyInbox()
{
	const Json inbox = Data(query_("GET", Route("/inbox"), nullptr));
	if (!inbox.is_array() || !inbox.empty()) throw std::runtime_error("OpenCode V2 has unverified or pre-existing queued native work. Exact resume/input remains held for explicit recovery.");
}
void Session::Policy(const Json& params)
{
	const Json meta = params.value("_meta", Json::object()).value("uamPolicy", Json{});
	if (!meta.is_object() || !meta.contains("readOnly") || !meta["readOnly"].is_boolean())
		throw std::runtime_error("OpenCode V2 input has no captured UAM permission policy.");
	read_only_ = meta.at("readOnly").get<bool>() || mode_ == "plan";
	Json capture = meta; capture["effectiveReadOnly"] = read_only_;
	if (capture != policy_capture_) session_grants_.clear();
	policy_capture_ = std::move(capture);
	publish_policy_(session_id_, cwd_, read_only_);
	if (session_id_.empty()) return;
	const Json rules = Rules(read_only_);
	query_("PATCH", Route(), {{"permissions", rules}});
	const Json observed = Data(query_("GET", Route(), nullptr));
	if (observed.value("id", "") != session_id_ || observed.value("location", Json::object()).value("directory", "") != cwd_ || observed.value("permissions", Json{}) != rules)
		throw std::runtime_error("OpenCode V2 did not confirm the exact session, workspace and UAM permission rules.");
}

Json Session::Options()
{
	Json models = Json::array();
	model_refs_.clear();
	model_context_.clear();
	const auto bind = [&](const std::string& key, Json ref)
	{
		const auto existing = model_refs_.find(key);
		if (existing != model_refs_.end() && existing->second != ref) throw std::runtime_error("OpenCode V2 advertised an ambiguous model identity.");
		model_refs_[key] = std::move(ref);
	};
	if (model_.empty())
	{
		const Json fallback = Data(query_("GET", "/api/model/default?location[directory]=" + Encode(cwd_), nullptr));
		if (fallback.is_object()) model_ = fallback.at("providerID").get<std::string>() + "/" + fallback.at("id").get<std::string>();
	}
	for (const auto& model : Data(query_("GET", "/api/model?location[directory]=" + Encode(cwd_), nullptr)))
	{
		if (!model.value("enabled", true)) continue;
		const auto provider = model.value("providerID", ""), id = model.value("id", "");
		if (provider.empty() || id.empty()) continue;
		models.push_back({{"modelId", provider + "/" + id}, {"name", model.value("name", id)}});
		bind(provider + "/" + id, {{"providerID", provider}, {"id", id}});
		const Json context = model.value("limit", Json::object()).value("context", Json{});
		if (context.is_number_integer() && context.get<double>() > 0 && context.get<double>() <= 9007199254740991.0)
		{
			model_context_[provider + "/" + id] = context.get<std::int64_t>();
			models.back()["contextWindowTokens"] = context;
		}
		const Json output = model.value("limit", Json::object()).value("output", Json{});
		if (output.is_number_integer() && output.get<double>() > 0 && output.get<double>() <= 9007199254740991.0) models.back()["maxOutputTokens"] = output;
		const Json base_advertisement = models.back();
		for (const auto& variant : model.value("variants", Json::array()))
			if (variant.is_object() && !variant.value("id", "").empty())
			{
				const auto key = provider + "/" + id + "/" + variant.at("id").get<std::string>();
				Json advertised = base_advertisement; advertised["modelId"] = key; advertised["name"] = model.value("name", id) + " / " + variant.at("id").get<std::string>();
				models.push_back(std::move(advertised));
				bind(key, {{"providerID", provider}, {"id", id}, {"variant", variant.at("id")}});
				bind(provider + "/" + id + "#" + variant.at("id").get<std::string>(), model_refs_.at(key));
			}
	}
	Json options = Json::array();
	const auto current = model_refs_.find(model_);
	const Json selected = current == model_refs_.end() ? Json::object() : current->second;
	const std::string base = selected.empty() ? model_ : selected.at("providerID").get<std::string>() + "/" + selected.at("id").get<std::string>();
	Json efforts = Json::array({{{"value", "default"}, {"name", "Default"}}});
	for (const auto& [key, ref] : model_refs_)
		if (key.starts_with(base + "/") && ref.contains("variant")) efforts.push_back({{"value", ref.at("variant")}, {"name", ref.at("variant")}});
	if (efforts.size() > 1) options.push_back({{"id", "effort"}, {"name", "Effort"}, {"category", "thought_level"}, {"type", "select"}, {"currentValue", selected.value("variant", "default")}, {"options", efforts}});
	return {{"models", {{"currentModelId", model_}, {"availableModels", models}}}, {"configOptions", options},
		{"modes", {{"currentModeId", mode_}, {"availableModes", Json::array({{{"id", "build"}, {"name", "Build"}}, {{"id", "plan"}, {"name", "Plan"}}})}}}};
}

Json Session::Setup(const Json& params, bool load)
{
	if (!session_id_.empty()) throw std::runtime_error("OpenCode V2 bridge already owns a session.");
	cwd_ = params.at("cwd").get<std::string>();
	if (cwd_.empty()) throw std::runtime_error("OpenCode V2 requires an exact workspace.");
	Policy(params); // inactive root denies native work until binding is complete
	query_("GET", "/api/location?location[directory]=" + Encode(cwd_), nullptr);
	// Resolve the plugin service explicitly; location lookup alone is not a
	// registration receipt. The transport still requires the nonce-bound proof.
	query_("GET", "/api/plugin?location[directory]=" + Encode(cwd_), nullptr);
	for (const auto& server : params.value("mcpServers", Json::array()))
	{
		const std::string name = server.at("name").get<std::string>();
		if (name.empty() || name.size() > 512 || name.find_first_of("\r\n\0", 0, 3) != std::string::npos || name == "opencode-progressive-mcp") throw std::runtime_error("Invalid OpenCode V2 MCP definition.");
		Json config;
		if (server.value("type", "stdio") == "stdio")
		{
			Json command = Json::array({server.at("command")});
			for (const auto& arg : server.value("args", Json::array())) command.push_back(arg);
			Json env = Json::object();
			for (const auto& entry : server.value("env", Json::array())) env[entry.at("name").get<std::string>()] = entry.at("value");
			config = {{"type", "local"}, {"command", command}, {"environment", env}};
		}
		else
		{
			Json headers = Json::object();
			for (const auto& entry : server.value("headers", Json::array())) headers[entry.at("name").get<std::string>()] = entry.at("value");
			config = {{"type", "remote"}, {"url", server.at("url")}, {"headers", headers}};
		}
		query_("PUT", "/api/experimental/mcp/" + Encode(name) + "?location[directory]=" + Encode(cwd_), {{"config", config}});
	}
	Json info;
	if (load)
	{
		const auto id = params.at("sessionId").get<std::string>();
		if (!Id(id)) throw std::runtime_error("Invalid OpenCode V2 resume identity.");
		info = Data(query_("GET", "/api/session/" + Encode(id), nullptr));
		if (info.value("id", "") != id || info.value("location", Json::object()).value("directory", "") != cwd_)
			throw std::runtime_error("OpenCode V2 resume identity or workspace does not match.");
	}
	else info = Data(query_("POST", "/api/session", {{"location", {{"directory", cwd_}}}, {"agent", read_only_ ? "plan" : "build"}, {"permissions", Rules(read_only_)}}));
	session_id_ = info.at("id").get<std::string>();
	if (!Id(session_id_) || info.value("location", Json::object()).value("directory", "") != cwd_) throw std::runtime_error("OpenCode V2 returned an invalid session binding.");
	const auto native_model = info.value("model", Json::object());
	if (native_model.is_object() && native_model.contains("id")) model_ = native_model.value("providerID", "") + "/" + native_model.value("id", "");
	if (native_model.is_object() && native_model.contains("variant")) model_ += "/" + native_model.at("variant").get<std::string>();
	mode_ = info.value("agent", "build");
	if (mode_ != "build" && mode_ != "plan") throw std::runtime_error("OpenCode V2 resume uses an unverified native agent policy.");
	if (load) RequireEmptyInbox(); // never rewrite policy for pre-existing queued work
	Policy(params); // replace/read back old session grants before readiness
	Json result = Options();
	result["sessionId"] = session_id_;
	ready_ = true;
	return result;
}

void Session::SelectModel(const std::string& id)
{
	const auto found = model_refs_.find(id);
	if (found == model_refs_.end()) throw std::runtime_error("OpenCode V2 model is not in this process's native catalogue.");
	const Json model = found->second;
	query_("POST", Route("/model"), {{"model", model}});
	const Json info = Data(query_("GET", Route(), nullptr));
	if (info.value("model", Json{}) != model) throw std::runtime_error("OpenCode V2 did not confirm the selected model.");
	model_ = model.at("providerID").get<std::string>() + "/" + model.at("id").get<std::string>() + (model.contains("variant") ? "/" + model.at("variant").get<std::string>() : "");
}

void Session::Handle(const Json& message)
{
	const Json id = message.value("id", Json{});
	try
	{
		if (disconnected_) throw std::runtime_error("OpenCode V2 event transport lost ownership. Restart explicitly before sending input.");
		if (!message.contains("method"))
		{
			if (!id.is_string()) throw std::runtime_error("Invalid OpenCode V2 approval identity.");
			const auto form = forms_.find(id.get<std::string>());
			if (form != forms_.end())
			{
				const auto route = Route("/form/" + Encode(form->second.at("id").get<std::string>()));
				const Json replies = message.value("result", Json::object()).value("answers", Json::object());
				if (cancel_requested_ || replies.empty() || message.contains("error")) query_("DELETE", route, nullptr);
				else
				{
					Json answer = Json::object();
					for (const auto& field : form->second.at("fields"))
					{
						const auto key = field.at("key").get<std::string>();
						if (!replies.contains(key)) continue;
						Json values = replies.at(key).at("answers");
						for (auto& value : values)
							for (const auto& option : field.value("options", Json::array()))
								if (value == option.at("label")) { value = option.at("value"); break; }
						answer[key] = field.at("type") == "multiselect" ? values : values.at(0);
					}
					query_("POST", route + "/reply", {{"answer", answer}});
				}
				forms_.erase(form); return;
			}
			const auto found = permissions_.find(id.get<std::string>());
			if (found == permissions_.end()) throw std::runtime_error("Unknown OpenCode V2 approval request.");
			const Json outcome = message.value("result", Json::object()).value("outcome", Json::object());
			const auto choice = outcome.value("optionId", "reject");
			const bool allow = !cancel_requested_ && outcome.value("outcome", "") == "selected" && (choice == "once" || choice == "session");
			query_("POST", Route("/permission/" + Encode(found->second.at("id").get<std::string>()) + "/reply"), {{"decision", allow ? "once" : "reject"}});
			if (allow && choice == "session" && found->second.contains("uamGrantKey")) session_grants_.insert(found->second.at("uamGrantKey").get<std::string>());
			permissions_.erase(found);
			return;
		}
		const auto method = message.at("method").get<std::string>();
		const Json params = message.value("params", Json::object());
		if (method == "initialize")
		{
			Reply(id, {{"protocolVersion", 1}, {"agentInfo", {{"name", "OpenCode V2 native API"}, {"version", "2.1.2"}}}, {"agentCapabilities", {{"loadSession", true}, {"mcpCapabilities", {{"http", true}, {"sse", true}}}}}});
			return;
		}
		if (method == "session/new" || method == "session/load" || method == "session/resume") { Reply(id, Setup(params, method != "session/new")); return; }
		if (!ready_ || params.value("sessionId", "") != session_id_ || session_id_.empty()) throw std::runtime_error("OpenCode V2 request does not own a ready session.");
		if (method == "session/prompt")
		{
			if (!prompt_id_.is_null()) throw std::runtime_error("OpenCode V2 already has an admitted turn.");
			RequireEmptyInbox();
			Policy(params);
			RequireEmptyInbox();
			if (!model_refs_.contains(model_)) throw std::runtime_error("The current OpenCode V2 model is not available in the owned native catalogue.");
			std::string text;
			for (const auto& part : params.at("prompt"))
			{
				if (part.value("type", "") != "text") throw std::runtime_error("OpenCode V2 currently requires captured text/file references; image Retry remains held.");
				if (!text.empty()) text += '\n';
				text += part.at("text").get<std::string>();
			}
			tools_.clear(); seen_steps_.clear(); usage_ = Json::object(); usage_invalid_ = false; last_step_used_ = 0; finish_.clear();
			prompt_id_ = id; prompt_started_ = false;
			const Json admitted = Data(query_("POST", Route("/prompt"), {{"text", text}}));
			prompt_inbox_ = admitted.at("id").get<std::string>();
			if (!Id(prompt_inbox_) || admitted.value("sessionID", "") != session_id_) throw std::runtime_error("OpenCode V2 returned an ambiguous prompt admission.");
			return; // admission is not completion; wait for the owned execution event
		}
		if (method == "session/cancel")
		{
			if (prompt_id_.is_null()) return;
			if (cancel_requested_) return; // do not extend the original cancel budget
			cancel_requested_ = true;
			cancel_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			if (!prompt_started_)
			{
				// V2 DELETE is a no-op for unavailable input. Neither success nor
				// failure proves delivery/cancellation; wait for the exact SSE fact.
				try { query_("DELETE", Route("/inbox/" + Encode(prompt_inbox_)), nullptr); } catch (...) {}
			}
			else InterruptCancelledTurn();
			return;
		}
		if (!prompt_id_.is_null()) throw std::runtime_error("OpenCode V2 settings cannot change during an admitted turn.");
		if (method == "session/set_model")
		{
			SelectModel(params.at("modelId").get<std::string>());
			const auto options = Options().at("configOptions");
			// Shared result handling adopts the confirmed model before its options;
			// an earlier notification would attach these options to the old model.
			Reply(id, {{"configOptions", options}}); return;
		}
		if (method == "session/set_config_option" && params.value("configId", "") == "effort")
		{
			const auto effort = params.at("value").get<std::string>();
			const Json current = model_refs_.at(model_);
			const auto base = current.at("providerID").get<std::string>() + "/" + current.at("id").get<std::string>();
			SelectModel(base + (effort == "default" ? "" : "/" + effort));
			Reply(id, {{"configOptions", Options().at("configOptions")}}); return;
		}
		if (method == "session/set_mode")
		{
			const auto requested = params.at("modeId").get<std::string>();
			const auto mode = requested == "acceptEdits" ? std::string("build") : requested;
			if (mode != "build" && mode != "plan") throw std::runtime_error("Unsupported OpenCode V2 native agent.");
			query_("POST", Route("/agent"), {{"agent", mode}});
			if (Data(query_("GET", Route(), nullptr)).value("agent", "") != mode) throw std::runtime_error("OpenCode V2 did not confirm its native agent.");
			mode_ = mode;
			Policy({{"_meta", {{"uamPolicy", policy_capture_}}}});
			Reply(id, Json::object()); return;
		}
		throw std::runtime_error("Unsupported OpenCode V2 native request: " + method);
	}
	catch (const std::exception& error)
	{
		if (id.is_string() && forms_.contains(id.get<std::string>()))
		{
			try { query_("DELETE", Route("/form/" + Encode(forms_.at(id.get<std::string>()).at("id").get<std::string>())), nullptr); } catch (...) {}
			forms_.erase(id.get<std::string>()); Disconnected("OpenCode V2 did not confirm the question reply. This turn is held without automatic replay."); return;
		}
		if (id.is_string() && permissions_.contains(id.get<std::string>()))
		{
			Disconnected("OpenCode V2 did not confirm the approval reply. This turn is held without automatic replay."); return;
		}
		if (message.value("method", "") == "session/cancel")
		{
			Disconnected("OpenCode V2 did not confirm cancellation. This turn is held until explicit recovery; input is not replayed."); return;
		}
		if (!prompt_id_.is_null() && id == prompt_id_)
		{
			// The POST may already own this input. Preserve its correlation until
			// the shared consumer receives the explicit held/no-replay failure.
			Disconnected(std::string("OpenCode V2 did not confirm prompt admission: ") + error.what() +
			    ". Input is held and is never replayed automatically.");
			return;
		}
		if (!id.is_null()) emit_({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32603}, {"message", error.what()}}}});
	}
}

void Session::Finish(const std::string& reason)
{
	if (prompt_id_.is_null()) return;
	Json result{{"stopReason", reason}};
	if (!usage_.empty()) result["usage"] = usage_;
	const auto selected = model_refs_.find(model_);
	if (!usage_.empty() && last_step_used_ > 0 && selected != model_refs_.end())
	{
		const auto base = selected->second.at("providerID").get<std::string>() + "/" + selected->second.at("id").get<std::string>();
		const auto window = model_context_.find(base);
		if (window != model_context_.end() && last_step_used_ <= window->second)
			Update({{"sessionUpdate", "usage_update"}, {"used", last_step_used_}, {"size", window->second}});
	}
	Reply(prompt_id_, result);
	prompt_id_ = nullptr; prompt_started_ = false; prompt_inbox_.clear();
	permissions_.clear(); forms_.clear();
	cancel_requested_ = false; cancel_interrupt_sent_ = false;
}

void Session::Event(const Json& event)
try
{
	if (disconnected_ || !event.is_object()) return;
	const auto type = event.value("type", "");
	const Json data = event.value("data", Json::object());
	const auto owner = type == "form.created" ? data.value("form", Json::object()).value("sessionID", "") : data.value("sessionID", "");
	if (session_id_.empty() || owner != session_id_) return;
	const auto event_id = event.value("id", "");
	if (!event_id.empty() && !seen_events_.insert(event_id).second) return;
	if (!event_id.empty()) seen_event_order_.push_back(event_id);
	if (seen_event_order_.size() > 4096) { seen_events_.erase(seen_event_order_.front()); seen_event_order_.pop_front(); }
	if (type == "form.created")
	{
		const Json form = data.value("form", Json::object());
		const auto id = form.at("id").get<std::string>();
		if (!Id(id)) { Disconnected(); return; }
		bool supported = !prompt_id_.is_null() && prompt_started_ && !cancel_requested_ && forms_.empty();
		std::unordered_set<std::string> keys;
		for (const auto& field : form.at("fields"))
		{
			if ((field.at("type") != "string" && field.at("type") != "multiselect") || field.contains("when") || field.value("hidden", false) || !keys.insert(field.at("key").get<std::string>()).second) supported = false;
			std::unordered_set<std::string> labels;
			for (const auto& option : field.value("options", Json::array())) if (!labels.insert(option.at("label").get<std::string>()).second) supported = false;
		}
		if (!supported) { query_("DELETE", Route("/form/" + Encode(id)), nullptr); return; }
		forms_["v2form:" + id] = form;
		emit_({{"jsonrpc", "2.0"}, {"id", "v2form:" + id}, {"method", "uam/opencode/question"}, {"params", {{"sessionId", session_id_}, {"form", form}}}});
		return;
	}
	if (type == "session.inbox.cancelled" && !prompt_id_.is_null() && data.value("inboxID", "") == prompt_inbox_) { Finish("cancelled"); return; }
	if (type == "session.inbox.delivered")
	{
		if (!prompt_id_.is_null() && data.value("inboxID", "") == prompt_inbox_)
		{
			prompt_started_ = true;
			if (cancel_requested_ && !cancel_interrupt_sent_) InterruptCancelledTurn();
		}
		return;
	}
	if (type == "permission.asked")
	{
		const std::string request = data.at("id").get<std::string>();
		if (!data.at("action").is_string() || data.at("action").get_ref<const std::string&>().empty() || !data.at("resources").is_array())
			throw std::runtime_error("Invalid native permission request.");
		for (const auto& resource : data.at("resources")) if (!resource.is_string()) throw std::runtime_error("Invalid native permission resource.");
		if (!Id(request)) { Disconnected(); return; }
		if (prompt_id_.is_null() || !prompt_started_ || cancel_requested_ || data.value("action", "") == "subagent" || data.value("action", "") == "task" || (read_only_ && data.value("action", "") == "edit"))
		{
			query_("POST", Route("/permission/" + Encode(request) + "/reply"), {{"decision", "reject"}}); return;
		}
		const auto source = data.value("source", Json::object());
		const auto message_id = source.value("messageID", ""), source_id = source.value("id", "");
		const std::string tool_id = !message_id.empty() && !source_id.empty() ? ToolKey(message_id, source_id) : "v2:permission:" + request;
		Json tool = tools_.contains(tool_id) ? tools_.at(tool_id) : Json{{"toolCallId", tool_id}, {"title", data.value("action", "Native action")}, {"kind", Kind(data.value("action", ""))}};
		if (data.value("action", "") == "external_directory")
		{
			// Access beyond the workspace is a separate decision, even when the
			// requesting tool is an edit/read that otherwise has automatic handling.
			tool["kind"] = "other"; tool["title"] = "External directory access";
		}
		tool["status"] = "pending";
		Json pending = data;
		if (tool.contains("rawInput"))
		{
			const std::string key = Json::array({data.at("action"), data.at("resources"), tool.value("name", ""), tool.at("rawInput")}).dump();
			if (session_grants_.contains(key))
			{
				query_("POST", Route("/permission/" + Encode(request) + "/reply"), {{"decision", "once"}}); return;
			}
			pending["uamGrantKey"] = key;
		}
		permissions_["v2:" + request] = pending;
		emit_({{"jsonrpc", "2.0"}, {"id", "v2:" + request}, {"method", "session/request_permission"}, {"params", {{"sessionId", session_id_}, {"toolCall", tool}, {"options", Json::array({{{"optionId", "once"}, {"kind", "allow_once"}, {"name", "Allow once"}}, {{"optionId", "session"}, {"kind", "allow_always"}, {"name", "Allow this exact action for this session"}}, {{"optionId", "reject"}, {"kind", "reject_once"}, {"name", "Reject"}}})}}}});
		return;
	}
	if (prompt_id_.is_null() || !prompt_started_) return;
	if (type == "session.text.delta" || type == "session.reasoning.delta")
		Update({{"sessionUpdate", type == "session.text.delta" ? "agent_message_chunk" : "agent_thought_chunk"}, {"content", {{"type", "text"}, {"text", data.at("delta")}}}});
	else if (type == "session.tool.input.started" || type == "session.tool.called")
	{
		const auto id = ToolKey(data.at("assistantMessageID").get<std::string>(), data.at("id").get<std::string>());
		const auto name = data.value("name", "tool");
		Json tool = tools_.contains(id) ? tools_.at(id) : Json{{"toolCallId", id}, {"title", name}, {"name", name}, {"kind", Kind(name)}};
		tool["status"] = type == "session.tool.called" ? "in_progress" : "pending";
		if (data.contains("input")) tool["rawInput"] = data.at("input");
		tools_[id] = tool;
		tool["sessionUpdate"] = type == "session.tool.called" ? "tool_call_update" : "tool_call";
		Update(tool);
	}
	else if (type == "session.tool.success" || type == "session.tool.failed" || type == "session.tool.progress")
	{
		Json tool{{"sessionUpdate", "tool_call_update"}, {"toolCallId", ToolKey(data.at("assistantMessageID").get<std::string>(), data.at("id").get<std::string>())}, {"status", type == "session.tool.success" ? "completed" : type == "session.tool.failed" ? "failed" : "in_progress"}};
		if (data.contains("content")) tool["rawOutput"] = data.at("content");
		if (data.contains("error")) tool["rawOutput"] = data.at("error");
		if (data.contains("metadata")) tool["_meta"] = data.at("metadata");
		Update(tool);
	}
	else if (type == "session.step.ended" || type == "session.step.failed")
	{
		finish_ = data.value("finish", "");
		const auto step = data.value("assistantMessageID", event_id);
		if (step.empty() || !seen_steps_.insert(step).second) return;
		if (usage_invalid_) return;
		const auto tokens = data.value("tokens", Json::object());
		const Json cache = tokens.value("cache", Json::object());
		const Json buckets = Json::array({tokens.value("input", Json{}), tokens.value("output", Json{}), tokens.value("reasoning", Json{}), cache.value("read", Json{}), cache.value("write", Json{})});
		const char* names[]{"inputTokens", "outputTokens", "thoughtTokens", "cachedReadTokens", "cachedWriteTokens"};
		constexpr std::int64_t max = 9007199254740991LL;
		std::int64_t total = 0, step_used = 0;
		for (std::size_t i = 0; i < buckets.size(); ++i)
		{
			const auto& bucket = buckets[i];
			if (!bucket.is_number_integer() || (bucket.is_number_unsigned() && bucket.get<std::uint64_t>() > static_cast<std::uint64_t>(max))) { usage_invalid_ = true; break; }
			const auto count = bucket.get<std::int64_t>(), previous = usage_.value(names[i], std::int64_t{0});
			if (count < 0 || count > max - previous || previous + count > max - total) { usage_invalid_ = true; break; }
			usage_[names[i]] = previous + count; total += previous + count;
			step_used += count;
		}
		if (usage_invalid_) usage_ = Json::object();
		else { usage_["totalTokens"] = total; last_step_used_ = step_used; }
	}
	else if (type == "session.execution.succeeded") Finish(finish_ == "length" ? "max_tokens" : finish_ == "content-filter" ? "refusal" : "end_turn");
	else if (type == "session.execution.interrupted") Finish("cancelled");
	else if (type == "session.execution.failed")
	{
		Json failure{{"nativeError", data.value("error", Json{})}};
		if (!usage_.empty()) failure["usage"] = usage_;
		emit_({{"jsonrpc", "2.0"}, {"id", prompt_id_}, {"error", {{"code", -32603}, {"message", "OpenCode V2 execution failed."}, {"data", failure}}}});
		prompt_id_ = nullptr;
		permissions_.clear(); forms_.clear(); prompt_started_ = false; prompt_inbox_.clear();
		cancel_requested_ = false; cancel_interrupt_sent_ = false;
	}
}
catch (const std::exception&)
{
	Disconnected("OpenCode V2 returned an unverified event shape or control response. Input is held and is never replayed automatically.");
}

void Session::InterruptCancelledTurn()
{
	cancel_interrupt_sent_ = true;
	try
	{
		const Json reply = query_("POST", Route("/interrupt"), Json::object());
		if (!reply.is_object() || !reply.contains("interrupted") || !reply.at("interrupted").is_boolean())
			throw std::runtime_error("Unverified OpenCode interrupt response.");
		// Both true and the supported idle no-op still await the owned terminal
		// event. A lost reply may also settle through that native event.
	}
	catch (const std::exception&) {}
}

void Session::CheckDeadlines(std::chrono::steady_clock::time_point now)
{
	if (cancel_requested_ && !prompt_id_.is_null() && now >= cancel_deadline_)
		Disconnected("OpenCode V2 cancellation was not confirmed by the owned native event. Input is held and is never replayed automatically.");
}

void Session::Disconnected(std::string reason)
{
	disconnected_ = true;
	permissions_.clear(); forms_.clear();
	cancel_requested_ = false; cancel_interrupt_sent_ = false;
	if (!prompt_id_.is_null())
	{
		emit_({{"jsonrpc", "2.0"}, {"id", prompt_id_}, {"error", {{"code", -32603}, {"message", std::move(reason)},
		    {"data", {{"uamInputHeld", true}, {"noReplay", true}}}}}});
		prompt_id_ = nullptr;
	}
}
}

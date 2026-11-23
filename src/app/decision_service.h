#pragma once

#include "common/models/app_models.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{

/// <summary>The three structured answers a decision can give (#363).</summary>
enum class DecisionShape
{
	Choice,      ///< Pick one of a fixed list of options.
	Score,       ///< Rate against described levels.
	Probability, ///< How likely a statement is true, 0 to 1.
};

struct DecisionScoreLevel
{
	int value = 0;
	std::string description;
};

/// <summary>One question for a decision backend. Build it with Choice, Score or Probability.</summary>
struct DecisionRequest
{
	DecisionShape shape = DecisionShape::Choice;
	std::string question;                   ///< Choice and Score: what is being decided.
	std::vector<std::string> options;       ///< Choice: the allowed answers.
	std::vector<DecisionScoreLevel> levels; ///< Score: the allowed levels.
	std::string statement;                  ///< Probability: the statement to rate.

	static DecisionRequest Choice(std::string question, std::vector<std::string> options);
	static DecisionRequest Score(std::string question, std::vector<DecisionScoreLevel> levels);
	static DecisionRequest Probability(std::string statement);
	/// <summary>Non-empty, distinct options or levels (at least two), or a non-empty statement.</summary>
	bool IsValid() const;
};

/// <summary>Who produced a decision: a model through a backend, a deterministic UAM rule, or UAM with no model answer.</summary>
enum class DecisionOrigin
{
	Model,
	Filter,
	Fallback,
};

/// <summary>
/// Attribution for one decision. Only DecisionService can make a Model attribution, and only from a backend reply
/// it validated against the request, so a filter or fallback path has no way to name a model.
/// </summary>
class DecisionAttribution
{
  public:
	static DecisionAttribution Filter();
	static DecisionAttribution Fallback();

	DecisionOrigin Origin() const { return origin_; }
	/// <summary>Structured backend id (kDecisionBackend* in app_models.h); empty unless Origin() is Model.</summary>
	const std::string& BackendId() const { return backend_id_; }
	/// <summary>The model that answered; empty unless Origin() is Model.</summary>
	const std::string& ModelId() const { return model_id_; }
	/// <summary>The saved decision source string: "model:&lt;id&gt;", "filter" or "fallback" (kDecisionSource*).</summary>
	std::string Source() const;

  private:
	friend class DecisionService;
	DecisionAttribution(DecisionOrigin origin, std::string backend_id, std::string model_id);

	DecisionOrigin origin_ = DecisionOrigin::Fallback;
	std::string backend_id_;
	std::string model_id_;
};

/// <summary>What a backend observed while answering one request.</summary>
struct DecisionRun
{
	std::string model_id;
	/// DecisionService::NowMs() when the request was sent and when its answer arrived; 0 when unknown.
	std::int64_t started_ms = 0;
	std::int64_t finished_ms = 0;
	/// Provider-reported tokens of the turn that answered; unknown stays unknown.
	std::optional<std::int64_t> usage_tokens;
};

/// <summary>A structured decision with its reason, attribution, latency and usage.</summary>
struct DecisionResult
{
	DecisionShape shape = DecisionShape::Choice;
	std::string choice;       ///< Choice: one of the request's options.
	int score = 0;            ///< Score: one of the request's level values.
	double probability = 0.0; ///< Probability: 0 to 1.
	std::string reason;
	std::vector<std::string> evidence;
	DecisionAttribution attribution = DecisionAttribution::Fallback();
	std::int64_t latency_ms = -1; ///< -1 when the start or end time is unknown.
	std::optional<std::int64_t> usage_tokens;
};

/// <summary>
/// One decision interface for review and gate decisions (#363). Backends answer a DecisionRequest with a
/// DecisionResult. The only backend today is the chat provider (kDecisionBackendChatProvider): a model answering in
/// a provider turn that the caller runs, so the interface is split into the prompt and the reply. Hosted and local
/// decision-model backends and a Settings choice between them are not implemented.
/// </summary>
class DecisionService
{
  public:
	/// <summary>Monotonic milliseconds for DecisionRun timing; never wall-clock time.</summary>
	static std::int64_t NowMs();

	/// <summary>Chat-provider instructions asking for one JSON answer to the request; empty for an invalid request.
	/// The request text is embedded as JSON-escaped data that the model is told not to follow.</summary>
	static std::string ChatProviderPrompt(const DecisionRequest& request);
	/// <summary>A model's reply to ChatProviderPrompt as a model decision, or nullopt when it does not answer the
	/// request (no JSON object, an option or level outside the request, a probability outside 0 to 1, or no reason).</summary>
	static std::optional<DecisionResult> ParseChatProviderReply(const DecisionRequest& request, std::string_view reply, const DecisionRun& run);
	/// <summary>A Choice the caller's own parser read from a chat-provider reply (the goal review has a richer reply
	/// format). Nullopt unless the request is a valid Choice and the answer is one of its options.</summary>
	static std::optional<DecisionResult> ChatProviderChoice(const DecisionRequest& request, std::string_view choice, std::string reason, std::vector<std::string> evidence, const DecisionRun& run);

	/// <summary>A deterministic UAM rule decided; never attributed to a model.</summary>
	static DecisionResult Filter(const DecisionRequest& request, std::string choice, std::string reason);
	/// <summary>No model decision was available (a failed, invalid or missing answer); never attributed to a model.</summary>
	static DecisionResult Fallback(const DecisionRequest& request, std::string choice, std::string reason, const DecisionRun& run = {});

	/// <summary>Elapsed milliseconds, or -1 when either time is unknown or the clock went backwards.</summary>
	static std::int64_t LatencyMs(std::int64_t started_ms, std::int64_t finished_ms);
	/// <summary>Bounded diagnostic fields (shape, answer, origin, backend, model, latency, usage). Leaves out the reason
	/// and evidence, which are model-written text.</summary>
	static std::string DiagnosticDetail(const DecisionResult& result);
};

} // namespace uam

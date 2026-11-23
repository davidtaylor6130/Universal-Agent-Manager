#pragma once

#include "common/models/app_models.h"

#include <cstdint>
#include <functional>
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

/// <summary>What a backend reply says, before DecisionService checks it against the request.</summary>
struct DecisionAnswer
{
	std::string choice;
	int score = 0;
	double probability = 0.0;
	std::string reason;
	std::vector<std::string> evidence;
};

/// <summary>Who produced a decision: a model through a backend, a deterministic UAM rule, or UAM with no model answer.</summary>
enum class DecisionOrigin
{
	Model,
	Filter,
	Fallback,
};

/// <summary>
/// Attribution for one decision. Only DecisionService can make a Model attribution, and only from a backend's
/// reply text whose answer it checked against the request; filter and fallback paths have no way to name a model.
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
	/// DecisionService::NowMs() when the request was first sent and when its settling answer arrived; 0 when unknown.
	std::int64_t started_ms = 0;
	std::int64_t finished_ms = 0;
	/// Provider-reported tokens of every turn the decision took; unknown stays unknown.
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
/// A decision backend: it phrases a request for its model and reads that model's reply. A prompt-driven backend's
/// turn is run by the caller, so the interface is split into the prompt and the reply text.
/// </summary>
class DecisionBackend
{
  public:
	virtual ~DecisionBackend() = default;
	/// <summary>Structured id saved with each model decision (kDecisionBackend* in app_models.h).</summary>
	virtual const char* Id() const = 0;
	/// <summary>Instructions asking for one answer to a valid request; empty for an invalid one.</summary>
	virtual std::string Prompt(const DecisionRequest& request) const = 0;
	/// <summary>The answer in a reply to Prompt, or nullopt when the reply holds none.</summary>
	virtual std::optional<DecisionAnswer> ReadReply(const DecisionRequest& request, std::string_view reply) const = 0;
};

/// <summary>Reads a caller's own reply format (the goal review asks for more than the answer) into an answer.</summary>
using DecisionReplyReader = std::function<std::optional<DecisionAnswer>(std::string_view reply)>;

/// <summary>
/// One decision interface for review and gate decisions (#363). The only backend is the chat provider
/// (kDecisionBackendChatProvider): a model answering in a provider turn. Hosted and local decision-model backends
/// and a Settings choice between them are not implemented, so ActiveBackend() always returns the chat provider.
/// </summary>
class DecisionService
{
  public:
	/// <summary>Monotonic milliseconds for DecisionRun timing; never wall-clock time.</summary>
	static std::int64_t NowMs();
	/// <summary>The backend decisions use. Callers never name a backend themselves.</summary>
	static const DecisionBackend& ActiveBackend();

	/// <summary>
	/// The model decision in a backend's reply text, read by the backend or by the caller's reader, or nullopt when the
	/// reply does not answer the request: no answer, an option or level outside the request, a probability outside
	/// 0 to 1, or neither a reason nor evidence. This is the only way to attribute a decision to a model.
	/// </summary>
	static std::optional<DecisionResult> FromReply(const DecisionBackend& backend, const DecisionRequest& request, std::string_view reply,
	                                               const DecisionRun& run, const DecisionReplyReader& reader = {});

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

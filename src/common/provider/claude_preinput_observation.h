#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

struct ChatSession;
namespace uam { struct AppState; struct AcpSessionState; }

namespace uam::claude_preinput
{
	class Observation;
	enum class State { Absent, Initialize, Status, Settings, Observed, HandedOff, InputAttempted, Refused };

	/// <summary>Native response DATA only: no saved-row, publication or input authority.</summary>
	class NativeTuple
	{
	  public:
		const std::string& NativeId() const { return native_id; }
		const std::string& Model() const { return model; }
		const std::string& Mode() const { return mode; }
		const std::string& Version() const { return version; }
	  private:
		friend class Observation;
		NativeTuple(std::string native, std::string selected_model, std::string permission_mode, std::string cli_version)
		    : native_id(std::move(native)), model(std::move(selected_model)), mode(std::move(permission_mode)), version(std::move(cli_version)) {}
		std::string native_id, model, mode, version;
	};

	/// Original observer/process witness only; no launch, storage, saved-row or input permission.
	/// Owner-thread checks latch refusal and never adopt a replacement observation or transport.
	class NativeWitness final
	{
	  public:
		NativeWitness(const NativeWitness&) = delete;
		NativeWitness& operator=(const NativeWitness&) = delete;
		const NativeTuple& Tuple() const
		{
			return m_tuple;
		}
		bool Current(const AppState& app, AcpSessionState& session, const ChatSession& chat) const;
		/// Startup projection only; the caller must separately prove its own binding publication.
		bool Handoff(const AppState& app, AcpSessionState& session, const ChatSession& chat) const;
		/// One-way release of the pre-input dispatcher immediately before the guarded input sink.
		bool InputAttempted(const AppState& app, AcpSessionState& session, const ChatSession& chat) const;
		bool Injected() const { return m_injected; }
	  private:
		friend std::shared_ptr<const NativeWitness> CurrentWitness(const AppState&, AcpSessionState&, const ChatSession&);
		NativeWitness(const std::shared_ptr<Observation>& observation, const AcpSessionState& owner, const NativeTuple& tuple, bool injected)
		    : m_observation(observation), m_owner(&owner), m_tuple(tuple), m_injected(injected)
		{
		}
		const std::weak_ptr<Observation> m_observation;
		const AcpSessionState* const m_owner;
		const NativeTuple m_tuple;
		const bool m_injected;
		mutable bool m_refused = false;
	};

	// The opt-in owning launch path arms immediately after actual SendInitialize writes its sole request and
	// before ANY output dispatch; false is final
	// for this attempt. Current cold Retry admission stays disabled; ordinary/warm sessions retain a null observation.
	bool Arm(const AppState& app, AcpSessionState& session, const ChatSession& chat);
	State CurrentState(const AppState& app, AcpSessionState& session, const ChatSession& chat);
	std::optional<NativeTuple> CurrentTuple(const AppState& app, AcpSessionState& session, const ChatSession& chat);
	/// Minted only after all native queries complete; copied tuple/metadata cannot create this witness.
	/// Holding the witness does not retain the observer, process, writer or permission epoch.
	std::shared_ptr<const NativeWitness> CurrentWitness(const AppState& app, AcpSessionState& session, const ChatSession& chat);

	/// Process-free fixture sources only. Witnesses from this seam are explicitly synthetic
	/// and the owning production binding consumer rejects them.
	struct InjectedTransportForTests
	{
		std::function<std::string()> process;
		std::function<std::shared_ptr<const void>()> writer;
		std::function<bool(const nlohmann::json&)> write_control;
	};
	bool ArmWithInjectedTransportForTests(const AppState&, AcpSessionState&, const ChatSession&,
	    std::shared_ptr<const InjectedTransportForTests>);
	// Adapter-only seams, before ordinary initialize/setup/message projection.
	bool Consume(const AppState& app, AcpSessionState& session, const ChatSession& chat, const nlohmann::json& message);
	bool BeforeSharedDispatch(const AppState& app, AcpSessionState& session, const ChatSession& chat, const nlohmann::json* decoded_frame) noexcept;
}

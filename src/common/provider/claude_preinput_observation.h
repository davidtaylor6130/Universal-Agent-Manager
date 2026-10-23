#pragma once

#include <optional>
#include <string>
#include <utility>

struct ChatSession;
namespace uam { struct AppState; struct AcpSessionState; }

namespace uam::claude_preinput
{
	class Observation;
	enum class State { Absent, Initialize, Status, Settings, Observed, Refused };

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

	// No production caller in this component. A future immutable launch-intent owner
	// must arm immediately after actual SendInitialize writes its sole request and
	// before ANY output dispatch; false is final
	// for this attempt. Ordinary/warm sessions retain a null observation.
	bool Arm(const AppState& app, AcpSessionState& session, const ChatSession& chat);
	State CurrentState(const AppState& app, AcpSessionState& session, const ChatSession& chat);
	std::optional<NativeTuple> CurrentTuple(const AppState& app, AcpSessionState& session, const ChatSession& chat);
}

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace uam::account_refusal
{
	/// <summary>
	/// A provider refusal that applies to the signed-in account on one computer rather than to the workspace
	/// UAM asked about, so retrying each workspace cannot succeed until the account or client changes.
	/// </summary>
	enum class Kind
	{
		None,
		/// <summary>The provider no longer accepts this client for the account (for example Gemini Code Assist for individuals).</summary>
		UnsupportedClient,
		/// <summary>The account has no entitlement or subscription for the provider.</summary>
		Entitlement,
		/// <summary>The provider needs the user to sign in again on that computer.</summary>
		Authentication,
	};

	/// <summary>JSON-RPC server error code ACP providers use for authentication and account refusals.</summary>
	inline constexpr int kAcpServerErrorCode = -32000;
	/// <summary>Client and entitlement refusals change only when the provider or account does.</summary>
	inline constexpr std::int64_t kAccountRefusalTtlSeconds = 24 * 60 * 60;
	/// <summary>Signing in again is a quick user fix, so an authentication refusal is retried sooner.</summary>
	inline constexpr std::int64_t kAuthenticationRefusalTtlSeconds = 60 * 60;
	/// <summary>Signing in again in CLI View must not wait for the TTL, so an authentication refusal never blocks a chat launch.</summary>
	inline bool BlocksChatLaunch(Kind kind) { return kind == Kind::UnsupportedClient || kind == Kind::Entitlement; }

	/// <summary>
	/// Classifies an ACP <c>initialize</c>, <c>session/new</c>, <c>session/load</c> or <c>session/resume</c> error. Only the server error code together with a
	/// small set of documented phrases qualifies; any other method, code or message is an ordinary scoped failure.
	/// </summary>
	Kind ClassifyAcpError(std::string_view method, bool has_code, int code, std::string_view message);

	/// <summary>How long automatic launches skip the provider on that computer.</summary>
	std::int64_t TtlSeconds(Kind kind);

	/// <summary>Stable name stored with the refusal and written to diagnostics.</summary>
	const char* KindName(Kind kind);
	Kind KindFromName(std::string_view name);

	/// <summary>Provider-neutral one-line notice; provider runtimes may name their own vendor instead.</summary>
	std::string GenericNotice(std::string_view provider_display_name, Kind kind);

	/// <summary>Bounded single-line copy of the provider's raw message for diagnostics.</summary>
	std::string DiagnosticMessage(std::string_view message);
}

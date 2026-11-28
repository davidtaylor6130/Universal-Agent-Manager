#include "common/provider/provider_account_refusal.h"

#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <array>

namespace uam::account_refusal
{
	namespace
	{
		// Keep these lists short and in step with docs/providers/models.md#account-refusals. Each phrase is a
		// fragment of the provider's message, never the whole text, so wording around it can change.
		// Fragments name the client or the account, so model, plan or workspace errors that merely say "no longer
		// supported" or "not eligible" stay scoped to their workspace.
		constexpr std::array<std::string_view, 3> kUnsupportedClientPhrases{
		    "client is no longer supported", "unsupported client", "client is not supported"};
		constexpr std::array<std::string_view, 3> kEntitlementPhrases{"account is not eligible", "account is not entitled", "no active subscription"};
		constexpr std::array<std::string_view, 3> kAuthenticationPhrases{"authentication required", "not authenticated", "login required"};
		constexpr std::size_t kMaxDiagnosticMessageBytes = 400;
	}

	Kind ClassifyAcpError(std::string_view method, bool has_code, int code, std::string_view message)
	{
		if (!has_code || code != kAcpServerErrorCode) return Kind::None;
		// Session setup: a new session, or resuming a saved one, which meets the same account refusal.
		if (method != uam::acp_methods::kInitialize && method != uam::acp_methods::kSessionNew &&
		    method != uam::acp_methods::kSessionLoad && method != uam::acp_methods::kSessionResume) return Kind::None;
		if (uam::strings::ContainsAnyCaseInsensitive(message, kUnsupportedClientPhrases)) return Kind::UnsupportedClient;
		if (uam::strings::ContainsAnyCaseInsensitive(message, kEntitlementPhrases)) return Kind::Entitlement;
		if (uam::strings::ContainsAnyCaseInsensitive(message, kAuthenticationPhrases)) return Kind::Authentication;
		return Kind::None;
	}

	std::int64_t TtlSeconds(Kind kind)
	{
		switch (kind)
		{
		case Kind::UnsupportedClient:
		case Kind::Entitlement:
			return kAccountRefusalTtlSeconds;
		case Kind::Authentication:
			return kAuthenticationRefusalTtlSeconds;
		case Kind::None:
			break;
		}
		return 0;
	}

	const char* KindName(Kind kind)
	{
		switch (kind)
		{
		case Kind::UnsupportedClient:
			return "unsupported-client";
		case Kind::Entitlement:
			return "entitlement";
		case Kind::Authentication:
			return "authentication";
		case Kind::None:
			break;
		}
		return "";
	}

	Kind KindFromName(std::string_view name)
	{
		for (const Kind kind : {Kind::UnsupportedClient, Kind::Entitlement, Kind::Authentication})
			if (name == KindName(kind)) return kind;
		return Kind::None;
	}

	std::string GenericNotice(std::string_view provider_display_name, Kind kind)
	{
		const std::string name = uam::strings::NonEmptyOrFallback(uam::strings::Trim(std::string(provider_display_name)), "This provider");
		switch (kind)
		{
		case Kind::UnsupportedClient:
			return name + " no longer supports this client for the signed-in account.";
		case Kind::Entitlement:
			return name + " says the signed-in account is not eligible to use it.";
		case Kind::Authentication:
			return name + " needs you to sign in again on this computer.";
		case Kind::None:
			break;
		}
		return {};
	}

	std::string DiagnosticMessage(std::string_view message)
	{
		std::string line;
		line.reserve(std::min(message.size(), kMaxDiagnosticMessageBytes));
		for (const char ch : message)
		{
			if (line.size() >= kMaxDiagnosticMessageBytes) break;
			line.push_back(ch == '\n' || ch == '\r' || ch == '\t' ? ' ' : ch);
		}
		// Never end inside a UTF-8 sequence.
		if (message.size() > line.size())
		{
			while (!line.empty() && (static_cast<unsigned char>(line.back()) & 0xC0) == 0x80) line.pop_back();
			if (!line.empty() && (static_cast<unsigned char>(line.back()) & 0x80) != 0) line.pop_back();
			line += "...";
		}
		return uam::strings::Trim(line);
	}
}

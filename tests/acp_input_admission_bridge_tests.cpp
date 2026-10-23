#include "test_harness.h"
#include "cef/uam_bridge_request.h"

UAM_TEST(SendAdmissionOriginUsesEnvelopeAndRemovesPayloadSpoof)
{
	const auto parsed = uam::cef::ParseBridgeRequest(R"({"action":"sendAcpPrompt","requestId":"envelope-1","payload":{"chatId":"chat-1","text":"same text","requestId":"payload-1","_uamOriginRequestId":"spoof-1"}})");
	UAM_ASSERT(parsed.ok);
	UAM_ASSERT_EQ(parsed.request.payload.at("_uamOriginRequestId").get<std::string>(), std::string("envelope-1"));
	UAM_ASSERT_EQ(parsed.request.payload.at("requestId").get<std::string>(), std::string("payload-1"));
	UAM_ASSERT_EQ(parsed.request.payload.at("text").get<std::string>(), std::string("same text"));
}

UAM_TEST(SendAdmissionOriginMissingOrMalformedDoesNotRejectSend)
{
	const auto missing = uam::cef::ParseBridgeRequest(R"({"action":"sendAcpPrompt","payload":{"chatId":"chat-1","text":"same text","requestId":"payload-1","_uamOriginRequestId":"spoof-1"}})");
	UAM_ASSERT(missing.ok);
	UAM_ASSERT(!missing.request.payload.contains("_uamOriginRequestId"));
	const std::vector<nlohmann::json> invalid = {nullptr, false, 42, nlohmann::json::array(), nlohmann::json::object(), "", std::string(129, 'a'), std::string(1, static_cast<char>(0x7f)), "line\nbreak", std::string("nul\0byte", 8), std::string("\xc3\xa9")};
	for (const auto& value : invalid)
	{
		const nlohmann::json request{{"action", "sendAcpPrompt"}, {"requestId", value}, {"payload", {{"chatId", "chat-1"}, {"text", "same text"}, {"_uamOriginRequestId", "spoof-1"}}}};
		const auto parsed = uam::cef::ParseBridgeRequest(request.dump());
		UAM_ASSERT(parsed.ok);
		UAM_ASSERT(!parsed.request.payload.contains("_uamOriginRequestId"));
		UAM_ASSERT_EQ(parsed.request.payload.at("text").get<std::string>(), std::string("same text"));
	}
}

UAM_TEST(SendAdmissionOriginAcceptsPrintableBoundsWithoutNormalization)
{
	for (const std::string& id : {std::string(" "), std::string(128, '~'), std::string(" exact origin ")})
	{
		const nlohmann::json request{{"action", "sendAcpPrompt"}, {"requestId", id}, {"payload", {{"chatId", "chat-1"}, {"text", "hello"}}}};
		const auto parsed = uam::cef::ParseBridgeRequest(request.dump());
		UAM_ASSERT(parsed.ok);
		UAM_ASSERT_EQ(parsed.request.payload.at("_uamOriginRequestId").get<std::string>(), id);
	}
}

UAM_TEST(SendAdmissionOriginLeavesOtherActionsAndExistingPayloadValidationUnchanged)
{
	const nlohmann::json payload{{"requestId", "payload-1"}, {"_uamOriginRequestId", "ordinary-other-field"}, {"text", "hello"}};
	const nlohmann::json other{{"action", "someOtherAction"}, {"requestId", "envelope-1"}, {"payload", payload}};
	const auto parsed = uam::cef::ParseBridgeRequest(other.dump());
	UAM_ASSERT(parsed.ok);
	UAM_ASSERT(parsed.request.payload == payload);
	const auto absent_payload = uam::cef::ParseBridgeRequest(R"({"action":"sendAcpPrompt","requestId":"envelope-1"})");
	UAM_ASSERT(absent_payload.ok);
	UAM_ASSERT_EQ(absent_payload.request.payload.at("_uamOriginRequestId").get<std::string>(), std::string("envelope-1"));
	const auto invalid_payload = uam::cef::ParseBridgeRequest(R"({"action":"sendAcpPrompt","requestId":"envelope-1","payload":[]})");
	UAM_ASSERT(!invalid_payload.ok);
	UAM_ASSERT_EQ(invalid_payload.status, 400);
}

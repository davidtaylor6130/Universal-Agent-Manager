// Also runnable as a focused executable with UAM_CODEX_CHILD_METADATA_STANDALONE=1.
#if defined(UAM_CODEX_CHILD_METADATA_STANDALONE)
#include <iostream>
#include <stdexcept>
#define UAM_TEST(name) static void name()
#define UAM_ASSERT(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)
#define UAM_ASSERT_EQ(actual, expected) UAM_ASSERT((actual) == (expected))
#else
#include "test_harness.h"
#endif

#include "common/provider/codex_native_children.h"

namespace
{
const std::string kNativeParent = "550e8400-e29b-41d4-a716-446655440000";
const std::string kNativeChild = "660e8400-e29b-41d4-a716-446655440001";
const std::string kNativeOther = "770e8400-e29b-41d4-a716-446655440002";
}

UAM_TEST(CodexCollaborationChildrenNormalizeNativeAliasesAndOwnStates)
{
	const std::string upper_parent = "550E8400-E29B-41D4-A716-446655440000";
	const std::string upper_child = "660E8400-E29B-41D4-A716-446655440001";
	const nlohmann::json item = {{"type", "collabAgentToolCall"}, {"status", "completed"}, {"senderThreadId", kNativeParent},
	    {"receiverThreadIds", nlohmann::json::array({nullptr, false, 42, "bad-id", kNativeParent, upper_parent, "urn:uuid:" + upper_parent,
	        kNativeChild, upper_child, "urn:uuid:" + upper_child, kNativeOther})},
	    {"agentsStates", {{"urn:uuid:" + upper_child, {{"status", "running"}}}, {kNativeOther, {{"status", "pendingInit"}}},
	        {"880e8400-e29b-41d4-a716-446655440003", {{"status", "completed"}}}}}};
	const std::vector<SubAgentChild> expected = {{kNativeChild, "", "running"}, {kNativeOther, "", "pendingInit"}};
	UAM_ASSERT_EQ(uam::codex::CollaborationChildren(item), expected);
	auto urn_first = item;
	urn_first["receiverThreadIds"] = nlohmann::json::array({"urn:uuid:" + upper_child, kNativeChild});
	const auto children = uam::codex::CollaborationChildren(urn_first);
	UAM_ASSERT_EQ(children.size(), std::size_t(1));
	UAM_ASSERT_EQ(children.front().id, "urn:uuid:" + upper_child);
	UAM_ASSERT_EQ(children.front().status, std::string("running"));
}

UAM_TEST(CodexCollaborationChildrenFailSafeOnMalformedOptionalFields)
{
	nlohmann::json item = {{"type", "collabAgentToolCall"}, {"status", "completed"}, {"senderThreadId", kNativeParent},
	    {"receiverThreadIds", nlohmann::json::array({kNativeChild, kNativeOther})}};
	const std::vector<SubAgentChild> no_states = {{kNativeChild, "", ""}, {kNativeOther, "", ""}};
	UAM_ASSERT_EQ(uam::codex::CollaborationChildren(item), no_states);
	for (const nlohmann::json malformed : {nlohmann::json(nullptr), nlohmann::json(42), nlohmann::json("running"), nlohmann::json::array({"running"}),
	    nlohmann::json{{kNativeChild, 42}, {kNativeOther, {{"status", false}}}}, nlohmann::json{{kNativeChild, {{"status", nlohmann::json::array({"running"})}}}}})
	{
		item["agentsStates"] = malformed;
		UAM_ASSERT_EQ(uam::codex::CollaborationChildren(item), no_states);
	}
	for (const nlohmann::json malformed : {nlohmann::json(nullptr), nlohmann::json(42), nlohmann::json(kNativeChild), nlohmann::json::object()})
	{
		item["receiverThreadIds"] = malformed;
		UAM_ASSERT(uam::codex::CollaborationChildren(item).empty());
	}
	item["receiverThreadIds"] = nlohmann::json::array({kNativeChild});
	item["type"] = "mcpToolCall";
	UAM_ASSERT(uam::codex::CollaborationChildren(item).empty());
	for (const nlohmann::json malformed : {nlohmann::json(nullptr), nlohmann::json(42), nlohmann::json::array()})
		UAM_ASSERT(uam::codex::CollaborationChildren(malformed).empty());
}

#if defined(UAM_CODEX_CHILD_METADATA_STANDALONE)
int main()
{
	try
	{
		CodexCollaborationChildrenNormalizeNativeAliasesAndOwnStates();
		CodexCollaborationChildrenFailSafeOnMalformedOptionalFields();
		std::cout << "PASS: 2 focused Codex child normalization tests\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
#endif

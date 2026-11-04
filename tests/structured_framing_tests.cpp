#include "test_harness.h"
#include "common/runtime/structured_framing.h"

using namespace uam_test;

UAM_TEST(NativeRpcFramesPreserveUtf8FragmentationAndAdjacentMessages)
{
	const std::string body = R"({"text":"hello 🚀","id":7})";
	const std::string encoded = uam::structured_framing::Encode(body);
	for (std::size_t bytes = 0; bytes < encoded.size(); ++bytes)
		UAM_ASSERT(uam::structured_framing::Read(std::string_view(encoded).substr(0, bytes)).status == uam::structured_framing::Status::Incomplete);
	const auto frame = uam::structured_framing::Read(encoded + encoded);
	UAM_ASSERT(frame.status == uam::structured_framing::Status::Complete);
	UAM_ASSERT_EQ(frame.consumed_bytes, encoded.size());
	UAM_ASSERT_EQ(encoded.substr(frame.body_offset, frame.body_bytes), body);
}

UAM_TEST(NativeRpcFramesRejectAmbiguousOversizedAndMalformedLengths)
{
	for (const std::string& header : {"Content-Length: -1", "Content-Length: 0", "Content-Length: 2x", "Content-Length: 184467440737095516160", "Content-Length: 65", "Content-Type: application/json", "Content-Length: 2\r\nContent-Length: 2"})
		UAM_ASSERT(uam::structured_framing::Read(header + "\r\n\r\n{}", 64).status == uam::structured_framing::Status::Invalid);
	UAM_ASSERT(uam::structured_framing::Read(std::string(8193, 'x')).status == uam::structured_framing::Status::Invalid);
	const auto frame = uam::structured_framing::Read("content-length: 2\r\nContent-Type: application/json\r\n\r\n{}");
	UAM_ASSERT(frame.status == uam::structured_framing::Status::Complete);
}

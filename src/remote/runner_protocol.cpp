#include "remote/runner_protocol.h"

#include "remote/runner_state.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <istream>
#include <ostream>
#include <string>

namespace uam::remote
{
	namespace
	{
		void SetError(std::string* error, std::string message)
		{
			if (error != nullptr) *error = std::move(message);
		}

		bool IsBoundedText(const nlohmann::json& value, std::size_t max_size)
		{
			if (!value.is_string()) return false;
			const std::string& text = value.get_ref<const std::string&>();
			return !text.empty() && text.size() <= max_size &&
			       std::ranges::none_of(text, [](unsigned char character)
			       { return character < 0x20 || character == 0x7f; });
		}

		nlohmann::json RequestId(const nlohmann::json& request)
		{
			if (request.is_object() && request.contains("id") &&
			    IsBoundedText(request["id"], 128))
				return request["id"];
			return nullptr;
		}

		nlohmann::json ErrorResponse(const nlohmann::json& request, std::string code,
		                             std::string message)
		{
			return {{"id", RequestId(request)}, {"type", "error"}, {"ok", false},
			        {"error", {{"code", std::move(code)}, {"message", std::move(message)}}}};
		}

		std::string PlatformName()
		{
#if defined(_WIN32)
			return "windows";
#elif defined(__APPLE__)
			return "macos";
#elif defined(__linux__)
			return "linux";
#else
			return "unknown";
#endif
		}

		std::string ArchitectureName()
		{
#if defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
			return "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
			return "x86_64";
#else
			return "unknown";
#endif
		}
	}

	FrameReadResult ReadFrame(std::istream& input, nlohmann::json& message, std::string* error)
	{
		message = nullptr;
		std::array<unsigned char, 4> header{};
		input.read(reinterpret_cast<char*>(header.data()),
		           static_cast<std::streamsize>(header.size()));
		if (input.gcount() == 0 && input.eof()) return FrameReadResult::EndOfStream;
		if (input.gcount() != static_cast<std::streamsize>(header.size()))
		{
			SetError(error, "Runner frame header is truncated.");
			return FrameReadResult::Error;
		}

		const std::uint32_t size = (static_cast<std::uint32_t>(header[0]) << 24U) |
		                           (static_cast<std::uint32_t>(header[1]) << 16U) |
		                           (static_cast<std::uint32_t>(header[2]) << 8U) |
		                           static_cast<std::uint32_t>(header[3]);
		if (size == 0 || size > kMaxRunnerFrameBytes)
		{
			SetError(error, "Runner frame size is invalid.");
			return FrameReadResult::Error;
		}

		std::string body(size, '\0');
		input.read(body.data(), static_cast<std::streamsize>(body.size()));
		if (input.gcount() != static_cast<std::streamsize>(body.size()))
		{
			SetError(error, "Runner frame body is truncated.");
			return FrameReadResult::Error;
		}

		message = nlohmann::json::parse(body, nullptr, false);
		if (message.is_discarded() || !message.is_object())
		{
			message = nullptr;
			SetError(error, "Runner frame must contain one JSON object.");
			return FrameReadResult::Error;
		}
		return FrameReadResult::Ok;
	}

	bool WriteFrame(std::ostream& output, const nlohmann::json& message, std::string* error)
	{
		const std::string body = message.dump();
		if (body.empty() || body.size() > kMaxRunnerFrameBytes)
		{
			SetError(error, "Runner response exceeds the frame limit.");
			return false;
		}
		const std::uint32_t size = static_cast<std::uint32_t>(body.size());
		const std::array<unsigned char, 4> header = {
		    static_cast<unsigned char>((size >> 24U) & 0xffU),
		    static_cast<unsigned char>((size >> 16U) & 0xffU),
		    static_cast<unsigned char>((size >> 8U) & 0xffU),
		    static_cast<unsigned char>(size & 0xffU),
		};
		output.write(reinterpret_cast<const char*>(header.data()),
		             static_cast<std::streamsize>(header.size()));
		output.write(body.data(), static_cast<std::streamsize>(body.size()));
		output.flush();
		if (!output)
		{
			SetError(error, "Runner response could not be written.");
			return false;
		}
		return true;
	}

	nlohmann::json HandleRunnerRequest(const nlohmann::json& request,
	                                   std::string_view runner_version,
	                                   RunnerState* state)
	{
		if (!request.is_object() || !request.contains("id") ||
		    !IsBoundedText(request.value("id", nlohmann::json{}), 128))
			return ErrorResponse(request, "invalid_request", "A bounded string id is required.");
		if (!request.contains("type") || !IsBoundedText(request["type"], 64))
			return ErrorResponse(request, "invalid_request", "A bounded request type is required.");

		const std::string type = request["type"].get<std::string>();
		if (type != "hello")
			return state != nullptr
			    ? state->HandleProcessRequest(request)
			    : ErrorResponse(request, "unsupported_request",
			                    "Remote process execution is unavailable in this context.");
		if (!request.contains("protocolVersion") || !request["protocolVersion"].is_number_integer() ||
		    request["protocolVersion"].get<int>() != kRunnerProtocolVersion)
			return ErrorResponse(request, "protocol_mismatch", "Runner protocol version " +
			                     std::to_string(kRunnerProtocolVersion) + " is required.");
		if (!request.contains("nonce") || !IsBoundedText(request["nonce"], 128))
			return ErrorResponse(request, "invalid_request", "A bounded nonce is required.");

		return {{"id", request["id"]}, {"type", "hello"}, {"ok", true},
		        {"protocolVersion", kRunnerProtocolVersion}, {"nonce", request["nonce"]},
		        {"runnerVersion", runner_version}, {"platform", PlatformName()},
		        {"architecture", ArchitectureName()},
		        {"capabilities", {{"computerUse", false},
			                          {"directoryBrowsing", state != nullptr},
			                          {"fileCopy", state != nullptr},
			                          {"processInputAcknowledgement", state != nullptr},
			                          {"processOutputAcknowledgement", state != nullptr},
			                          {"channelOutputAcknowledgement", state != nullptr},
		                          {"processExecution", state != nullptr}}}};
	}
}

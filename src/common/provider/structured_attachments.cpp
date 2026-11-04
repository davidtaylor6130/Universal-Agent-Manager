#include "common/provider/structured_attachments.h"
#include "common/chat/message_attachment_sources.h"
#include "common/config/execution_host_config.h"
#include "common/paths/path_utils.h"
#include "common/provider/provider_profile_constants.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/platform/async_byte_writer.h"
#include "common/utils/base64.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"
#include "app/attachment_preview_service.h"
#include "common/state/app_state.h"
#include <algorithm>
#include <array>
#include <filesystem>

namespace uam::structured_attachments
{
	bool ValidateDeliverySources(const std::vector<MessageAttachment>& attachments, std::string& error)
	{
		for (const auto& attachment : attachments)
			if (attachment.source != AttachmentSource::LegacyPath || !attachment_sources::IsPersistable(attachment))
			{
				error = "Recorded or invalid attachment sources cannot be sent as file paths. Remove them and attach files from the execution host again.";
				return false;
			}
			else if (strings::TrimAsciiView(attachment.path).empty() || attachment.path.find('\0') != std::string::npos)
			{
				error = "An attached resource has an invalid path. Remove it and attach it again.";
				return false;
			}
		return true;
	}

	std::vector<MessageAttachment> CurrentTurn(const ChatSession& chat, const AcpSessionState& session)
	{
		std::vector<MessageAttachment> result;
		const int first = session.turn_first_user_message_index >= 0 ? session.turn_first_user_message_index : session.turn_user_message_index;
		if (first < 0 || session.turn_user_message_index < first) return result;
		for (std::size_t index = static_cast<std::size_t>(first); index < chat.messages.size() && index <= static_cast<std::size_t>(session.turn_user_message_index); ++index)
		{
			const Message& message = chat.messages[index];
			if (message.role != MessageRole::User || message.acp_prompt_discarded) continue;
			result.insert(result.end(), message.attachments.begin(), message.attachments.end());
		}
		return result;
	}

	std::string ProviderPath(const std::string& workspace, const std::string& path, const std::string& platform, bool remote, std::string& error)
	{
		if (path.empty() || path.find('\0') != std::string::npos || workspace.find('\0') != std::string::npos)
		{ error = "An attached resource has an invalid path. Remove it and attach it again."; return {}; }
		if (remote)
		{
			if (execution_hosts::IsAbsoluteRemotePath(platform, path)) return path;
			if (!execution_hosts::IsAbsoluteRemotePath(platform, workspace))
			{ error = "Attachments require an absolute workspace on the SSH host."; return {}; }
			return execution_hosts::JoinRemotePath(platform, workspace, path);
		}
		std::error_code ec;
		std::filesystem::path resolved = paths::PathFromUtf8(path);
		if (!resolved.is_absolute()) resolved = paths::PathFromUtf8(workspace) / resolved;
		resolved = std::filesystem::absolute(resolved, ec);
		if (ec) { error = "Could not resolve an attached resource in this workspace."; return {}; }
		return paths::Utf8PathString(resolved.lexically_normal());
	}

	std::string FileUri(std::string path)
	{
		if (path.starts_with("\\\\")) std::replace(path.begin(), path.end(), '\\', '/');
		if (path.size() >= 3 && path[1] == ':')
		{
			std::replace(path.begin(), path.end(), '\\', '/');
			path.insert(path.begin(), '/');
		}
		std::string encoded = "file://";
		if (path.starts_with("//")) path.erase(0, 2);
		constexpr char digits[] = "0123456789ABCDEF";
		for (const unsigned char byte : path)
		{
			if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '/' || byte == ':' || byte == '-' || byte == '_' || byte == '.' || byte == '~') encoded += static_cast<char>(byte);
			else { encoded += '%'; encoded += digits[byte >> 4]; encoded += digits[byte & 15]; }
		}
		return encoded;
	}

	void AppendPreparedAcpImages(nlohmann::json& request, const nlohmann::json& prepared)
	{
		if (!request.contains("params") || !request["params"].is_object() || !request["params"].contains("prompt") || !request["params"]["prompt"].is_array() ||
		    !prepared.contains("message") || !prepared["message"].is_object() || !prepared["message"].contains("content") || !prepared["message"]["content"].is_array()) return;
		for (const nlohmann::json& part : prepared["message"]["content"])
		{
			if (part.value("type", "") != "image") continue;
			const nlohmann::json source = part.value("source", nlohmann::json::object());
			if (!source.is_object() || source.value("type", "") != "base64") continue;
			request["params"]["prompt"].push_back({{"type", "image"}, {"mimeType", source.value("media_type", "")}, {"data", source.value("data", "")}});
		}
	}

	bool AppendNative(nlohmann::json& request, const AcpSessionState& session, const std::vector<MessageAttachment>& attachments,
	    const std::string& workspace, const std::string& platform, bool remote, std::string& error)
	{
		if (!ValidateDeliverySources(attachments, error)) return false;
		using namespace provider_profile_constants;
		const bool acp = session.protocol_kind == kProtocolGeminiAcp || session.protocol_kind == kProtocolOpenCodeAcp || session.protocol_kind == kProtocolCopilotAcp;
		const bool codex = session.protocol_kind == kProtocolCodexAppServer;
		const bool copilot = session.protocol_kind == "copilot-sdk";
		const bool claude = session.protocol_kind == kProtocolClaudeCodeStreamJson;
		const bool claude_images = claude && request.contains("message") && request["message"].is_object() && request["message"].contains("content") && request["message"]["content"].is_array() &&
		    std::ranges::any_of(request["message"]["content"], [](const nlohmann::json& part) { return part.is_object() && part.value("type", "") == "image"; });
		const bool inline_images = acp && request.contains("params") && request["params"].is_object() && request["params"].contains("prompt") && request["params"]["prompt"].is_array() &&
		    std::ranges::any_of(request["params"]["prompt"], [](const nlohmann::json& part) { return part.is_object() && part.value("type", "") == "image"; });
		nlohmann::json parts = nlohmann::json::array();
		for (const MessageAttachment& attachment : attachments)
		{
			const bool image = attachment.kind == "image";
			if (image && ((!acp && !codex && !copilot && !claude) || (claude && remote && !claude_images) || (acp && !session.prompt_image_supported)))
			{
				error = claude && remote ? "Native image input for Claude over SSH could not be prepared. Attach the image again and retry."
				    : "This provider session has not advertised native image input. Remove the image or choose a provider that supports it.";
				return false;
			}
			if (claude || (!acp && !codex && !copilot) || (codex && !image) || (image && inline_images)) continue;
			const std::string path = ProviderPath(workspace, attachment.path, platform, remote, error);
			if (path.empty()) return false;
			if (codex) parts.push_back({{"type", "localImage"}, {"path", path}});
			else if (copilot) parts.push_back({{"type", attachment.kind == "directory" ? "directory" : "file"}, {"path", path}, {"displayName", attachment.name}});
			else
			{
				const std::string uri = FileUri(path);
				// Gemini's ACP reader currently strips file:// without URI decoding. Preserve the
				// existing literal path in prompt text rather than sending a known broken link.
				// Images use prepared native bytes above, never this compatibility fallback.
				if (session.protocol_kind == kProtocolGeminiAcp && uri != "file://" + path && !image) continue;
				nlohmann::json part{{"type", "resource_link"}, {"uri", uri}, {"name", attachment.name.empty() ? attachment.path : attachment.name}};
				if (!attachment.mime_type.empty()) part["mimeType"] = attachment.mime_type;
				parts.push_back(std::move(part));
			}
		}
		if (parts.empty()) return true;
		const char* field = codex ? "input" : copilot ? "attachments" : "prompt";
		if (!request.is_object() || !request.contains("params") || !request["params"].is_object() ||
		    (request["params"].contains(field) && !request["params"][field].is_array()))
		{ error = "The provider did not create a valid native attachment request."; return false; }
		if (!request["params"].contains(field)) request["params"][field] = nlohmann::json::array();
		for (nlohmann::json& part : parts) request["params"][field].push_back(std::move(part));
		return true;
	}
}

namespace uam::claude_input
{
	std::vector<MessageAttachment> TurnImages(const ChatSession& chat, int first, int last)
	{
		std::vector<MessageAttachment> images;
		for (int index = first; index >= 0 && index <= last && index < static_cast<int>(chat.messages.size()); ++index)
		{
			const Message& user = chat.messages[static_cast<std::size_t>(index)];
			if (user.role != MessageRole::User || user.acp_prompt_discarded) continue;
			for (const MessageAttachment& attachment : user.attachments)
				if (attachment.kind == "image") images.push_back(attachment);
		}
		return images;
	}

	nlohmann::json Build(const std::string& prompt, const std::filesystem::path& workspace,
	    const std::vector<MessageAttachment>& images, std::string& error, bool prepare_delivery, std::stop_token cancel)
	{
		return BuildWithReader(prompt, images, error, prepare_delivery, cancel,
		    [&workspace](const MessageAttachment& image, std::string& bytes, std::size_t max_bytes, std::string&)
		    {
			    const std::filesystem::path path = uam::paths::PathFromUtf8(image.path);
			    return uam::attachment_preview::ReadDeliverySource(path.is_absolute() ? path : workspace / path, bytes, max_bytes);
		    });
	}

	namespace
	{
		nlohmann::json BuildImageMessage(const std::string& prompt, const std::vector<MessageAttachment>& images, std::string& error, bool prepare_delivery, std::stop_token cancel, const ImageSourceReader& reader, std::size_t max_wire_bytes)
		{
			error.clear();
			const auto fail = [&error](std::string message) -> nlohmann::json
			{
				error = std::move(message);
				return nullptr;
			};
			const std::size_t limit = std::min(max_wire_bytes, uam::platform::kAsyncInputMaxQueuedBytes);
			const std::string input_limit = limit == uam::platform::kAsyncInputMaxQueuedBytes ? "UAM's 4 MiB input limit" : "the SSH input size limit";
			if (cancel.stop_requested())
				return fail("Image preparation canceled.");
			if (prompt.size() > limit)
				return fail("The prompt exceeds " + input_limit + ".");
			if (images.size() > 32)
				return fail("Send at most 32 images in one message.");
			nlohmann::json message = uam::acp_detail::BuildClaudeInputMessage(prompt);
			if (message.dump().size() + 1 > limit)
				return fail("The prompt exceeds " + input_limit + ".");
			std::vector<uam::attachment_preview::DeliveryImage> inputs;
			std::vector<std::string> originals;
			std::size_t source_bytes = 0;
			std::size_t encoded_bytes = 0;
			for (const MessageAttachment& image : images)
			{
				if (cancel.stop_requested())
					return fail("Image preparation canceled.");
				const std::filesystem::path path = uam::paths::PathFromUtf8(image.path);
				std::string bytes;
				std::string read_error;
				if (!reader(image, bytes, 50 * 1024 * 1024 - source_bytes, read_error))
					return fail(read_error.empty() ? "Could not read attached image: " + image.name : read_error);
				if (bytes.size() > 50 * 1024 * 1024 - source_bytes)
					return fail("Images exceed the 50 MiB preparation limit. Send fewer images.");
				source_bytes += bytes.size();
				uam::attachment_preview::DeliveryImage input;
				if (prepare_delivery)
				{
					input = uam::attachment_preview::PrepareDeliveryImage(bytes, uam::attachment_preview::kMaxSourceBytes, cancel);
					if (input.bytes.empty())
						return fail("Invalid or unsupported image. Use PNG, JPEG, GIF or WebP: " + image.name);
				}
				else
				{
					std::string mime = uam::strings::ToLowerAscii(image.mime_type);
					if (mime.empty())
					{
						const std::string extension = uam::strings::ToLowerAscii(path.extension().string());
						mime = extension == ".png" ? "image/png" : extension == ".jpg" || extension == ".jpeg" ? "image/jpeg" : extension == ".gif" ? "image/gif" : extension == ".webp" ? "image/webp" : "";
					}
					if (!uam::ranges::Contains(std::array<std::string_view, 4>{"image/png", "image/jpeg", "image/gif", "image/webp"}, std::string_view(mime)))
						return fail("Unsupported image. Use PNG, JPEG, GIF or WebP: " + image.name);
					input = {bytes, 0, 0, mime};
				}
				encoded_bytes += 4 * ((input.bytes.size() + 2) / 3);
				message["message"]["content"].push_back({{"type", "image"}, {"source", {{"type", "base64"}, {"media_type", input.mime_type}, {"data", ""}}}});
				inputs.push_back(std::move(input));
				originals.push_back(std::move(bytes));
			}
			const std::size_t envelope_bytes = message.dump().size() + 1;
			if (envelope_bytes >= limit)
				return fail("The prompt and images exceed " + input_limit + ".");
			const std::size_t available = limit - envelope_bytes;
			if (encoded_bytes > available)
			{
				if (!prepare_delivery)
					return fail("The prompt and images exceed " + input_limit + ". Send fewer or smaller images.");
				std::size_t reserved = 0;
				std::size_t compressible = encoded_bytes;
				std::vector<bool> preserve(inputs.size(), false);
				std::vector<std::size_t> static_indices;
				for (std::size_t index = 0; index < inputs.size(); ++index)
				{
					const std::size_t encoded_size = 4 * ((inputs[index].bytes.size() + 2) / 3);
					if (inputs[index].animated)
					{
						preserve[index] = true;
						reserved += encoded_size;
						compressible -= encoded_size;
					}
					else
						static_indices.push_back(index);
				}
				if (reserved >= available || static_indices.empty())
					return fail("Animated images must fit unchanged. Send fewer images or shorten the prompt.");
				std::ranges::sort(static_indices, [&inputs](std::size_t left, std::size_t right) { return inputs[left].bytes.size() < inputs[right].bytes.size(); });
				std::size_t remaining = static_indices.size();
				// Keep every image that fits its share unchanged before allocating space to larger images.
				for (const std::size_t index : static_indices)
				{
					const std::size_t encoded_size = 4 * ((inputs[index].bytes.size() + 2) / 3);
					if (encoded_size > (available - reserved) / remaining)
						break;
					preserve[index] = true;
					reserved += encoded_size;
					compressible -= encoded_size;
					--remaining;
				}
				for (std::size_t index = 0; index < inputs.size(); ++index)
				{
					if (cancel.stop_requested())
						return fail("Image preparation canceled.");
					const std::size_t encoded_size = 4 * ((inputs[index].bytes.size() + 2) / 3);
					if (preserve[index])
						continue;
					const std::size_t encoded_budget = (available - reserved) * encoded_size / compressible;
					const std::size_t byte_budget = encoded_budget / 4 * 3;
					if (byte_budget == 0)
						return fail("Too little input space remains for images. Shorten the prompt or send fewer images.");
					inputs[index] = uam::attachment_preview::PrepareDeliveryImage(originals[index], byte_budget, cancel);
					if (inputs[index].bytes.empty())
						return fail("This image could not fit UAM's input limit. Animated images must fit unchanged: " + images[index].name);
					message["message"]["content"][index + 1]["source"]["media_type"] = inputs[index].mime_type;
				}
			}
			for (std::size_t index = 0; index < inputs.size(); ++index)
				message["message"]["content"][index + 1]["source"]["data"] = uam::base64::Encode(inputs[index].bytes);
			if (cancel.stop_requested())
				return fail("Image preparation canceled.");
			// MIME changes and JSON escaping are included in this final transport bound.
			if (message.dump().size() + 1 > limit)
				return fail("The prompt and images exceed " + input_limit + ". Send fewer or smaller images.");
			return message;
		}
	} // namespace

	nlohmann::json BuildWithReader(const std::string& prompt, const std::vector<MessageAttachment>& images, std::string& error, bool prepare_delivery, std::stop_token cancel, const ImageSourceReader& reader, std::size_t max_wire_bytes)
	{
		error.clear();
		if (!structured_attachments::ValidateDeliverySources(images, error))
			return nullptr;
		return BuildImageMessage(prompt, images, error, prepare_delivery, cancel, reader, max_wire_bytes);
	}

	nlohmann::json BuildSavedImageRetry(const std::filesystem::path& data_root, const std::string& admitted_owner, const std::filesystem::path& workspace, const std::string& prompt, const std::vector<MessageAttachment>& attachments, std::string& error, std::stop_token cancel, SavedImageSourcePolicy policy, std::size_t max_wire_bytes)
	try
	{
		error.clear();
		const auto fail = [&error](const char* message) -> nlohmann::json
		{
			error = message;
			return nullptr;
		};
		if (admitted_owner.empty())
			return fail("Saved image Retry requires an admitted owner.");
		std::vector<MessageAttachment> images;
		for (const auto& attachment : attachments)
		{
			if (policy == SavedImageSourcePolicy::OwnedImagesOnly && attachment.source != AttachmentSource::OwnedNativeAsset)
				return fail("SSH saved image Retry accepts only retained original images. Attach other files again.");
			if (attachment.source == AttachmentSource::LegacyPath)
			{
				if (!attachment_sources::IsPersistable(attachment))
					return fail("Saved image Retry contains a mixed or invalid path source.");
			}
			else if (attachment.source == AttachmentSource::OwnedNativeAsset)
			{
				const std::string mime = uam::strings::ToLowerAscii(attachment.mime_type);
				if (attachment.kind != "image" || !attachment_sources::IsPersistable(attachment, admitted_owner) || !uam::ranges::Contains(std::array<std::string_view, 4>{"image/png", "image/jpeg", "image/gif", "image/webp"}, std::string_view(mime)))
					return fail("Saved image Retry requires valid owner-scoped PNG, JPEG, GIF or WebP originals.");
			}
			else
				return fail("Saved image Retry cannot read pending, opaque or unknown attachment sources.");
			if (attachment.kind == "image")
				images.push_back(attachment);
		}
		// Validate the COMPLETE source list before selecting/reading any original or local image.
		if (cancel.stop_requested())
			return fail("Image preparation canceled.");
		if (images.size() > 32)
			return fail("Send at most 32 images in one message.");
		const std::size_t limit = std::min(uam::platform::kAsyncInputMaxQueuedBytes, max_wire_bytes);
		if (prompt.size() > limit)
			return fail(policy == SavedImageSourcePolicy::OwnedImagesOnly ? "The prompt exceeds the SSH input limit." : "The prompt exceeds UAM's 4 MiB input limit.");
		std::string labeled_prompt = prompt;
		for (std::size_t index = 0; index < images.size(); ++index)
		{
			if (cancel.stop_requested())
				return fail("Image preparation canceled.");
			if (images[index].name.size() > limit - labeled_prompt.size())
				return fail(policy == SavedImageSourcePolicy::OwnedImagesOnly ? "The prompt and image names exceed the SSH input limit." : "The prompt and image names exceed UAM's 4 MiB input limit.");
			const std::string quoted_name = nlohmann::json(images[index].name).dump();
			const std::string prefix = "\nImage " + std::to_string(index + 1) + " (native image-block order): ";
			if (prefix.size() > limit - labeled_prompt.size() || quoted_name.size() > limit - labeled_prompt.size() - prefix.size())
				return fail(policy == SavedImageSourcePolicy::OwnedImagesOnly ? "The prompt and image names exceed the SSH input limit." : "The prompt and image names exceed UAM's 4 MiB input limit.");
			labeled_prompt += prefix + quoted_name;
		}
		return BuildImageMessage(
		    labeled_prompt, images, error, true, cancel,
		    [&data_root, &admitted_owner, &workspace, cancel, policy](const MessageAttachment& image, std::string& bytes, std::size_t max_bytes, std::string& read_error)
		    {
			    if (image.source == AttachmentSource::OwnedNativeAsset)
			    {
				    const std::size_t declared_limit = policy == SavedImageSourcePolicy::OwnedImagesOnly
				        ? static_cast<std::size_t>(image.size_bytes) : native_assets::kMaxOriginalBytes;
				    auto original = native_assets::ReadOwned(data_root, admitted_owner, image.native_asset_key, std::min({max_bytes, native_assets::kMaxOriginalBytes, declared_limit}), cancel);
				    if (!original.ok)
				    {
					    read_error = std::move(original.error);
					    return false;
				    }
				    bytes = std::move(original.bytes);
				    return true;
			    }
			    if (policy == SavedImageSourcePolicy::OwnedImagesOnly)
			    { read_error = "SSH saved image Retry cannot read a controller path."; return false; }
			    const std::filesystem::path path = uam::paths::PathFromUtf8(image.path);
			    return uam::attachment_preview::ReadDeliverySource(path.is_absolute() ? path : workspace / path, bytes, max_bytes);
		    },
		    limit);
	}
	catch (...)
	{
		error = "Saved image Retry preparation failed.";
		return nullptr;
	}
} // namespace uam::claude_input

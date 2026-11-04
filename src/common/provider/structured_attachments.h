#pragma once

#include "common/models/app_models.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace uam { struct AcpSessionState; }

namespace uam::structured_attachments
{
	/// Captures only the user messages belonging to this dispatch, including merged unsent batches.
	std::vector<MessageAttachment> CurrentTurn(const ChatSession& chat, const AcpSessionState& session);
	/// Pure admission for current path-based input. Saved native source metadata
	/// is history/preview evidence, not permission to read a path or omit a resource.
	bool ValidateDeliverySources(const std::vector<MessageAttachment>& attachments, std::string& error);
	/// Resolve paths in the provider host's namespace, never against the desktop for SSH sessions.
	std::string ProviderPath(const std::string& workspace, const std::string& path,
	    const std::string& platform, bool remote, std::string& error);
	std::string FileUri(std::string path);
	/// Reuse bounded, decoded image preparation across Claude stream JSON and ACP native input.
	void AppendPreparedAcpImages(nlohmann::json& request, const nlohmann::json& prepared_claude_message);
	/// Append native path attachments without opening files on the UI thread.
	bool AppendNative(nlohmann::json& request, const AcpSessionState& session,
	    const std::vector<MessageAttachment>& attachments, const std::string& workspace,
	    const std::string& platform, bool remote, std::string& error);
} // namespace uam::structured_attachments

namespace uam::claude_input
{
	enum class SavedImageSourcePolicy { LocalCompatible, OwnedImagesOnly };
	/// Worker-only saved Retry. Caller admits the exact owner/root/row; this byte
	/// builder grants no dispatch authority. Pass COMPLETE declarations. Remote
	/// owned-only mode cannot select the controller-local legacy path reader.
	nlohmann::json BuildSavedImageRetry(const std::filesystem::path& data_root, const std::string& admitted_owner, const std::filesystem::path& workspace, const std::string& prompt, const std::vector<MessageAttachment>& attachments, std::string& error, std::stop_token cancel = {}, SavedImageSourcePolicy policy = SavedImageSourcePolicy::LocalCompatible, std::size_t max_wire_bytes = 4 * 1024 * 1024);
	using ImageSourceReader = std::function<bool(const MessageAttachment&, std::string&, std::size_t, std::string&)>;
	/// Same bounded image preparation, with bytes supplied by a host-owned local or SSH reader.
	nlohmann::json BuildWithReader(const std::string& prompt, const std::vector<MessageAttachment>& images, std::string& error, bool prepare_delivery, std::stop_token cancel, const ImageSourceReader& reader, std::size_t max_wire_bytes = 4 * 1024 * 1024);
} // namespace uam::claude_input

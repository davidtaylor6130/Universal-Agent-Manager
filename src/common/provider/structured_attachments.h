#pragma once

#include "common/models/app_models.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace uam { struct AcpSessionState; }
namespace uam::remote { class RunnerClient; }

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
	/// This turn's files staged under .UAM/attachments/ that a Git worktree chat must hold before dispatch.
	/// ponytail: a native steer skips this. Only a draft staged before the worktree and steered into a turn started on another device misses the copy; route steers through it if that appears.
	std::vector<MessageAttachment> StagedForWorktree(const ChatSession& chat, const AcpSessionState& session);
	/// Drops files the dispatch worker copies and then checks, since they can still be in the source checkout at send time.
	std::vector<MessageAttachment> WithoutWorktreeCopies(const ChatSession& chat, std::vector<MessageAttachment> attachments);
	/// Worker-only. Copies each staged file the worktree lacks out of the source checkout. A null client means the local host.
	bool EnsureStagedInWorktree(const std::vector<MessageAttachment>& staged, const std::string& worktree, const std::string& source,
	    const std::string& platform, remote::RunnerClient* client, std::string& error, std::stop_token cancel);
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
	/// Checks that each image exists on its host and is a supported format, for providers that open the file themselves.
	bool CheckImageFiles(const std::vector<MessageAttachment>& images, std::string& error, std::stop_token cancel, const ImageSourceReader& reader);
	/// Same bounded image preparation, with bytes supplied by a host-owned local or SSH reader.
	nlohmann::json BuildWithReader(const std::string& prompt, const std::vector<MessageAttachment>& images, std::string& error, bool prepare_delivery, std::stop_token cancel, const ImageSourceReader& reader, std::size_t max_wire_bytes = 4 * 1024 * 1024);
} // namespace uam::claude_input

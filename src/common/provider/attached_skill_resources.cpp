#include "common/provider/attached_skill_resources.h"

#include "common/config/execution_host_config.h"
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/provider/provider_setup.h"
#include "remote/runner_client.h"
#include "remote/runner_proxy.h"
#include "common/utils/string_utils.h"

#include <map>

namespace uam::attached_skills
{
	Span Capture(std::size_t offset, std::string_view captured_block)
	{
		Span span{offset, captured_block.size(), provider_setup::Digest(captured_block), {}};
		constexpr std::string_view prefix = "--- BEGIN ATTACHED SKILL: ";
		const std::size_t end = captured_block.find('\n');
		if (captured_block.starts_with(prefix) && end != std::string_view::npos)
		{
			const std::string_view header = captured_block.substr(0, end);
			if (header.ends_with(" ---") && header.size() > prefix.size() + 4)
			{
				span.title = std::string(uam::strings::Utf8Prefix(header.substr(prefix.size(), header.size() - prefix.size() - 4), 256));
				for (char& character : span.title)
					if (static_cast<unsigned char>(character) < 0x20 || character == 0x7f) character = ' ';
			}
		}
		return span;
	}

	void Shift(std::vector<Span>& spans, std::size_t amount)
	{
		for (Span& span : spans) span.offset += amount;
	}

	bool Validate(std::string_view prompt, const std::vector<Span>& spans, std::string& error)
	{
		std::size_t end = 0;
		std::size_t bytes = 0;
		for (const Span& span : spans)
		{
			if (span.offset < end || span.offset > prompt.size() || span.length == 0 ||
			    span.length > prompt.size() - span.offset || span.length > 2 * 1024 * 1024 - bytes)
			{
				error = "Attached skill snapshot ownership changed.";
				return false;
			}
			const Span captured = Capture(span.offset, prompt.substr(span.offset, span.length));
			if (captured.digest != span.digest || captured.title != span.title)
			{
				error = "Attached skill snapshot ownership changed.";
				return false;
			}
			end = span.offset + span.length;
			bytes += span.length;
		}
		return true;
	}

	bool Prepare(const std::filesystem::path& data_root, const ExecutionHost& host,
	             const std::string& workspace, const std::string& prompt,
	             const std::vector<Span>& spans, std::string& wire_prompt,
	             std::string& error, std::stop_token stop)
	{
		wire_prompt.clear();
		if (!Validate(prompt, spans, error)) return false;
		if (spans.empty()) { wire_prompt = prompt; return true; }
		const bool local = host.id == execution_hosts::kLocalHostId;
		if ((local && !paths::PathFromUtf8(workspace).is_absolute()) ||
		    (!local && !execution_hosts::IsAbsoluteRemotePath(host.platform, workspace)) ||
		    workspace.find_first_of("\r\n") != std::string::npos)
		{
			error = "Attached skills require an absolute workspace path.";
			return false;
		}
		std::map<std::string, std::string> resources;
		nlohmann::json files = nlohmann::json::object();
		for (const Span& span : spans)
		{
			const std::string name = span.digest + ".md";
			const std::string bytes = prompt.substr(span.offset, span.length);
			const std::pair<std::map<std::string, std::string>::iterator, bool> entry = resources.emplace(name, bytes);
			if (!entry.second && entry.first->second != bytes)
			{
				error = "Attached skill resource identity conflicts.";
				return false;
			}
			files[name] = {{"size", static_cast<std::uint64_t>(bytes.size())}, {"digest", span.digest}};
		}
		const nlohmann::json manifest{{"format", 1}, {"files", files}};
		const std::string relative = std::string(local ? ".UAM/attached-skills/" : ".UAM/provider-setup/") + provider_setup::Digest(manifest.dump());
		const std::string target_text = local
		    ? paths::Utf8PathString(paths::PathFromUtf8(workspace) / paths::PathFromUtf8(relative))
		    : execution_hosts::JoinRemotePath(host.platform, workspace, relative);
		const std::filesystem::path target = paths::PathFromUtf8(target_text);
		const std::filesystem::path source = local ? target : data_root / "attached-skills" / provider_setup::Digest(manifest.dump());
		std::vector<std::string> missing;
		if (stop.stop_requested() || !provider_setup::Prepare(source, manifest, missing, error)) return false;
		// Prepare fully verified the existing source directory; each file still checks every component below it.
		for (const std::string& name : missing)
		{
			if (stop.stop_requested() || !provider_setup::WriteRuntimeFile(source / name, resources.at(name), error, false, source)) return false;
		}
		if (!provider_setup::Prepare(source, manifest, missing, error) || !missing.empty())
		{
			if (error.empty()) error = "Attached skill resources could not be verified.";
			return false;
		}
		// The manifest digest is an index; exact captured bytes remain the publication authority.
		for (const std::pair<const std::string, std::string>& entry : resources)
		{
			std::string bytes;
			if (!provider_setup::ReadFile(source / entry.first, bytes, error, source) || bytes != entry.second)
			{
				if (error.empty()) error = "Attached skill resource bytes changed.";
				return false;
			}
		}
		if (!local)
		{
			remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
			    host);
			if (!client.InstallProviderConfiguration(source, target, &error, stop)) return false;
		}
		if (stop.stop_requested()) return false;
		std::size_t cursor = 0;
		for (const Span& span : spans)
		{
			wire_prompt.append(prompt, cursor, span.offset - cursor);
			const std::string file = local ? paths::Utf8PathString(target / (span.digest + ".md"))
			    : execution_hosts::JoinRemotePath(host.platform, target_text, span.digest + ".md");
			wire_prompt += "Attached skill" + (span.title.empty() ? std::string{} : " " + nlohmann::json(span.title).dump()) + ": read " + nlohmann::json(file).dump() +
			    " before using it; if read is denied or unavailable, do not use this skill.";
			cursor = span.offset + span.length;
		}
		wire_prompt.append(prompt, cursor, std::string::npos);
		return true;
	}
}

#pragma once

#include <algorithm>
#include <cctype>
#include <string_view>

namespace uam::opencode
{

	/// <summary>Validates native OpenCode identities independently of execution-host ID bounds.</summary>
	inline bool IsNativeId(std::string_view value)
	{
		return !value.empty() && value.size() <= 512 && !value.starts_with('-') &&
		    std::ranges::all_of(value, [](unsigned char character)
		    {
			    return std::isalnum(character) != 0 || character == '-' || character == '_';
		    });
	}

	/// <summary>Compare native workspace spelling without expanding path identity or aliases.</summary>
	inline bool WorkspaceDirectoriesMatch(std::string_view expected, std::string_view actual,
	    std::string_view platform = {})
	{
		// Preserve the prior byte-equal contract, including its existing path admission elsewhere.
		if (expected == actual) return true;
		if ((platform != "windows" && platform != "Windows") || expected.size() != actual.size()) return false;
		const auto separator = [](char value) { return value == '/' || value == '\\'; };
		const auto ordinary_drive_path = [&separator](std::string_view value)
		{
			if (value.size() < 3 || value.size() > 4096 ||
			    !((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= 'a' && value[0] <= 'z')) ||
			    value[1] != ':' || !separator(value[2])) return false;
			const auto reserved = [](std::string_view component)
			{
				std::string_view stem = component.substr(0, component.find('.'));
				while (!stem.empty() && stem.back() == ' ') stem.remove_suffix(1);
				const auto equal_ascii = [&stem](std::string_view name)
				{
					if (stem.size() != name.size()) return false;
					for (std::size_t index = 0; index < stem.size(); ++index)
					{
						const char value = stem[index];
						if ((value >= 'a' && value <= 'z' ? static_cast<char>(value - 'a' + 'A') : value) != name[index]) return false;
					}
					return true;
				};
				if (equal_ascii("CON") || equal_ascii("PRN") || equal_ascii("AUX") || equal_ascii("NUL") ||
				    equal_ascii("CONIN$") || equal_ascii("CONOUT$") || equal_ascii("CLOCK$")) return true;
				if (stem.size() < 4) return false;
				const std::string_view prefix = stem.substr(0, 3);
				const bool numbered = (prefix[0] == 'C' || prefix[0] == 'c') && (prefix[1] == 'O' || prefix[1] == 'o') && (prefix[2] == 'M' || prefix[2] == 'm');
				const bool printer = (prefix[0] == 'L' || prefix[0] == 'l') && (prefix[1] == 'P' || prefix[1] == 'p') && (prefix[2] == 'T' || prefix[2] == 't');
				const std::string_view suffix = stem.substr(3);
				return (numbered || printer) && ((suffix.size() == 1 && suffix[0] >= '1' && suffix[0] <= '9') ||
				    suffix == "\xc2\xb9" || suffix == "\xc2\xb2" || suffix == "\xc2\xb3");
			};
			for (std::size_t begin = 3; begin < value.size();)
			{
				const std::size_t end = value.find_first_of("/\\", begin);
				const std::string_view component = value.substr(begin, (end == std::string_view::npos ? value.size() : end) - begin);
				if (component.empty() || component == "." || component == ".." || component.back() == '.' || component.back() == ' ' ||
				    reserved(component) || std::ranges::any_of(component, [](unsigned char character)
				    { return character < 32 || character == 127 || std::string_view(":*?\"<>|").find(static_cast<char>(character)) != std::string_view::npos; })) return false;
				if (end == std::string_view::npos) break;
				begin = end + 1;
			}
			return true;
		};
		if (!ordinary_drive_path(expected) || !ordinary_drive_path(actual)) return false;
		for (std::size_t index = 0; index < expected.size(); ++index)
			if (expected[index] != actual[index] && !(separator(expected[index]) && separator(actual[index]))) return false;
		return true;
	}

}

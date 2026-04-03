namespace
{

	int ParsePositiveLineNumber(const std::string& text, const int fallback)
	{
		const std::string trimmed = Trim(text);

		if (trimmed.empty())
		{
			return fallback;
		}

		int value = fallback;

		try
		{
			value = std::stoi(trimmed);
		}
		catch (...)
		{
			return fallback;
		}

		return std::max(1, value);
	}

} // namespace

RagSnippet RagIndexService::ParseSnippet(const std::string& snippet_text, const std::size_t max_snippet_chars)
{
	RagSnippet snippet;

	if (snippet_text.empty())
	{
		return snippet;
	}

	const std::size_t newline = snippet_text.find('\n');
	const std::string header = (newline == std::string::npos) ? snippet_text : snippet_text.substr(0, newline);
	const std::string body = (newline == std::string::npos) ? std::string{} : snippet_text.substr(newline + 1);

	std::string location = header;

	if (!location.empty() && location.front() == '[')
	{
		const std::size_t close = location.find("] ");

		if (close != std::string::npos)
		{
			location = location.substr(close + 2);
		}
	}

	if (const std::size_t metadata = location.find(" ["); metadata != std::string::npos)
	{
		location = location.substr(0, metadata);
	}

	location = Trim(location);

	const std::size_t dash = location.rfind('-');
	const std::size_t colon = (dash == std::string::npos) ? std::string::npos : location.rfind(':', dash);

	if (colon != std::string::npos && dash != std::string::npos && dash > colon + 1)
	{
		snippet.relative_path = Trim(location.substr(0, colon));
		snippet.start_line = ParsePositiveLineNumber(location.substr(colon + 1, dash - colon - 1), 1);
		snippet.end_line = ParsePositiveLineNumber(location.substr(dash + 1), snippet.start_line);

		if (snippet.end_line < snippet.start_line)
		{
			snippet.end_line = snippet.start_line;
		}
	}
	else
	{
		snippet.relative_path = location;
		snippet.start_line = 1;
		snippet.end_line = 1;
	}

	snippet.text = TruncateSnippet(body.empty() ? snippet_text : body, max_snippet_chars);
	return snippet;
}

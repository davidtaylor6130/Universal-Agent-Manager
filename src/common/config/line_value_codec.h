#pragma once

#include <string>

namespace uam
{

inline constexpr const char* kEncodedLineValuePrefix = "@uam-escaped:";

inline std::string EncodeLineValue(const std::string& value)
{
	bool needs_encoding = false;

	for (const char ch : value)
	{
		if (ch == '\\' || ch == '\n' || ch == '\r' || ch == '\t')
		{
			needs_encoding = true;
			break;
		}
	}

	if (!needs_encoding)
	{
		return value;
	}

	std::string encoded = kEncodedLineValuePrefix;
	encoded.reserve(encoded.size() + value.size());

	for (const char ch : value)
	{
		switch (ch)
		{
		case '\\':
			encoded += "\\\\";
			break;
		case '\n':
			encoded += "\\n";
			break;
		case '\r':
			encoded += "\\r";
			break;
		case '\t':
			encoded += "\\t";
			break;
		default:
			encoded.push_back(ch);
			break;
		}
	}

	return encoded;
}

inline std::string DecodeLineValue(const std::string& value)
{
	if (value.rfind(kEncodedLineValuePrefix, 0) != 0)
	{
		return value;
	}

	const std::string encoded = value.substr(std::char_traits<char>::length(kEncodedLineValuePrefix));
	std::string decoded;
	decoded.reserve(encoded.size());

	for (std::size_t i = 0; i < encoded.size(); ++i)
	{
		const char ch = encoded[i];

		if (ch != '\\' || i + 1 >= encoded.size())
		{
			decoded.push_back(ch);
			continue;
		}

		const char next = encoded[++i];

		switch (next)
		{
		case 'n':
			decoded.push_back('\n');
			break;
		case 'r':
			decoded.push_back('\r');
			break;
		case 't':
			decoded.push_back('\t');
			break;
		case '\\':
			decoded.push_back('\\');
			break;
		default:
			decoded.push_back('\\');
			decoded.push_back(next);
			break;
		}
	}

	return decoded;
}

} // namespace uam

#pragma once

#include "vectorised_rag_common.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tree_sitter/api.h>

extern "C" const TSLanguage* tree_sitter_cpp(void);

namespace ollama_engine::internal::vectorised_rag
{
	namespace fs = std::filesystem;

	struct ChunkRecord
	{
		std::string pSId;
		std::string pSSourceId;
		std::string pSFilePath;
		std::string pSChunkType;
		std::string pSSymbolName;
		std::string pSParentSymbol;
		int piStartLine = 1;
		int piEndLine = 1;
		std::string pSHash;
		std::string pSRawText;
		std::vector<float> pVecfEmbedding;
	};

	struct QueryCaptureMatch
	{
		TSNode pNodeChunk;
		TSNode pNodeName;
		bool pbHasName = false;
	};

	inline std::string GetNodeText(const TSNode pNodeValue, const std::string& pSContent)
	{
		if (ts_node_is_null(pNodeValue))
		{
			return {};
		}

		const std::uint32_t liStart = ts_node_start_byte(pNodeValue);
		const std::uint32_t liEnd = ts_node_end_byte(pNodeValue);

		if (liEnd <= liStart || liEnd > pSContent.size())
		{
			return {};
		}

		return pSContent.substr(liStart, liEnd - liStart);
	}

	inline TSNode GetChildByFieldName(const TSNode pNodeValue, const char* pSFieldName)
	{
		if (ts_node_is_null(pNodeValue) || pSFieldName == nullptr)
		{
			return TSNode{};
		}

		return ts_node_child_by_field_name(pNodeValue, pSFieldName, std::strlen(pSFieldName));
	}

	inline std::string ExtractDeclaratorName(const TSNode pNodeValue, const std::string& pSContent)
	{
		if (ts_node_is_null(pNodeValue))
		{
			return {};
		}

		const std::string_view lSType(ts_node_type(pNodeValue));

		if (lSType == "identifier" || lSType == "field_identifier" || lSType == "operator_name" || lSType == "destructor_name" || lSType == "namespace_identifier" || lSType == "type_identifier")
		{
			return TrimAscii(GetNodeText(pNodeValue, pSContent));
		}

		const TSNode lNodeNamed = GetChildByFieldName(pNodeValue, "name");

		if (!ts_node_is_null(lNodeNamed))
		{
			const std::string lSNamed = ExtractDeclaratorName(lNodeNamed, pSContent);

			if (!lSNamed.empty())
			{
				return lSNamed;
			}
		}

		const TSNode lNodeDeclarator = GetChildByFieldName(pNodeValue, "declarator");

		if (!ts_node_is_null(lNodeDeclarator))
		{
			const std::string lSDeclarator = ExtractDeclaratorName(lNodeDeclarator, pSContent);

			if (!lSDeclarator.empty())
			{
				return lSDeclarator;
			}
		}

		const std::uint32_t liNamedChildren = ts_node_named_child_count(pNodeValue);

		for (std::uint32_t liChildIndex = 0; liChildIndex < liNamedChildren; ++liChildIndex)
		{
			const TSNode lNodeChild = ts_node_named_child(pNodeValue, liChildIndex);
			const std::string lSChildName = ExtractDeclaratorName(lNodeChild, pSContent);

			if (!lSChildName.empty())
			{
				return lSChildName;
			}
		}

		return {};
	}

	inline std::string ExtractFunctionName(const TSNode pNodeFunction, const std::string& pSContent)
	{
		const TSNode lNodeDeclarator = GetChildByFieldName(pNodeFunction, "declarator");

		if (ts_node_is_null(lNodeDeclarator))
		{
			return {};
		}

		return ExtractDeclaratorName(lNodeDeclarator, pSContent);
	}

	inline std::string JoinWithScope(const std::vector<std::string>& pVecSParts)
	{
		if (pVecSParts.empty())
		{
			return {};
		}

		std::ostringstream lStream;

		for (std::size_t liIndex = 0; liIndex < pVecSParts.size(); ++liIndex)
		{
			if (liIndex > 0)
			{
				lStream << "::";
			}

			lStream << pVecSParts[liIndex];
		}

		return lStream.str();
	}

	inline std::string BuildParentSymbol(const TSNode pNodeValue, const std::string& pSContent)
	{
		if (ts_node_is_null(pNodeValue))
		{
			return {};
		}

		std::vector<std::string> lVecSAncestors;
		TSNode lNodeCurrent = ts_node_parent(pNodeValue);

		while (!ts_node_is_null(lNodeCurrent))
		{
			const std::string_view lSType(ts_node_type(lNodeCurrent));

			if (lSType == "namespace_definition" || lSType == "class_specifier" || lSType == "struct_specifier")
			{
				const TSNode lNodeName = GetChildByFieldName(lNodeCurrent, "name");
				std::string lSName = TrimAscii(GetNodeText(lNodeName, pSContent));

				if (lSName.empty() && lSType == "namespace_definition")
				{
					lSName = "(anonymous_namespace)";
				}

				if (!lSName.empty())
				{
					lVecSAncestors.push_back(lSName);
				}
			}

			lNodeCurrent = ts_node_parent(lNodeCurrent);
		}

		std::reverse(lVecSAncestors.begin(), lVecSAncestors.end());
		return JoinWithScope(lVecSAncestors);
	}

	inline bool IsInsideClassOrStruct(const TSNode pNodeValue)
	{
		if (ts_node_is_null(pNodeValue))
		{
			return false;
		}

		TSNode lNodeCurrent = ts_node_parent(pNodeValue);

		while (!ts_node_is_null(lNodeCurrent))
		{
			const std::string_view lSType(ts_node_type(lNodeCurrent));

			if (lSType == "class_specifier" || lSType == "struct_specifier")
			{
				return true;
			}

			lNodeCurrent = ts_node_parent(lNodeCurrent);
		}

		return false;
	}

	inline std::vector<QueryCaptureMatch> RunQuery(const TSNode pNodeRoot, const std::string& pSQuerySource)
	{
		std::vector<QueryCaptureMatch> lVecMatches;
		std::uint32_t liErrorOffset = 0;
		TSQueryError lQueryErrorType = TSQueryErrorNone;
		TSQuery* lPtrQuery = ts_query_new(tree_sitter_cpp(), pSQuerySource.c_str(), pSQuerySource.size(), &liErrorOffset, &lQueryErrorType);

		if (lPtrQuery == nullptr || lQueryErrorType != TSQueryErrorNone)
		{
			if (lPtrQuery != nullptr)
			{
				ts_query_delete(lPtrQuery);
			}

			return lVecMatches;
		}

		TSQueryCursor* lPtrCursor = ts_query_cursor_new();

		if (lPtrCursor == nullptr)
		{
			ts_query_delete(lPtrQuery);
			return lVecMatches;
		}

		ts_query_cursor_exec(lPtrCursor, lPtrQuery, pNodeRoot);

		TSQueryMatch lQueryMatch;

		while (ts_query_cursor_next_match(lPtrCursor, &lQueryMatch))
		{
			QueryCaptureMatch lCaptureMatch{};
			lCaptureMatch.pNodeChunk = TSNode{};
			lCaptureMatch.pNodeName = TSNode{};
			lCaptureMatch.pbHasName = false;

			for (std::uint16_t liCaptureIndex = 0; liCaptureIndex < lQueryMatch.capture_count; ++liCaptureIndex)
			{
				const TSQueryCapture lCapture = lQueryMatch.captures[liCaptureIndex];
				std::uint32_t liNameLength = 0;
				const char* lPtrCaptureName = ts_query_capture_name_for_id(lPtrQuery, lCapture.index, &liNameLength);

				if (lPtrCaptureName == nullptr || liNameLength == 0)
				{
					continue;
				}

				const std::string_view lSCaptureName(lPtrCaptureName, liNameLength);

				if (lSCaptureName == "chunk")
				{
					lCaptureMatch.pNodeChunk = lCapture.node;
				}
				else if (lSCaptureName == "name")
				{
					lCaptureMatch.pNodeName = lCapture.node;
					lCaptureMatch.pbHasName = true;
				}
			}

			if (!ts_node_is_null(lCaptureMatch.pNodeChunk))
			{
				lVecMatches.push_back(lCaptureMatch);
			}
		}

		ts_query_cursor_delete(lPtrCursor);
		ts_query_delete(lPtrQuery);
		return lVecMatches;
	}

	inline void FinalizeChunkIdentity(ChunkRecord* pPtrChunk)
	{
		if (pPtrChunk == nullptr)
		{
			return;
		}

		const std::string lSHashSeed = pPtrChunk->pSSourceId + "|" + pPtrChunk->pSFilePath + "|" + pPtrChunk->pSChunkType + "|" + pPtrChunk->pSSymbolName + "|" + pPtrChunk->pSParentSymbol + "|" + std::to_string(pPtrChunk->piStartLine) + "|" + std::to_string(pPtrChunk->piEndLine) + "|" + pPtrChunk->pSRawText;
		pPtrChunk->pSHash = determanistic_hash::HashTextHex(pPtrChunk->pSRawText);
		pPtrChunk->pSId = determanistic_hash::HashTextHex(lSHashSeed);
	}

	inline std::vector<ChunkRecord> MaybeSplitLargeChunk(const ChunkRecord& pChunkRecord, const std::size_t piChunkCharLimit)
	{
		constexpr std::size_t kiSplitSymbolLines = 120;

		if (pChunkRecord.pSRawText.size() <= piChunkCharLimit)
		{
			return {pChunkRecord};
		}

		const std::vector<std::string> lVecSLines = SplitLines(pChunkRecord.pSRawText);

		if (lVecSLines.empty())
		{
			return {pChunkRecord};
		}

		std::vector<ChunkRecord> lVecChunks;
		std::size_t liLineIndex = 0;
		int liCurrentLine = pChunkRecord.piStartLine;

		while (liLineIndex < lVecSLines.size())
		{
			const std::size_t liSegmentStart = liLineIndex;
			int liSegmentStartLine = liCurrentLine;
			std::size_t liChars = 0;

			while (liLineIndex < lVecSLines.size() && (liLineIndex - liSegmentStart) < kiSplitSymbolLines)
			{
				const std::size_t liNextChars = liChars + lVecSLines[liLineIndex].size() + 1;

				if (liLineIndex > liSegmentStart && liNextChars > piChunkCharLimit)
				{
					break;
				}

				liChars = liNextChars;
				++liLineIndex;
				++liCurrentLine;
			}

			if (liLineIndex == liSegmentStart)
			{
				++liLineIndex;
				++liCurrentLine;
			}

			ChunkRecord lSegmentChunk = pChunkRecord;
			lSegmentChunk.piStartLine = liSegmentStartLine;
			lSegmentChunk.piEndLine = std::max(liSegmentStartLine, liCurrentLine - 1);
			lSegmentChunk.pSRawText = JoinLines(lVecSLines, liSegmentStart, liLineIndex);
			lSegmentChunk.pSRawText = "// symbol: " + pChunkRecord.pSSymbolName + "\n" + (pChunkRecord.pSParentSymbol.empty() ? std::string{} : ("// parent: " + pChunkRecord.pSParentSymbol + "\n")) + lSegmentChunk.pSRawText;
			FinalizeChunkIdentity(&lSegmentChunk);
			lVecChunks.push_back(std::move(lSegmentChunk));
		}

		return lVecChunks;
	}

	inline std::vector<ChunkRecord> BuildFileSummaryFallback(const std::string& pSSourceId, const std::string& pSRelativePath, const std::string& pSContent, const std::size_t piChunkCharLimit)
	{
		constexpr int kiMaxSummaryLines = 120;
		std::vector<ChunkRecord> lVecChunks;
		ChunkRecord lChunk;
		lChunk.pSSourceId = pSSourceId;
		lChunk.pSFilePath = pSRelativePath;
		lChunk.pSChunkType = "file_summary";
		lChunk.pSSymbolName = fs::path(pSRelativePath).filename().string();
		lChunk.piStartLine = 1;
		lChunk.pSRawText = BuildOverviewText(pSContent, kiMaxSummaryLines, std::max<std::size_t>(piChunkCharLimit, std::size_t{384}));

		if (lChunk.pSRawText.empty())
		{
			return lVecChunks;
		}

		const std::size_t liLineSpan = std::max<std::size_t>(1, SplitLines(lChunk.pSRawText).size());
		lChunk.piEndLine = static_cast<int>(liLineSpan);
		FinalizeChunkIdentity(&lChunk);
		lVecChunks.push_back(std::move(lChunk));
		return lVecChunks;
	}

	inline bool IsRstHeadingUnderline(const std::string& pSLine)
	{
		const std::string lSTrimmed = TrimAscii(pSLine);

		if (lSTrimmed.size() < 3)
		{
			return false;
		}

		const char lCChar = lSTrimmed.front();

		if (lCChar != '=' && lCChar != '-' && lCChar != '~' && lCChar != '^' && lCChar != '*')
		{
			return false;
		}

		for (const char lCCheck : lSTrimmed)
		{
			if (lCCheck != lCChar)
			{
				return false;
			}
		}

		return true;
	}

	inline std::vector<ChunkRecord> ExtractChunksFromDocumentationFile(const std::string& pSSourceId, const std::string& pSRelativePath, const std::string& pSContent, const std::size_t piChunkCharLimit)
	{
		const std::size_t liChunkLimit = std::max<std::size_t>(piChunkCharLimit, std::size_t{512});
		const std::vector<std::string> lVecSLines = SplitLines(pSContent);

		if (lVecSLines.empty())
		{
			return BuildFileSummaryFallback(pSSourceId, pSRelativePath, pSContent, liChunkLimit);
		}

		std::vector<ChunkRecord> lVecChunks;
		std::string lSCurrentHeading = fs::path(pSRelativePath).filename().string();
		std::string lSCurrentChunkText;
		int liCurrentChunkStartLine = 1;

		auto FlushChunk = [&](int piEndLineInclusive)
		{
			const std::string lSTrimmed = TrimAscii(lSCurrentChunkText);

			if (lSTrimmed.empty())
			{
				lSCurrentChunkText.clear();
				return;
			}

			ChunkRecord lChunk;
			lChunk.pSSourceId = pSSourceId;
			lChunk.pSFilePath = pSRelativePath;
			lChunk.pSChunkType = "doc_section";
			lChunk.pSSymbolName = lSCurrentHeading.empty() ? fs::path(pSRelativePath).filename().string() : lSCurrentHeading;
			lChunk.pSParentSymbol.clear();
			lChunk.piStartLine = liCurrentChunkStartLine;
			lChunk.piEndLine = std::max(liCurrentChunkStartLine, piEndLineInclusive);
			lChunk.pSRawText = lSTrimmed;
			FinalizeChunkIdentity(&lChunk);
			std::vector<ChunkRecord> lVecSplitChunks = MaybeSplitLargeChunk(lChunk, liChunkLimit);
			lVecChunks.insert(lVecChunks.end(), std::make_move_iterator(lVecSplitChunks.begin()), std::make_move_iterator(lVecSplitChunks.end()));
			lSCurrentChunkText.clear();
		};

		std::size_t liIndex = 0;

		while (liIndex < lVecSLines.size())
		{
			const std::string& lSLine = lVecSLines[liIndex];
			const std::string lSTrimmed = TrimAscii(lSLine);

			bool lbMarkdownHeading = false;
			std::string lSHeadingText;

			if (!lSTrimmed.empty() && lSTrimmed.front() == '#')
			{
				lbMarkdownHeading = true;
				std::size_t liPos = 0;

				while (liPos < lSTrimmed.size() && lSTrimmed[liPos] == '#')
				{
					++liPos;
				}

				lSHeadingText = TrimAscii(lSTrimmed.substr(liPos));
			}

			bool lbSetextHeading = false;

			if (!lbMarkdownHeading && !lSTrimmed.empty() && (liIndex + 1) < lVecSLines.size() && IsRstHeadingUnderline(lVecSLines[liIndex + 1]))
			{
				lbSetextHeading = true;
				lSHeadingText = lSTrimmed;
			}

			if (lbMarkdownHeading || lbSetextHeading)
			{
				const int liHeadingLine = static_cast<int>(liIndex + 1);
				FlushChunk(liHeadingLine - 1);

				if (!lSHeadingText.empty())
				{
					lSCurrentHeading = lSHeadingText;
				}

				liCurrentChunkStartLine = liHeadingLine;
				lSCurrentChunkText = lSLine;

				if (lbSetextHeading)
				{
					lSCurrentChunkText += "\n";
					lSCurrentChunkText += lVecSLines[liIndex + 1];
					liIndex += 2;
				}
				else
				{
					++liIndex;
				}

				continue;
			}

			if (lSCurrentChunkText.empty())
			{
				liCurrentChunkStartLine = static_cast<int>(liIndex + 1);
				lSCurrentChunkText = lSLine;
			}
			else
			{
				lSCurrentChunkText += "\n";
				lSCurrentChunkText += lSLine;
			}

			const bool lbParagraphBoundary = lSTrimmed.empty();
			const bool lbChunkLarge = lSCurrentChunkText.size() >= liChunkLimit;

			if (lbChunkLarge || (lbParagraphBoundary && lSCurrentChunkText.size() >= (liChunkLimit / 2)))
			{
				FlushChunk(static_cast<int>(liIndex + 1));
			}

			++liIndex;
		}

		FlushChunk(static_cast<int>(lVecSLines.size()));

		if (lVecChunks.empty())
		{
			return BuildFileSummaryFallback(pSSourceId, pSRelativePath, pSContent, liChunkLimit);
		}

		return lVecChunks;
	}

	inline std::vector<ChunkRecord> ExtractChunksFromCppFile(const std::string& pSSourceId, const std::string& pSRelativePath, const std::string& pSContent, const std::size_t piChunkCharLimit)
	{
		std::vector<ChunkRecord> lVecAllChunks;

		TSParser* lPtrParser = ts_parser_new();

		if (lPtrParser == nullptr)
		{
			return BuildFileSummaryFallback(pSSourceId, pSRelativePath, pSContent, piChunkCharLimit);
		}

		if (!ts_parser_set_language(lPtrParser, tree_sitter_cpp()))
		{
			ts_parser_delete(lPtrParser);
			return BuildFileSummaryFallback(pSSourceId, pSRelativePath, pSContent, piChunkCharLimit);
		}

		TSTree* lPtrTree = ts_parser_parse_string(lPtrParser, nullptr, pSContent.c_str(), pSContent.size());

		if (lPtrTree == nullptr)
		{
			ts_parser_delete(lPtrParser);
			return BuildFileSummaryFallback(pSSourceId, pSRelativePath, pSContent, piChunkCharLimit);
		}

		const TSNode lNodeRoot = ts_tree_root_node(lPtrTree);
		std::vector<ChunkRecord> lVecFunctionChunks;
		std::vector<ChunkRecord> lVecTypeOverviewChunks;
		std::vector<ChunkRecord> lVecEnumNamespaceGlobalChunks;
		constexpr int kiMaxSummaryLines = 120;

		const std::vector<QueryCaptureMatch> lVecFunctionMatches = RunQuery(lNodeRoot, "(function_definition) @chunk");

		for (const QueryCaptureMatch& lMatch : lVecFunctionMatches)
		{
			ChunkRecord lChunk;
			lChunk.pSSourceId = pSSourceId;
			lChunk.pSFilePath = pSRelativePath;
			lChunk.pSChunkType = IsInsideClassOrStruct(lMatch.pNodeChunk) ? "method" : "function";
			lChunk.pSSymbolName = ExtractFunctionName(lMatch.pNodeChunk, pSContent);

			if (lChunk.pSSymbolName.empty())
			{
				lChunk.pSSymbolName = "(anonymous_function)";
			}

			lChunk.pSParentSymbol = BuildParentSymbol(lMatch.pNodeChunk, pSContent);
			lChunk.piStartLine = static_cast<int>(ts_node_start_point(lMatch.pNodeChunk).row + 1);
			lChunk.piEndLine = static_cast<int>(ts_node_end_point(lMatch.pNodeChunk).row + 1);
			lChunk.pSRawText = TrimAscii(GetNodeText(lMatch.pNodeChunk, pSContent));

			if (lChunk.pSRawText.empty())
			{
				continue;
			}

			FinalizeChunkIdentity(&lChunk);
			std::vector<ChunkRecord> lVecChunksForSymbol = MaybeSplitLargeChunk(lChunk, std::max(piChunkCharLimit, std::size_t{512}));
			lVecFunctionChunks.insert(lVecFunctionChunks.end(), std::make_move_iterator(lVecChunksForSymbol.begin()), std::make_move_iterator(lVecChunksForSymbol.end()));
		}

		const std::vector<QueryCaptureMatch> lVecClassStructMatches = RunQuery(lNodeRoot, "(class_specifier name: (type_identifier) @name) @chunk\n"
		                                                                                  "(struct_specifier name: (type_identifier) @name) @chunk");

		for (const QueryCaptureMatch& lMatch : lVecClassStructMatches)
		{
			ChunkRecord lChunk;
			lChunk.pSSourceId = pSSourceId;
			lChunk.pSFilePath = pSRelativePath;
			lChunk.pSChunkType = (std::string_view(ts_node_type(lMatch.pNodeChunk)) == "struct_specifier") ? "struct_overview" : "class_overview";
			lChunk.pSSymbolName = lMatch.pbHasName ? TrimAscii(GetNodeText(lMatch.pNodeName, pSContent)) : "";

			if (lChunk.pSSymbolName.empty())
			{
				lChunk.pSSymbolName = "(anonymous_type)";
			}

			lChunk.pSParentSymbol = BuildParentSymbol(lMatch.pNodeChunk, pSContent);
			lChunk.piStartLine = static_cast<int>(ts_node_start_point(lMatch.pNodeChunk).row + 1);
			lChunk.pSRawText = BuildOverviewText(GetNodeText(lMatch.pNodeChunk, pSContent), kiMaxSummaryLines, std::max<std::size_t>(piChunkCharLimit / 2, std::size_t{512}));

			if (lChunk.pSRawText.empty())
			{
				continue;
			}

			const std::size_t liLineSpan = std::max<std::size_t>(1, SplitLines(lChunk.pSRawText).size());
			lChunk.piEndLine = lChunk.piStartLine + static_cast<int>(liLineSpan) - 1;
			FinalizeChunkIdentity(&lChunk);
			lVecTypeOverviewChunks.push_back(std::move(lChunk));
		}

		const std::vector<QueryCaptureMatch> lVecEnumMatches = RunQuery(lNodeRoot, "(enum_specifier name: (type_identifier) @name) @chunk");

		for (const QueryCaptureMatch& lMatch : lVecEnumMatches)
		{
			ChunkRecord lChunk;
			lChunk.pSSourceId = pSSourceId;
			lChunk.pSFilePath = pSRelativePath;
			lChunk.pSChunkType = "enum";
			lChunk.pSSymbolName = lMatch.pbHasName ? TrimAscii(GetNodeText(lMatch.pNodeName, pSContent)) : "(anonymous_enum)";
			lChunk.pSParentSymbol = BuildParentSymbol(lMatch.pNodeChunk, pSContent);
			lChunk.piStartLine = static_cast<int>(ts_node_start_point(lMatch.pNodeChunk).row + 1);
			lChunk.piEndLine = static_cast<int>(ts_node_end_point(lMatch.pNodeChunk).row + 1);
			lChunk.pSRawText = TrimAscii(GetNodeText(lMatch.pNodeChunk, pSContent));

			if (lChunk.pSRawText.empty())
			{
				continue;
			}

			FinalizeChunkIdentity(&lChunk);
			std::vector<ChunkRecord> lVecChunksForSymbol = MaybeSplitLargeChunk(lChunk, std::max(piChunkCharLimit, std::size_t{512}));
			lVecEnumNamespaceGlobalChunks.insert(lVecEnumNamespaceGlobalChunks.end(), std::make_move_iterator(lVecChunksForSymbol.begin()), std::make_move_iterator(lVecChunksForSymbol.end()));
		}

		const std::vector<QueryCaptureMatch> lVecNamespaceMatches = RunQuery(lNodeRoot, "(namespace_definition name: (namespace_identifier) @name) @chunk");

		for (const QueryCaptureMatch& lMatch : lVecNamespaceMatches)
		{
			ChunkRecord lChunk;
			lChunk.pSSourceId = pSSourceId;
			lChunk.pSFilePath = pSRelativePath;
			lChunk.pSChunkType = "namespace";
			lChunk.pSSymbolName = lMatch.pbHasName ? TrimAscii(GetNodeText(lMatch.pNodeName, pSContent)) : "(anonymous_namespace)";

			if (lChunk.pSSymbolName.empty())
			{
				lChunk.pSSymbolName = "(anonymous_namespace)";
			}

			lChunk.pSParentSymbol = BuildParentSymbol(lMatch.pNodeChunk, pSContent);
			lChunk.piStartLine = static_cast<int>(ts_node_start_point(lMatch.pNodeChunk).row + 1);
			lChunk.pSRawText = BuildOverviewText(GetNodeText(lMatch.pNodeChunk, pSContent), kiMaxSummaryLines, std::max<std::size_t>(piChunkCharLimit / 2, std::size_t{512}));

			if (lChunk.pSRawText.empty())
			{
				continue;
			}

			const std::size_t liLineSpan = std::max<std::size_t>(1, SplitLines(lChunk.pSRawText).size());
			lChunk.piEndLine = lChunk.piStartLine + static_cast<int>(liLineSpan) - 1;
			FinalizeChunkIdentity(&lChunk);
			lVecEnumNamespaceGlobalChunks.push_back(std::move(lChunk));
		}

		const std::uint32_t liRootChildCount = ts_node_named_child_count(lNodeRoot);

		for (std::uint32_t liChildIndex = 0; liChildIndex < liRootChildCount; ++liChildIndex)
		{
			const TSNode lNodeChild = ts_node_named_child(lNodeRoot, liChildIndex);
			const std::string_view lSType(ts_node_type(lNodeChild));

			if (lSType == "function_definition" || lSType == "class_specifier" || lSType == "struct_specifier" || lSType == "enum_specifier" || lSType == "namespace_definition")
			{
				continue;
			}

			ChunkRecord lChunk;
			lChunk.pSSourceId = pSSourceId;
			lChunk.pSFilePath = pSRelativePath;
			lChunk.pSChunkType = "global_block";
			lChunk.pSSymbolName = std::string(lSType);
			lChunk.pSParentSymbol.clear();
			lChunk.piStartLine = static_cast<int>(ts_node_start_point(lNodeChild).row + 1);
			lChunk.pSRawText = BuildOverviewText(GetNodeText(lNodeChild, pSContent), kiMaxSummaryLines, std::max<std::size_t>(piChunkCharLimit / 2, std::size_t{384}));

			if (lChunk.pSRawText.empty())
			{
				continue;
			}

			const std::size_t liLineSpan = std::max<std::size_t>(1, SplitLines(lChunk.pSRawText).size());
			lChunk.piEndLine = lChunk.piStartLine + static_cast<int>(liLineSpan) - 1;
			FinalizeChunkIdentity(&lChunk);
			lVecEnumNamespaceGlobalChunks.push_back(std::move(lChunk));
		}

		lVecAllChunks.reserve(lVecFunctionChunks.size() + lVecTypeOverviewChunks.size() + lVecEnumNamespaceGlobalChunks.size() + 1);
		lVecAllChunks.insert(lVecAllChunks.end(), std::make_move_iterator(lVecFunctionChunks.begin()), std::make_move_iterator(lVecFunctionChunks.end()));
		lVecAllChunks.insert(lVecAllChunks.end(), std::make_move_iterator(lVecTypeOverviewChunks.begin()), std::make_move_iterator(lVecTypeOverviewChunks.end()));
		lVecAllChunks.insert(lVecAllChunks.end(), std::make_move_iterator(lVecEnumNamespaceGlobalChunks.begin()), std::make_move_iterator(lVecEnumNamespaceGlobalChunks.end()));

		if (lVecAllChunks.empty())
		{
			lVecAllChunks = BuildFileSummaryFallback(pSSourceId, pSRelativePath, pSContent, piChunkCharLimit);
		}

		ts_tree_delete(lPtrTree);
		ts_parser_delete(lPtrParser);
		return lVecAllChunks;
	}

} // namespace ollama_engine::internal::vectorised_rag

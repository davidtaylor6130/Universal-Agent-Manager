#pragma once

#include "vectorised_rag_common.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace ollama_engine::internal::vectorised_rag
{
	namespace fs = std::filesystem;

	struct SourceSpec
	{
		std::string pSInput;
		std::string pSSourceId;
		fs::path pPathRoot;
	};

	inline bool LooksLikeRemoteRepo(const std::string& pSInput)
	{
		return StartsWith(pSInput, "http://") || StartsWith(pSInput, "https://") || StartsWith(pSInput, "git@") || StartsWith(pSInput, "ssh://");
	}

	inline std::pair<std::string, std::string> SplitRemoteAndBranch(const std::string& pSInput)
	{
		const std::size_t liHashPosition = pSInput.find('#');

		if (liHashPosition == std::string::npos)
		{
			return {pSInput, ""};
		}

		return {pSInput.substr(0, liHashPosition), pSInput.substr(liHashPosition + 1)};
	}

	inline fs::path BuildRemoteClonePath(const std::string& pSRemoteUrl, const std::string& pSBranch)
	{
		const std::string lSKey = pSRemoteUrl + "#" + pSBranch;
		return fs::temp_directory_path() / "uam_ollama_engine_vectorised_rag" / determanistic_hash::HashTextHex(lSKey);
	}

	inline bool EnsureRemoteClone(const std::string& pSRemoteUrl, const std::string& pSBranch, const fs::path& pPathClone, std::string* pSErrorOut)
	{
		std::error_code lErrorCode;
		fs::create_directories(pPathClone.parent_path(), lErrorCode);
		const fs::path lPathGitDirectory = pPathClone / ".git";

		if (!fs::exists(lPathGitDirectory, lErrorCode))
		{
			const std::string lSBranchArg = pSBranch.empty() ? "" : (" --branch " + ShellQuote(pSBranch));
			const std::string lSCloneCommand = "git clone --depth 1" + lSBranchArg + " " + ShellQuote(pSRemoteUrl) + " " + ShellQuote(pPathClone.string());
			std::string lSCloneOutput;

			if (RunShellCommand(lSCloneCommand, &lSCloneOutput) != 0)
			{
				if (pSErrorOut != nullptr)
				{
					*pSErrorOut = "Failed to clone repository: " + TrimAscii(lSCloneOutput);
				}

				return false;
			}

			return true;
		}

		std::string lSFetchOutput;
		const std::string lSFetchCommand = "git -C " + ShellQuote(pPathClone.string()) + " fetch --all --prune";

		if (RunShellCommand(lSFetchCommand, &lSFetchOutput) != 0)
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Failed to fetch remote updates: " + TrimAscii(lSFetchOutput);
			}

			return false;
		}

		if (!pSBranch.empty())
		{
			std::string lSCheckoutOutput;
			const std::string lSCheckoutCommand = "git -C " + ShellQuote(pPathClone.string()) + " checkout " + ShellQuote(pSBranch);

			if (RunShellCommand(lSCheckoutCommand, &lSCheckoutOutput) != 0)
			{
				if (pSErrorOut != nullptr)
				{
					*pSErrorOut = "Failed to checkout branch: " + TrimAscii(lSCheckoutOutput);
				}

				return false;
			}

			std::string lSPullOutput;
			const std::string lSPullCommand = "git -C " + ShellQuote(pPathClone.string()) + " pull --ff-only origin " + ShellQuote(pSBranch);
			RunShellCommand(lSPullCommand, &lSPullOutput);
		}

		return true;
	}

	inline bool ResolveSourceSpec(Context& pContext, const std::optional<std::string>& pOptSVectorFile, SourceSpec* pPtrSourceSpecOut, std::string* pSErrorOut)
	{
		if (pPtrSourceSpecOut == nullptr)
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Source output pointer was null.";
			}

			return false;
		}

		std::string lSInput;
		{
			std::lock_guard<std::mutex> lGuard(pContext.pMutex);

			if (pOptSVectorFile.has_value() && !pOptSVectorFile->empty())
			{
				lSInput = *pOptSVectorFile;
				pContext.pSLastVectorFileInput = lSInput;
			}
			else
			{
				lSInput = pContext.pSLastVectorFileInput;
			}
		}

		if (lSInput.empty())
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "No scan target is known. Provide vector_file at least once.";
			}

			return false;
		}

		if (LooksLikeRemoteRepo(lSInput))
		{
			const auto [lSRemoteUrl, lSBranch] = SplitRemoteAndBranch(lSInput);

			if (lSRemoteUrl.empty())
			{
				if (pSErrorOut != nullptr)
				{
					*pSErrorOut = "Remote repository URL was empty.";
				}

				return false;
			}

			const fs::path lPathClone = BuildRemoteClonePath(lSRemoteUrl, lSBranch);

			if (!EnsureRemoteClone(lSRemoteUrl, lSBranch, lPathClone, pSErrorOut))
			{
				return false;
			}

			pPtrSourceSpecOut->pSInput = lSInput;
			pPtrSourceSpecOut->pSSourceId = lSRemoteUrl + (lSBranch.empty() ? "" : ("#" + lSBranch));
			pPtrSourceSpecOut->pPathRoot = lPathClone;
			return true;
		}

		std::error_code lErrorCode;
		fs::path lPathResolved = fs::absolute(fs::path(lSInput), lErrorCode);

		if (lErrorCode)
		{
			lPathResolved = fs::path(lSInput);
		}

		if (!fs::exists(lPathResolved, lErrorCode))
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Local path does not exist: " + lSInput;
			}

			return false;
		}

		if (fs::is_regular_file(lPathResolved, lErrorCode))
		{
			lPathResolved = lPathResolved.parent_path();
		}

		if (!fs::is_directory(lPathResolved, lErrorCode))
		{
			if (pSErrorOut != nullptr)
			{
				*pSErrorOut = "Scan target must be a directory or repository path.";
			}

			return false;
		}

		pPtrSourceSpecOut->pSInput = lSInput;
		pPtrSourceSpecOut->pSSourceId = NormalizePathKey(lPathResolved);
		pPtrSourceSpecOut->pPathRoot = lPathResolved;
		return true;
	}

	inline bool IsSupportedCppFile(const fs::path& pPathFile)
	{
		static const std::unordered_set<std::string> kSetSExtensions = {".c++", ".cc", ".cpp", ".cxx", ".h", ".h++", ".hh", ".hpp", ".hxx", ".ipp", ".tcc", ".tpp"};
		return kSetSExtensions.find(ToLowerAscii(pPathFile.extension().string())) != kSetSExtensions.end();
	}

	inline bool IsSupportedDocumentationFile(const fs::path& pPathFile)
	{
		static const std::unordered_set<std::string> kSetSExtensions = {".md", ".markdown", ".txt", ".rst", ".adoc", ".asciidoc"};
		return kSetSExtensions.find(ToLowerAscii(pPathFile.extension().string())) != kSetSExtensions.end();
	}

	inline bool IsIndexableSourceFile(const fs::path& pPathFile)
	{
		return IsSupportedCppFile(pPathFile) || IsSupportedDocumentationFile(pPathFile);
	}

	inline bool ShouldSkipDirectory(const fs::path& pPathDirectory)
	{
		const std::string lSName = pPathDirectory.filename().string();

		if (lSName.empty())
		{
			return false;
		}

		if (lSName == ".git" || lSName == ".svn" || lSName == ".hg")
		{
			return true;
		}

		if (!lSName.empty() && lSName.front() == '.')
		{
			return true;
		}

		return false;
	}

	inline std::vector<fs::path> CollectIndexableFiles(const fs::path& pPathRoot)
	{
		std::vector<fs::path> lVecPathFiles;
		std::error_code lErrorCode;
		fs::recursive_directory_iterator lIterator(pPathRoot, fs::directory_options::skip_permission_denied, lErrorCode);
		const fs::recursive_directory_iterator lEndIterator;

		while (!lErrorCode && lIterator != lEndIterator)
		{
			const fs::directory_entry lEntry = *lIterator;
			lIterator.increment(lErrorCode);

			if (lErrorCode)
			{
				lErrorCode.clear();
				continue;
			}

			if (lEntry.is_directory(lErrorCode))
			{
				if (!lErrorCode && ShouldSkipDirectory(lEntry.path()))
				{
					lIterator.disable_recursion_pending();
				}

				lErrorCode.clear();
				continue;
			}

			if (!lEntry.is_regular_file(lErrorCode) || lErrorCode)
			{
				lErrorCode.clear();
				continue;
			}

			if (!IsIndexableSourceFile(lEntry.path()))
			{
				continue;
			}

			lVecPathFiles.push_back(lEntry.path());
		}

		std::sort(lVecPathFiles.begin(), lVecPathFiles.end());
		return lVecPathFiles;
	}

	inline std::optional<std::string> ReadFileText(const fs::path& pPathFile)
	{
		constexpr std::uintmax_t kiMaxSourceFileBytes = 2 * 1024 * 1024;
		std::error_code lErrorCode;
		const std::uintmax_t liFileSize = fs::file_size(pPathFile, lErrorCode);

		if (lErrorCode || liFileSize > kiMaxSourceFileBytes)
		{
			return std::nullopt;
		}

		std::ifstream lFileIn(pPathFile, std::ios::binary);

		if (!lFileIn.good())
		{
			return std::nullopt;
		}

		std::ostringstream lBuffer;
		lBuffer << lFileIn.rdbuf();
		return lBuffer.str();
	}

	inline bool IsLikelyBinary(const std::string& pSContent)
	{
		const std::size_t liProbeSize = std::min<std::size_t>(pSContent.size(), 4096);

		for (std::size_t liIndex = 0; liIndex < liProbeSize; ++liIndex)
		{
			if (pSContent[liIndex] == '\0')
			{
				return true;
			}
		}

		return false;
	}

} // namespace ollama_engine::internal::vectorised_rag

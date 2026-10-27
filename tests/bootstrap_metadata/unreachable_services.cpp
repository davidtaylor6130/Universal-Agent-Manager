// MSVC resolves every reference before /OPT:REF discards code, unlike -dead_strip and --gc-sections.
// These product services are referenced only by RunnerClient/MemoryLibraryService paths this isolated target
// never reaches; a call aborts loudly, and any other missing symbol still fails the link.
#include "app/chat_domain_service.h"
#include "app/memory_service.h"
#include "common/usage/antigravity_accounting.h"
#include "common/usage/usage_store.h"

#include <cstdio>
#include <cstdlib>

namespace
{
	[[noreturn]] void Unreachable(const char* name)
	{
		std::fprintf(stderr, "uam_bootstrap_metadata_tests reached unlinked service %s\n", name);
		std::abort();
	}
}

const ChatFolder* ChatDomainService::FindFolderById(const uam::AppState&, const std::string&) const { Unreachable("ChatDomainService::FindFolderById"); }
std::filesystem::path MemoryService::GlobalMemoryRoot(const std::filesystem::path&) { Unreachable("MemoryService::GlobalMemoryRoot"); }
std::filesystem::path MemoryService::LocalMemoryRoot(const std::filesystem::path&) { Unreachable("MemoryService::LocalMemoryRoot"); }

namespace uam::usage
{
	std::optional<AccountingRecord> DecodeAccountingRecord(const nlohmann::json&) { Unreachable("usage::DecodeAccountingRecord"); }
	bool ReceiptSnapshot::Visit(const std::function<bool(const AccountingRecord&)>&, std::string&, const std::function<bool()>&) const { Unreachable("usage::ReceiptSnapshot::Visit"); }
	std::shared_ptr<const ReceiptSnapshot> ReadReceiptSnapshot(const std::filesystem::path&, std::string&, const std::function<bool()>&) { Unreachable("usage::ReadReceiptSnapshot"); }
	std::optional<AntigravityUsageCandidate> DecodeAntigravityAccountingRecord(const Record&) { Unreachable("usage::DecodeAntigravityAccountingRecord"); }
}

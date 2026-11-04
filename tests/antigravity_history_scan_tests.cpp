#include "test_harness.h"
#include "common/usage/antigravity_history_scan.h"
#include "common/usage/usage_history_service.h"
#include "common/usage/usage_collector.h"
#include <thread>
#include <sqlite3.h>

using namespace uam_test;
namespace
{
	std::string ProtoNumber(std::uint32_t field,std::uint64_t value)
	{
		const std::function<std::string(std::uint64_t)> encode=[](std::uint64_t input)
		{
			std::string bytes;
			do { bytes.push_back(static_cast<char>((input&127)|(input>127?128:0))); input>>=7; } while(input);
			return bytes;
		};
		return encode(field*8)+encode(value);
	}
	std::string ProtoBytes(std::uint32_t field,const std::string& bytes)
	{
		std::string header=ProtoNumber(field,bytes.size()); header[0]=static_cast<char>(static_cast<unsigned char>(header[0])+2);
		return header+bytes;
	}
	using UsageDb=std::unique_ptr<sqlite3,decltype(&sqlite3_close_v2)>;
	UsageDb CreateUsageDb(const fs::path& path)
	{
		sqlite3* raw=nullptr; UAM_ASSERT_EQ(sqlite3_open(path.string().c_str(),&raw),SQLITE_OK);
		UsageDb database(raw,sqlite3_close_v2);
		UAM_ASSERT_EQ(sqlite3_exec(raw,"PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0; CREATE TABLE gen_metadata(idx INTEGER,data BLOB); CREATE TABLE steps(idx INTEGER,metadata BLOB)",nullptr,nullptr,nullptr),SQLITE_OK);
		return database;
	}
	void WriteUsageBlob(sqlite3* database,const char* query,int index,const std::string& blob)
	{
		sqlite3_stmt* raw=nullptr; UAM_ASSERT_EQ(sqlite3_prepare_v2(database,query,-1,&raw,nullptr),SQLITE_OK);
		const std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> statement(raw,sqlite3_finalize);
		UAM_ASSERT_EQ(sqlite3_bind_int(raw,1,index),SQLITE_OK); UAM_ASSERT_EQ(sqlite3_bind_blob(raw,2,blob.data(),static_cast<int>(blob.size()),SQLITE_TRANSIENT),SQLITE_OK);
		UAM_ASSERT_EQ(sqlite3_step(raw),SQLITE_DONE);
	}
	uam::usage::ScanResult CompleteUsageScan(uam::usage::AntigravityHistoryScan& scan,const fs::path& path)
	{
		for(int batch=0;batch<20;++batch)
		{
			const uam::usage::ScanResult result=scan.Refresh(path);
			UAM_ASSERT(result.error.empty()); UAM_ASSERT(!result.cancelled);
			if(result.complete) return result;
		}
		UAM_ASSERT(false); return {};
	}
}

UAM_TEST(UsageAntigravitySqliteMergesGenerationStepRetriesAndReportsCorruptMetadataWithoutWritingHistory)
{
	TempDir temp("uam-antigravity-sqlite"); const fs::path path=temp.root/"conversation.db"; UsageDb database=CreateUsageDb(path);
	const std::string usage=ProtoNumber(2,10)+ProtoNumber(3,4)+ProtoBytes(11,"response"), stamp=ProtoNumber(1,1780000000);
	WriteUsageBlob(database.get(),"INSERT INTO gen_metadata VALUES(?1,?2)",0,ProtoBytes(1,ProtoBytes(4,usage)+ProtoBytes(19,"Gemini 3 Pro")+ProtoBytes(9,ProtoBytes(4,stamp))));
	WriteUsageBlob(database.get(),"INSERT INTO steps VALUES(?1,?2)",0,ProtoBytes(9,usage)+ProtoBytes(8,stamp)+ProtoBytes(28,ProtoBytes(2,ProtoNumber(1,1026)+ProtoNumber(2,3)+ProtoBytes(11,"retry"))));
	WriteUsageBlob(database.get(),"INSERT INTO steps VALUES(?1,?2)",1,std::string("\x0f",1));
	const std::string main_before=ReadFile(path);
	uam::usage::AntigravityHistoryScan scan; UAM_ASSERT(CompleteUsageScan(scan,path).reset); UAM_ASSERT_EQ(scan.InvalidRows(),std::size_t(1));
	std::vector<const uam::usage::AntigravityUsageCandidate*> references;
	for(const uam::usage::AntigravityUsageCandidate& candidate:scan.Candidates()) references.push_back(&candidate);
	const std::optional<std::vector<uam::usage::Record>> records=uam::usage::MergeAntigravityCandidates(references);
	UAM_ASSERT(records); UAM_ASSERT_EQ(records->size(),std::size_t(2));
	std::int64_t tokens=0; for(const uam::usage::Record& record:*records) tokens+=record.tokens.Total(); UAM_ASSERT_EQ(tokens,17LL);
	UAM_ASSERT_EQ(ReadFile(path),main_before); const uam::usage::ScanResult cached=scan.Refresh(path); UAM_ASSERT(cached.complete); UAM_ASSERT_EQ(cached.bytes_read,std::size_t(0));
}

UAM_TEST(UsageAntigravitySqliteSnapshotStaysConsistentAcrossBatchesAndDetectsWalWritesWithRestoredMtime)
{
	TempDir temp("uam-antigravity-snapshot"); const fs::path path=temp.root/"conversation.db"; UsageDb database=CreateUsageDb(path);
	UAM_ASSERT_EQ(sqlite3_exec(database.get(),"BEGIN",nullptr,nullptr,nullptr),SQLITE_OK);
	for(int index=0;index<600;++index) WriteUsageBlob(database.get(),"INSERT INTO gen_metadata VALUES(?1,?2)",index,ProtoBytes(1,ProtoBytes(4,ProtoNumber(2,1))));
	UAM_ASSERT_EQ(sqlite3_exec(database.get(),"COMMIT",nullptr,nullptr,nullptr),SQLITE_OK);
	uam::usage::AntigravityHistoryScan scan;
	UAM_ASSERT(!scan.Refresh(path).complete); // Schema probe pins the original read snapshot.
	const fs::path wal=path.string()+"-wal"; const fs::file_time_type modified=fs::last_write_time(wal);
	WriteUsageBlob(database.get(),"UPDATE gen_metadata SET data=?2 WHERE idx=?1",599,ProtoBytes(1,ProtoBytes(4,ProtoNumber(2,7))));
	fs::last_write_time(wal,modified);
	CompleteUsageScan(scan,path); UAM_ASSERT_EQ(scan.Candidates().size(),std::size_t(600));
	UAM_ASSERT_EQ(scan.Candidates().back().record.tokens.input,1LL);
	CompleteUsageScan(scan,path); UAM_ASSERT_EQ(scan.Candidates().back().record.tokens.input,7LL);
	UAM_ASSERT_EQ(scan.Refresh(path).bytes_read,std::size_t(0));
}

UAM_TEST(UsageAntigravitySqliteCancellationAndSchemaFailurePreserveTheLastCompletedSnapshot)
{
	TempDir temp("uam-antigravity-recovery"); const fs::path path=temp.root/"conversation.db"; UsageDb database=CreateUsageDb(path);
	WriteUsageBlob(database.get(),"INSERT INTO gen_metadata VALUES(?1,?2)",0,ProtoBytes(1,ProtoBytes(4,ProtoNumber(2,10))));
	uam::usage::AntigravityHistoryScan scan; CompleteUsageScan(scan,path);
	WriteUsageBlob(database.get(),"UPDATE gen_metadata SET data=?2 WHERE idx=?1",0,ProtoBytes(1,ProtoBytes(4,ProtoNumber(2,20))));
	UAM_ASSERT(!scan.Refresh(path).complete);
	UAM_ASSERT(scan.Refresh(path,[]{return true;}).cancelled); UAM_ASSERT_EQ(scan.Candidates()[0].record.tokens.input,10LL);
	CompleteUsageScan(scan,path); UAM_ASSERT_EQ(scan.Candidates()[0].record.tokens.input,20LL);
	UAM_ASSERT_EQ(sqlite3_exec(database.get(),"DROP TABLE gen_metadata; DROP TABLE steps",nullptr,nullptr,nullptr),SQLITE_OK);
	uam::usage::ScanResult failed;
	for(int batch=0;batch<4 && failed.error.empty();++batch) failed=scan.Refresh(path);
	UAM_ASSERT(!failed.error.empty()); UAM_ASSERT_EQ(scan.Candidates()[0].record.tokens.input,20LL);
	const fs::path missing=temp.root/"missing.db"; UAM_ASSERT(!scan.Refresh(missing).error.empty()); UAM_ASSERT(!fs::exists(missing));
}


UAM_TEST(UsageAntigravityHistoryServiceCollectsCompletedCachesAndRefreshesMutableAccounting)
{
	TempDir temp("uam-antigravity-history-service");
	const fs::path path = temp.root/"conversation.db"; UsageDb database = CreateUsageDb(path);
	const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	const std::string timestamp = ProtoNumber(1,now/1000-2);
	const std::string usage = ProtoNumber(2,10)+ProtoNumber(3,4)+ProtoBytes(11,"response");
	WriteUsageBlob(database.get(),"INSERT INTO gen_metadata VALUES(?1,?2)",0,ProtoBytes(1,ProtoBytes(4,usage)+ProtoBytes(19,"Gemini 3 Pro")+ProtoBytes(9,ProtoBytes(4,timestamp))));
	WriteUsageBlob(database.get(),"INSERT INTO steps VALUES(?1,?2)",0,ProtoBytes(9,usage)+ProtoBytes(8,timestamp)+ProtoBytes(28,ProtoBytes(2,ProtoNumber(1,1026)+ProtoNumber(2,3)+ProtoBytes(11,"retry"))));
	WriteUsageBlob(database.get(),"INSERT INTO steps VALUES(?1,?2)",1,std::string("\x0f",1));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root/"ignored.jsonl","not an Antigravity SQLite history"));
	std::error_code link_error; fs::create_symlink(path,temp.root/"linked.db",link_error);
	uam::usage::HistoryService history({{uam::provider_ids::kAntigravityCli,{temp.root},{path}}});
	nlohmann::json progress;
	for (int batch=0;batch<20;++batch)
	{
		progress=history.Refresh("first",now-86400000,now+1000); UAM_ASSERT(!progress.contains("error"));
		if (progress.value("complete",false)) break;
	}
	UAM_ASSERT(progress.value("complete",false)); UAM_ASSERT_EQ(progress["filesTotal"].get<int>(),1);
	UAM_ASSERT_EQ(progress["totals"]["totalTokens"].get<int>(),17);
	UAM_ASSERT(progress["providers"][5]["available"].get<bool>()); UAM_ASSERT_EQ(progress["providers"][5]["invalidLines"].get<int>(),1);
	UAM_ASSERT_EQ(history.Refresh("unchanged",now-86400000,now+1000)["bytesRead"].get<int>(),0);
	uam::usage::Store store(temp.root/"accounting");
	const uam::usage::AccountingSink sink = [&](const std::string& source,const std::vector<uam::usage::Record>& records,std::size_t position,std::string& error)
	{
		std::vector<uam::usage::SourceRecord> rows;
		for (std::size_t index=position;index<records.size();++index) rows.push_back({&records[index],source,index});
		return store.Import(rows,error);
	};
	progress=history.Collect("account",now-86400000,now+1000,sink); UAM_ASSERT(!progress.contains("error")); UAM_ASSERT(progress.value("complete",false));
	std::string error;
	const std::optional<uam::usage::Ledger> first_ledger=store.Read(now+1000,error); UAM_ASSERT(first_ledger);
	const std::optional<uam::usage::Summary> first_total=first_ledger->Query(first_ledger->WindowStart(1),now+1000); UAM_ASSERT(first_total);
	UAM_ASSERT_EQ(first_total->total.tokens.Total(),17LL);
	WriteUsageBlob(database.get(),"UPDATE gen_metadata SET data=?2 WHERE idx=?1",0,ProtoBytes(1,ProtoBytes(4,ProtoNumber(2,14)+ProtoNumber(3,4)+ProtoBytes(11,"response"))+ProtoBytes(19,"Gemini 3 Pro")+ProtoBytes(9,ProtoBytes(4,timestamp))));
	for (int batch=0;batch<20;++batch)
	{
		progress=history.Collect("changed",now-86400000,now+1000,sink); UAM_ASSERT(!progress.contains("error"));
		if (progress.value("complete",false)) break;
	}
	UAM_ASSERT(progress.value("complete",false));
	const std::optional<uam::usage::Ledger> changed_ledger=store.Read(now+1000,error); UAM_ASSERT(changed_ledger);
	const std::optional<uam::usage::Summary> changed_total=changed_ledger->Query(changed_ledger->WindowStart(1),now+1000); UAM_ASSERT(changed_total);
	UAM_ASSERT_EQ(changed_total->total.tokens.Total(),21LL);
	const std::vector<uam::usage::HistorySource> sources{{uam::provider_ids::kAntigravityCli,{temp.root},{path}}};
	uam::usage::Collector collector(temp.root/"collector",sources,uam::usage::RateFetcher{},true);
	const std::chrono::steady_clock::time_point deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
	nlohmann::json view;
	do
	{
		view=collector.Query(1);
		if (view.contains("totals") && !view.value("collecting",true)) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	} while (std::chrono::steady_clock::now()<deadline);
	UAM_ASSERT(view.contains("totals")); UAM_ASSERT_EQ(view["totals"]["totalTokens"].get<int>(),21);
	UAM_ASSERT(collector.AccountingSnapshot());
}

UAM_TEST(UsageAntigravityHistoryRootsUseConfiguredDirectoriesAndNestedConversations)
{
	TempDir temp("uam-antigravity-roots");
	const fs::path root=temp.root/"configured", nested=root/"conversations";
	fs::create_directories(nested);
	ScopedEnvVar home("HOME",temp.root.string()); ScopedEnvVar user_profile("USERPROFILE",temp.root.string());
	ScopedEnvVar xdg("XDG_DATA_HOME",(temp.root/"xdg").string()); ScopedEnvVar opencode("OPENCODE_DB",(temp.root/"missing.db").string());
	ScopedEnvVar configured("ANTIGRAVITY_DATA_DIR",root.string()+", ~/alternate , relative-is-ignored");
	const std::vector<uam::usage::HistorySource> sources=uam::usage::LocalHistorySources();
	const std::vector<uam::usage::HistorySource>::const_iterator found=std::find_if(sources.begin(),sources.end(),[](const uam::usage::HistorySource& source) { return source.provider_id==uam::provider_ids::kAntigravityCli; });
	UAM_ASSERT(found!=sources.end()); UAM_ASSERT_EQ(found->directories.size(),std::size_t(2));
	UAM_ASSERT_EQ(found->directories[0],nested); UAM_ASSERT_EQ(found->directories[1],temp.root/"alternate");
}

UAM_TEST(UsageAntigravityStreamingPassesTheOldRowLimitWithoutRetainingCandidateCaches)
{
	TempDir temp("uam-antigravity-streamed-large"); const fs::path path = temp.root / "conversation.db"; UsageDb database = CreateUsageDb(path);
	UAM_ASSERT_EQ(sqlite3_exec(database.get(), "BEGIN", nullptr, nullptr, nullptr), SQLITE_OK);
	const std::string blob = ProtoBytes(1, ProtoBytes(4, ProtoNumber(2, 1)));
	sqlite3_stmt* raw = nullptr; UAM_ASSERT_EQ(sqlite3_prepare_v2(database.get(), "INSERT INTO gen_metadata VALUES(?1,?2)", -1, &raw, nullptr), SQLITE_OK);
	for (int index = 0; index < 100001; ++index)
	{
		sqlite3_reset(raw); sqlite3_bind_int(raw, 1, index); sqlite3_bind_blob(raw, 2, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT); UAM_ASSERT_EQ(sqlite3_step(raw), SQLITE_DONE);
	}
	sqlite3_finalize(raw); UAM_ASSERT_EQ(sqlite3_exec(database.get(), "COMMIT", nullptr, nullptr, nullptr), SQLITE_OK);
	uam::usage::AntigravityHistoryScan scan; std::size_t emitted = 0;
	const std::function<bool(const std::vector<uam::usage::Record>&, std::string&)> sink = [&](const std::vector<uam::usage::Record>& records, std::string&)
	{ emitted += records.size(); UAM_ASSERT(records.size() <= 257); return true; };
	uam::usage::ScanResult result;
	for (int batch = 0; batch < 210; ++batch) { result = scan.Refresh(path, {}, sink); UAM_ASSERT(result.error.empty()); if (result.complete) break; }
	UAM_ASSERT(result.complete); UAM_ASSERT_EQ(emitted, std::size_t{100001}); UAM_ASSERT(scan.Candidates().empty());
	UAM_ASSERT(scan.Refresh(path, {}, sink).complete); UAM_ASSERT_EQ(emitted, std::size_t{100001});
}

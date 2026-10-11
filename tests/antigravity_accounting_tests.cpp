#include "test_harness.h"
#include "common/usage/antigravity_accounting.h"
#include "common/usage/usage_store.h"

using namespace uam_test;
namespace
{
	std::string Varint(std::uint64_t value)
	{
		std::string bytes;
		do { bytes.push_back(static_cast<char>((value & 127) | (value > 127 ? 128 : 0))); value >>= 7; } while (value);
		return bytes;
	}
	std::string Number(std::uint32_t field, std::uint64_t value) { return Varint(field * 8) + Varint(value); }
	std::string Bytes(std::uint32_t field, const std::string& bytes) { return Varint(field * 8 + 2) + Varint(bytes.size()) + bytes; }
	const std::string usage = Number(2,100) + Number(3,40) + Number(4,5) + Number(5,20) + Number(9,10) + Bytes(11,"response") + Bytes(12,"request");
	const std::string stamp = Number(1,1780000000) + Number(2,123456789);
}

UAM_TEST(UsageAntigravityMetadataPreservesRetriesBucketsModelSwitchesAndAccountingIdentities)
{
	const std::string generation = Bytes(1,Bytes(4,usage) + Bytes(19,"Gemini 3 Pro") + Bytes(9,Bytes(4,stamp)));
	const std::string retry = Number(1,1026) + Number(2,12) + Number(3,3) + Bytes(11,"retry");
	const std::string step = Bytes(9,usage) + Bytes(8,stamp) + Bytes(28,Bytes(2,retry));
	const std::optional<uam::usage::AntigravityMetadata> first = uam::usage::DecodeAntigravityMetadata(generation,"session",0,false,1);
	const std::optional<uam::usage::AntigravityMetadata> second = uam::usage::DecodeAntigravityMetadata(step,"session",0,true,2,{},first ? first->model : "");
	UAM_ASSERT(first && second); UAM_ASSERT_EQ(first->candidates.size(),std::size_t(1)); UAM_ASSERT_EQ(second->candidates.size(),std::size_t(2));
	const uam::usage::Record& record = first->candidates[0].record;
	UAM_ASSERT_EQ(record.model_id,std::string("gemini-3-pro")); UAM_ASSERT_EQ(record.timestamp_ms,1780000000123LL);
	UAM_ASSERT_EQ(record.tokens.input,100LL); UAM_ASSERT_EQ(record.tokens.cached_input,20LL); UAM_ASSERT_EQ(record.tokens.cache_write,5LL);
	UAM_ASSERT_EQ(record.tokens.output,40LL); UAM_ASSERT_EQ(record.tokens.reasoning,10LL); UAM_ASSERT_EQ(record.tokens.Total(),165LL);
	UAM_ASSERT_EQ(record.dedupe_key,second->candidates[0].record.dedupe_key); UAM_ASSERT_EQ(first->candidates[0].aliases.size(),std::size_t(2));
	UAM_ASSERT_EQ(second->candidates[1].record.model_id,std::string("claude-opus-4-6")); UAM_ASSERT_EQ(second->candidates[1].record.tokens.input,12LL);
	UAM_ASSERT(!record.reported_cost_usd && !record.estimated_cost_usd);
	const std::string identity = nlohmann::json::array({"native",record.provider_id,record.dedupe_key}).dump();
	UAM_ASSERT(uam::usage::DecodeAccountingRecord(uam::usage::EncodeAccountingRecord({identity,record})).has_value());
}

UAM_TEST(UsageAntigravityMetadataKeepsUnknownModelsAndStableRetrySlotsWithoutInventingTokens)
{
	const std::string no_usage_retry = Bytes(28,Bytes(1,"no-usage"));
	const std::string retry = Bytes(28,Bytes(2,Number(1,999999) + Number(2,7)));
	const std::optional<uam::usage::AntigravityMetadata> before = uam::usage::DecodeAntigravityMetadata(no_usage_retry + retry,"session",8,true,10);
	const std::optional<uam::usage::AntigravityMetadata> after = uam::usage::DecodeAntigravityMetadata(Bytes(9,Number(2,5)) + no_usage_retry + retry,"session",8,true,10);
	UAM_ASSERT(before && after); UAM_ASSERT_EQ(before->candidates.size(),std::size_t(1)); UAM_ASSERT_EQ(after->candidates.size(),std::size_t(2));
	UAM_ASSERT_EQ(before->candidates[0].record.dedupe_key,after->candidates[1].record.dedupe_key);
	UAM_ASSERT_EQ(before->candidates[0].record.model_id,std::string("antigravity-model-999999")); UAM_ASSERT_EQ(before->candidates[0].timestamp_quality,0);
	const std::optional<uam::usage::AntigravityMetadata> empty = uam::usage::DecodeAntigravityMetadata(Bytes(24,Bytes(12,"Claude 4.6 Opus (Thinking)")),"session",0,true,10);
	UAM_ASSERT(empty); UAM_ASSERT(empty->candidates.empty()); UAM_ASSERT_EQ(empty->model,std::string("claude-opus-4-6"));
	const std::optional<uam::usage::AntigravityMetadata> fallback = uam::usage::DecodeAntigravityMetadata(Bytes(9,Number(9,12)+Number(10,4)),"session",1,true,10,20,"claude-sonnet-4-6");
	UAM_ASSERT(fallback); UAM_ASSERT_EQ(fallback->candidates[0].record.timestamp_ms,20LL); UAM_ASSERT_EQ(fallback->candidates[0].record.tokens.output,16LL); UAM_ASSERT_EQ(fallback->candidates[0].record.tokens.reasoning,12LL);
}

UAM_TEST(UsageAntigravityMetadataRejectsMalformedWireUtf8CountersAndRetryBounds)
{
	for (const std::string& malformed : {std::string("\0",1),std::string("\x0f",1),std::string("\x0a\x80",2),Bytes(9,Number(2,std::uint64_t(uam::usage::kMaxExactTokenCount)+1)),Bytes(9,Number(2,uam::usage::kMaxExactTokenCount)+Number(3,1)),
	    Bytes(9,Number(9,uam::usage::kMaxExactTokenCount)+Number(10,1)),Bytes(9,usage)+Bytes(24,Bytes(12,std::string("\xff",1))),Bytes(8,Number(1,1)+Number(2,1000000000)),std::string(8*1024*1024+1,'x')})
		UAM_ASSERT(!uam::usage::DecodeAntigravityMetadata(malformed,"session",0,true,10));
	std::string too_many;
	for (int index=0;index<257;++index) too_many+=Bytes(28,Bytes(2,Number(2,1)));
	UAM_ASSERT(!uam::usage::DecodeAntigravityMetadata(too_many,"session",0,true,10));
	UAM_ASSERT(!uam::usage::DecodeAntigravityMetadata("","session",0,false,10));
	UAM_ASSERT(!uam::usage::DecodeAntigravityMetadata(Bytes(9,usage),"session",-1,true,10));
	std::optional<std::int64_t> trajectory;
	UAM_ASSERT(uam::usage::DecodeAntigravityTrajectoryTimestamp(Bytes(2,stamp),trajectory)); UAM_ASSERT_EQ(*trajectory,1780000000123LL);
	UAM_ASSERT(uam::usage::DecodeAntigravityTrajectoryTimestamp("",trajectory)); UAM_ASSERT(!trajectory);
	UAM_ASSERT(!uam::usage::DecodeAntigravityTrajectoryTimestamp(std::string("\x0f",1),trajectory));
}

UAM_TEST(UsageAntigravityAliasesMergeTransitivelyBeforeFilteringAndKeepReliableTimestamps)
{
	const std::optional<uam::usage::AntigravityMetadata> step = uam::usage::DecodeAntigravityMetadata(Bytes(9,Number(2,100)+Bytes(11,"response")),"copy-session",1,true,2000);
	const std::optional<uam::usage::AntigravityMetadata> generation = uam::usage::DecodeAntigravityMetadata(Bytes(1,Bytes(4,Number(3,40)+Bytes(12,"request"))+Bytes(19,"Gemini 3 Pro")),"session",1,false,3000);
	const std::optional<uam::usage::AntigravityMetadata> bridge = uam::usage::DecodeAntigravityMetadata(Bytes(9,Number(2,50)+Bytes(11,"response")+Bytes(12,"request"))+Bytes(8,Number(1,1)),"session",2,true,4000);
	UAM_ASSERT(step && generation && bridge);
	const std::optional<std::vector<uam::usage::Record>> records = uam::usage::MergeAntigravityCandidates({&step->candidates[0],&generation->candidates[0],&bridge->candidates[0]});
	UAM_ASSERT(records); UAM_ASSERT_EQ(records->size(),std::size_t(1));
	UAM_ASSERT_EQ(records->front().tokens.input,100LL); UAM_ASSERT_EQ(records->front().tokens.output,40LL); UAM_ASSERT_EQ(records->front().timestamp_ms,1000LL);
	UAM_ASSERT_EQ(records->front().model_id,std::string("gemini-3-pro"));
	const std::optional<uam::usage::AntigravityMetadata> unknown_retry = uam::usage::DecodeAntigravityMetadata(Bytes(24,Bytes(12,"Gemini 3 Pro"))+Bytes(28,Bytes(2,Number(1,999999)+Number(2,1))),"session",0,true,10);
	UAM_ASSERT(unknown_retry); UAM_ASSERT_EQ(unknown_retry->candidates[0].record.model_id,std::string("antigravity-model-999999"));
}

UAM_TEST(UsageAntigravityDurableAliasesSurviveExpansionRestartAndMachineCopies)
{
	TempDir temp("uam-antigravity-durable-aliases");
	const std::int64_t end = 1780000005000LL;
	const std::string native_usage = Number(2,20)+Bytes(11,"response")+Bytes(12,"request");
	const std::optional<uam::usage::AntigravityMetadata> no_id = uam::usage::DecodeAntigravityMetadata(Bytes(9,Number(2,10)),"session",1,true,end-500);
	const std::optional<uam::usage::AntigravityMetadata> request = uam::usage::DecodeAntigravityMetadata(Bytes(9,Number(2,15)+Bytes(12,"request")),"session",1,true,end-400);
	const std::optional<uam::usage::AntigravityMetadata> expanded = uam::usage::DecodeAntigravityMetadata(Bytes(9,native_usage)+Bytes(8,stamp),"session",1,true,end-300);
	const std::optional<uam::usage::AntigravityMetadata> remote = uam::usage::DecodeAntigravityMetadata(Bytes(9,Number(2,18)+Bytes(11,"response")),"copy-session",8,true,end-100);
	UAM_ASSERT(no_id && request && expanded && remote);
	const std::optional<uam::usage::Record> first = uam::usage::EncodeAntigravityAccountingRecord(no_id->candidates[0]);
	const std::optional<uam::usage::Record> second = uam::usage::EncodeAntigravityAccountingRecord(request->candidates[0]);
	const std::optional<uam::usage::Record> third = uam::usage::EncodeAntigravityAccountingRecord(expanded->candidates[0]);
	const std::optional<uam::usage::Record> peer = uam::usage::EncodeAntigravityAccountingRecord(remote->candidates[0]);
	UAM_ASSERT(first && second && third && peer);
	std::string error;
	{
		uam::usage::Store store(temp.root);
		UAM_ASSERT(store.Import({{&*first,"source",0}},error));
		UAM_ASSERT(store.Import({{&*second,"source",0}},error));
		UAM_ASSERT(store.Import({{&*third,"source",0}},error));
	}
	uam::usage::Store reopened(temp.root);
	const std::optional<std::vector<uam::usage::AccountingRecord>> facts = reopened.ReadRecords(end,error);
	UAM_ASSERT(facts); UAM_ASSERT_EQ(facts->size(),std::size_t(3));
	std::vector<uam::usage::AccountingRecord> transferred;
	for (const uam::usage::AccountingRecord& fact : *facts)
	{
		const std::optional<uam::usage::AccountingRecord> decoded = uam::usage::DecodeAccountingRecord(uam::usage::EncodeAccountingRecord(fact));
		UAM_ASSERT(decoded); transferred.push_back(*decoded);
	}
	transferred.push_back({nlohmann::json::array({"native",peer->provider_id,peer->dedupe_key}).dump(),*peer});
	std::vector<const uam::usage::Record*> references;
	for (const uam::usage::AccountingRecord& fact : transferred) references.push_back(&fact.record);
	const std::optional<uam::usage::Ledger> all = uam::usage::Ledger::Create(references,end);
	UAM_ASSERT(all);
	const std::optional<uam::usage::Summary> total = all->Query(all->WindowStart(1),end);
	UAM_ASSERT(total); UAM_ASSERT_EQ(total->total.records,std::size_t(1)); UAM_ASSERT_EQ(total->total.tokens.input,20LL); UAM_ASSERT_EQ(total->duplicates,std::size_t(3));
	const std::optional<uam::usage::Summary> after_real_time = all->Query(1780000000124LL,end);
	UAM_ASSERT(after_real_time); UAM_ASSERT_EQ(after_real_time->total.tokens.Total(),0LL);
	const std::optional<uam::usage::Ledger> local = reopened.Read(end,error);
	UAM_ASSERT(local); UAM_ASSERT_EQ(local->Query(local->WindowStart(1),end)->total.tokens.input,20LL);
}

UAM_TEST(UsageAntigravityDurableAliasesMergeBeforeDailyCohortsAndRejectInvalidEnvelopes)
{
	const std::int64_t end = 1780000005000LL;
	const std::optional<uam::usage::AntigravityMetadata> decoded = uam::usage::DecodeAntigravityMetadata(Bytes(9,Number(2,4)+Bytes(12,"request")),"session",1,true,end-100);
	UAM_ASSERT(decoded);
	uam::usage::AntigravityUsageCandidate reliable = decoded->candidates[0];
	reliable.timestamp_quality = 2; reliable.record.timestamp_ms = end-2*86400000;
	const std::optional<uam::usage::Record> old = uam::usage::EncodeAntigravityAccountingRecord(reliable);
	const std::optional<uam::usage::Record> recent = uam::usage::EncodeAntigravityAccountingRecord(decoded->candidates[0]);
	UAM_ASSERT(old && recent);
	const std::optional<uam::usage::Ledger> ledger = uam::usage::Ledger::Create({&*recent,&*old},end);
	UAM_ASSERT(ledger); UAM_ASSERT_EQ(ledger->Query(ledger->WindowStart(1),end)->total.tokens.Total(),0LL);
	UAM_ASSERT_EQ(ledger->Query(ledger->WindowStart(90),end)->total.tokens.Total(),4LL);
	UAM_ASSERT(uam::usage::Ledger::Decode(ledger->Encode()));
	for (const nlohmann::json& invalid : {nlohmann::json::array({"antigravity-accounting-v1",3,nlohmann::json::array(),reliable.location_id}),
	    nlohmann::json::array({"antigravity-accounting-v1",0,nlohmann::json::array({"[11,\"same\"]","[11,\"other\"]"}),reliable.location_id}),
	    nlohmann::json::array({"antigravity-accounting-v1",0,nlohmann::json::array(),"[\"antigravity\",\"wrong-session\",\"step\",1,0]"})})
	{
		uam::usage::Record malformed = *recent; malformed.dedupe_key = invalid.dump();
		UAM_ASSERT(!uam::usage::Ledger::Create({&malformed},end));
	}
	uam::usage::Record negative = *recent; negative.tokens.input = -1;
	UAM_ASSERT(!uam::usage::Ledger::Create({&negative,&*old},end));
	uam::usage::Record ordinary = *recent; ordinary.provider_id = uam::provider_ids::kCodexCli; ordinary.dedupe_key = "native-event";
	const std::optional<uam::usage::Ledger> separate = uam::usage::Ledger::Create({&ordinary,&*recent},end);
	UAM_ASSERT(separate); UAM_ASSERT_EQ(separate->Query(separate->WindowStart(1),end)->total.tokens.Total(),8LL);
}

UAM_TEST(UsageAntigravityRetentionKeepsLinkedReliableEvidenceAndExpiresWholeOldGroups)
{
	TempDir temp("uam-antigravity-retention");
	const std::int64_t end = 1780000005000LL;
	const std::optional<uam::usage::AntigravityMetadata> decoded = uam::usage::DecodeAntigravityMetadata(Bytes(9,Number(2,4)+Bytes(12,"request")),"session",1,true,end-100);
	UAM_ASSERT(decoded);
	uam::usage::AntigravityUsageCandidate reliable = decoded->candidates[0];
	reliable.timestamp_quality = 2; reliable.record.timestamp_ms = end-100*86400000LL;
	const std::optional<uam::usage::Record> old = uam::usage::EncodeAntigravityAccountingRecord(reliable);
	const std::optional<uam::usage::Record> recent = uam::usage::EncodeAntigravityAccountingRecord(decoded->candidates[0]);
	const std::optional<uam::usage::AntigravityMetadata> unrelated = uam::usage::DecodeAntigravityMetadata(Bytes(9,Number(2,7)),"other-session",2,true,end-110*86400000LL);
	UAM_ASSERT(old && recent && unrelated);
	const std::optional<uam::usage::Record> expired = uam::usage::EncodeAntigravityAccountingRecord(unrelated->candidates[0]); UAM_ASSERT(expired);
	uam::usage::Store store(temp.root); std::string error;
	UAM_ASSERT(store.Import({{&*old,"original",0},{&*recent,"copy",0},{&*expired,"expired",0}},error));
	const std::optional<std::vector<uam::usage::AccountingRecord>> retained = store.ReadRecords(end,error);
	UAM_ASSERT(retained); UAM_ASSERT_EQ(retained->size(),std::size_t(2));
	UAM_ASSERT_EQ(store.Read(end,error)->Query(end-86400000,end)->total.tokens.Total(),0LL);
	const std::optional<std::vector<uam::usage::AccountingRecord>> restarted = uam::usage::Store(temp.root).ReadRecords(end,error);
	UAM_ASSERT(restarted); UAM_ASSERT_EQ(restarted->size(),std::size_t(2));
	const std::optional<std::vector<uam::usage::AccountingRecord>> aged = store.ReadRecords(end+100*86400000LL,error);
	UAM_ASSERT(aged); UAM_ASSERT(aged->empty());
	UAM_ASSERT(store.ReadRecords(end,error)->empty()); // The expired group was removed, not only hidden from the result.
}

UAM_TEST(UsageAntigravityRetentionCarriesFutureReliableEvidenceWithoutChargingRecentCopies)
{
	TempDir temp("uam-antigravity-future-evidence");
	const std::int64_t end = 1780000005000LL;
	const std::optional<uam::usage::AntigravityMetadata> decoded = uam::usage::DecodeAntigravityMetadata(Bytes(9,Number(2,4)+Bytes(12,"request")),"session",1,true,end-100);
	UAM_ASSERT(decoded);
	uam::usage::AntigravityUsageCandidate reliable = decoded->candidates[0];
	reliable.timestamp_quality = 2; reliable.record.timestamp_ms = end+86400000;
	const std::optional<uam::usage::Record> future = uam::usage::EncodeAntigravityAccountingRecord(reliable);
	const std::optional<uam::usage::Record> recent = uam::usage::EncodeAntigravityAccountingRecord(decoded->candidates[0]);
	UAM_ASSERT(future && recent);
	uam::usage::Store store(temp.root); std::string error;
	UAM_ASSERT(store.Import({{&*future,"original",0},{&*recent,"copy",0}},error));
	UAM_ASSERT_EQ(store.ReadRecords(end,error)->size(),std::size_t(2));
	UAM_ASSERT_EQ(store.Read(end,error)->Query(end-86400000,end)->total.tokens.Total(),0LL);
	const std::int64_t later = end+2*86400000;
	const std::optional<uam::usage::Ledger> settled = store.Read(later,error);
	UAM_ASSERT(settled); UAM_ASSERT_EQ(settled->Query(settled->WindowStart(7),later)->total.tokens.Total(),4LL);
}

UAM_TEST(UsageAntigravityDurableAliasesKeepStepModelPriorityAfterStoreOrdering)
{
	const std::int64_t end=1780000005000LL;
	const std::string native=Number(2,4)+Bytes(12,"request");
	const std::optional<uam::usage::AntigravityMetadata> generation=uam::usage::DecodeAntigravityMetadata(Bytes(1,Bytes(4,native)+Bytes(19,"Gemini 3 Pro")),"session",1,false,end-100);
	const std::optional<uam::usage::AntigravityMetadata> step=uam::usage::DecodeAntigravityMetadata(Bytes(9,native)+Bytes(24,Bytes(12,"Claude 4.6 Opus")),"session",1,true,end-100);
	UAM_ASSERT(generation && step);
	const std::optional<uam::usage::Record> first=uam::usage::EncodeAntigravityAccountingRecord(generation->candidates[0]);
	const std::optional<uam::usage::Record> second=uam::usage::EncodeAntigravityAccountingRecord(step->candidates[0]); UAM_ASSERT(first && second);
	for (const std::vector<const uam::usage::Record*>& order : {std::vector<const uam::usage::Record*>{&*first,&*second},std::vector<const uam::usage::Record*>{&*second,&*first}})
	{
		const std::optional<uam::usage::Ledger> ledger=uam::usage::Ledger::Create(order,end); UAM_ASSERT(ledger);
		const std::optional<uam::usage::Summary> total=ledger->Query(ledger->WindowStart(1),end); UAM_ASSERT(total);
		UAM_ASSERT_EQ(total->models.size(),std::size_t(1)); UAM_ASSERT_EQ(std::get<1>(total->models.begin()->first),std::string("claude-opus-4-6"));
	}
	const std::optional<uam::usage::AntigravityMetadata> unknown=uam::usage::DecodeAntigravityMetadata(Bytes(9,Number(1,999999)+native),"session",1,true,end-100); UAM_ASSERT(unknown);
	const std::optional<uam::usage::Record> unknown_record=uam::usage::EncodeAntigravityAccountingRecord(unknown->candidates[0]); UAM_ASSERT(unknown_record);
	const std::optional<uam::usage::Ledger> unpriced=uam::usage::Ledger::Create({&*first,&*unknown_record},end); UAM_ASSERT(unpriced);
	const std::optional<uam::usage::Summary> total=unpriced->Query(unpriced->WindowStart(1),end); UAM_ASSERT(total);
	UAM_ASSERT_EQ(std::get<1>(total->models.begin()->first),std::string("antigravity-model-999999"));
	const std::optional<uam::usage::AntigravityMetadata> unknown_generation=uam::usage::DecodeAntigravityMetadata(Bytes(1,Bytes(4,Number(1,999999)+native)),"session",1,false,end-100);
	const std::optional<uam::usage::AntigravityMetadata> unnamed_step=uam::usage::DecodeAntigravityMetadata(Bytes(9,native),"session",1,true,end-100); UAM_ASSERT(unknown_generation && unnamed_step);
	const std::optional<uam::usage::Record> known_id=uam::usage::EncodeAntigravityAccountingRecord(unknown_generation->candidates[0]);
	const std::optional<uam::usage::Record> unnamed=uam::usage::EncodeAntigravityAccountingRecord(unnamed_step->candidates[0]); UAM_ASSERT(known_id && unnamed);
	const std::optional<uam::usage::Ledger> identified_unknown=uam::usage::Ledger::Create({&*known_id,&*unnamed},end); UAM_ASSERT(identified_unknown);
	const std::optional<uam::usage::Summary> retained_id=identified_unknown->Query(identified_unknown->WindowStart(1),end); UAM_ASSERT(retained_id);
	UAM_ASSERT_EQ(std::get<1>(retained_id->models.begin()->first),std::string("antigravity-model-999999"));
}


UAM_TEST(UsageAntigravityRegeneratedRowsKeepDistinctRequestsWithoutProvisionalBridges)
{
	TempDir temp("uam-antigravity-regenerated-rows"); const std::int64_t end=1780000005000LL;
	const std::optional<uam::usage::AntigravityMetadata> provisional=uam::usage::DecodeAntigravityMetadata(Bytes(9,Number(2,20)),"session",0,true,end-500);
	const std::optional<uam::usage::AntigravityMetadata> generation=uam::usage::DecodeAntigravityMetadata(Bytes(1,Bytes(4,Number(2,4)+Bytes(12,"first-request"))+Bytes(19,"Gemini 3 Pro")),"session",0,false,end-400);
	const std::optional<uam::usage::AntigravityMetadata> first=uam::usage::DecodeAntigravityMetadata(Bytes(9,Number(2,4)+Bytes(12,"first-request")),"session",0,true,end-300,{},"gemini-3-pro");
	const std::optional<uam::usage::AntigravityMetadata> regenerated=uam::usage::DecodeAntigravityMetadata(Bytes(9,Number(2,7)+Bytes(12,"second-request")),"session",0,true,end-200,{},"gemini-3-pro");
	UAM_ASSERT(provisional && generation && first && regenerated);
	std::vector<uam::usage::Record> receipts;
	for (const uam::usage::AntigravityUsageCandidate* candidate : {&provisional->candidates[0],&generation->candidates[0],&regenerated->candidates[0],&first->candidates[0]})
	{
		const std::optional<uam::usage::Record> encoded=uam::usage::EncodeAntigravityAccountingRecord(*candidate); UAM_ASSERT(encoded); receipts.push_back(*encoded);
	}
	std::vector<const uam::usage::Record*> references;
	std::vector<uam::usage::SourceRecord> rows;
	for (std::size_t index=0;index<receipts.size();++index) { references.push_back(&receipts[index]); rows.push_back({&receipts[index],"source",index}); }
	const std::optional<uam::usage::Ledger> ledger=uam::usage::Ledger::Create(references,end); UAM_ASSERT(ledger);
	const std::optional<uam::usage::Summary> total=ledger->Query(ledger->WindowStart(1),end); UAM_ASSERT(total);
	UAM_ASSERT_EQ(total->total.records,std::size_t(2)); UAM_ASSERT_EQ(total->total.tokens.input,11LL);
	UAM_ASSERT_EQ(total->duplicates,std::size_t(2));
	uam::usage::Store store(temp.root); std::string error; UAM_ASSERT(store.Import(rows,error));
	const std::optional<uam::usage::Ledger> restored=uam::usage::Store(temp.root).Read(end,error); UAM_ASSERT(restored);
	const std::optional<uam::usage::Summary> durable=restored->Query(restored->WindowStart(1),end); UAM_ASSERT(durable);
	UAM_ASSERT_EQ(durable->total.records,std::size_t(2)); UAM_ASSERT_EQ(durable->total.tokens.input,11LL);
	uam::usage::AntigravityUsageCandidate old_time=provisional->candidates[0]; old_time.timestamp_quality=2; old_time.record.timestamp_ms=end-100*86400000LL;
	const std::optional<uam::usage::Record> witness=uam::usage::EncodeAntigravityAccountingRecord(old_time); UAM_ASSERT(witness);
	const std::optional<uam::usage::Ledger> with_witness=uam::usage::Ledger::Create({&*witness,&receipts[1],&receipts[2],&receipts[3]},end); UAM_ASSERT(with_witness);
	const std::optional<uam::usage::Summary> current=with_witness->Query(with_witness->WindowStart(1),end); UAM_ASSERT(current);
	UAM_ASSERT_EQ(current->total.records,std::size_t(1)); UAM_ASSERT_EQ(current->total.tokens.input,7LL);
}

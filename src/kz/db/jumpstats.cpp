#include "kz_db.h"
#include "kz/jumpstats/kz_jumpstats.h"
#include "queries/jumpstats.h"
#include "vendor/sql_mm/src/public/sql_mm.h"

#include <cmath>

using namespace KZ::Database;

void KZDatabaseService::SaveJumpstatPB(u64 steamID64, const char *jumpUUID, JumpType jumpType, i32 modeID, bool isBlock, f32 distance, i32 block,
									   i32 strafes, f32 sync, f32 pre, f32 max, f32 airtime)
{
	if (!IsReady())
	{
		return;
	}

	i32 distInt = (i32)round(distance * JS_DB_SCALE);
	i32 syncInt = (i32)round(sync * JS_DB_SCALE);
	i32 preInt = (i32)round(pre * JS_DB_SCALE);
	i32 maxInt = (i32)round(max * JS_DB_SCALE);
	i32 airInt = (i32)round(airtime * JS_DB_SCALE);
	std::string uuid = jumpUUID;

	char query[1024];
	V_snprintf(query, sizeof(query), sql_jumpstats_getrecord, steamID64, jumpType, modeID, (i32)isBlock);
	Transaction txn;
	txn.queries.push_back(query);

	auto onSuccess = [=](std::vector<ISQLQuery *> queries)
	{
		bool isPB = true;
		ISQLResult *result = queries[0]->GetResultSet();
		if (result && result->GetRowCount() > 0 && result->FetchRow())
		{
			i32 bestDist = result->GetInt(1);
			i32 bestBlock = result->GetInt(2);
			isPB = isBlock ? (block > bestBlock || (block == bestBlock && distInt > bestDist)) : (distInt > bestDist);
		}
		if (!isPB)
		{
			return;
		}

		char insertQuery[1024];
		V_snprintf(insertQuery, sizeof(insertQuery), sql_jumpstats_insert, uuid.c_str(), steamID64, jumpType, modeID, distInt, (i32)isBlock, block,
				   strafes, syncInt, preInt, maxInt, airInt);
		Transaction insertTxn;
		insertTxn.queries.push_back(insertQuery);

		auto onInserted = [=](std::vector<ISQLQuery *> insertQueries)
		{
			Player *player = g_pKZPlayerManager->SteamIdToPlayer(steamID64);
			CALL_FORWARD(eventListeners, OnJumpstatPB, player, jumpType, (u64)modeID, (f64)distance, (u32)block, (u32)strafes, sync, pre, max,
						 airtime);
		};
		GetDatabaseConnection()->ExecuteTransaction(insertTxn, onInserted, OnGenericTxnFailure);
	};
	GetDatabaseConnection()->ExecuteTransaction(txn, onSuccess, OnGenericTxnFailure);
}

void KZDatabaseService::QueryJumpstatTop(JumpType jumpType, i32 modeID, bool isBlock, u32 count, TransactionSuccessCallbackFunc onSuccess,
										 TransactionFailureCallbackFunc onFailure)
{
	char query[2048];
	V_snprintf(query, sizeof(query), sql_jumpstats_ranking_gettop, jumpType, modeID, (i32)isBlock, jumpType, modeID, (i32)isBlock, count);
	Transaction txn;
	txn.queries.push_back(query);
	GetDatabaseConnection()->ExecuteTransaction(txn, onSuccess, onFailure);
}

void KZDatabaseService::QueryJumpstatPBs(u64 steamID64, i32 modeID, TransactionSuccessCallbackFunc onSuccess,
										 TransactionFailureCallbackFunc onFailure)
{
	char query[2048];
	Transaction txn;
	V_snprintf(query, sizeof(query), sql_jumpstats_ranking_getpbs, steamID64, modeID);
	txn.queries.push_back(query);
	V_snprintf(query, sizeof(query), sql_jumpstats_ranking_getblockpbs, steamID64, modeID);
	txn.queries.push_back(query);
	GetDatabaseConnection()->ExecuteTransaction(txn, onSuccess, onFailure);
}

void KZDatabaseService::DeleteJump(const char *jumpUUID, TransactionSuccessCallbackFunc onSuccess, TransactionFailureCallbackFunc onFailure)
{
	std::string cleaned = GetDatabaseConnection()->Escape(jumpUUID);
	char query[512];
	V_snprintf(query, sizeof(query), sql_jumpstats_deletejump, cleaned.c_str());
	Transaction txn;
	txn.queries.push_back(query);
	GetDatabaseConnection()->ExecuteTransaction(txn, onSuccess, onFailure);
}

void KZDatabaseService::DeleteAllJumps(u64 steamID64, TransactionSuccessCallbackFunc onSuccess, TransactionFailureCallbackFunc onFailure)
{
	char query[512];
	V_snprintf(query, sizeof(query), sql_jumpstats_deleteallrecords, steamID64);
	Transaction txn;
	txn.queries.push_back(query);
	GetDatabaseConnection()->ExecuteTransaction(txn, onSuccess, onFailure);
}

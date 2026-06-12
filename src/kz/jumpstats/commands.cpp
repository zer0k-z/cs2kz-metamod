#include "../kz.h"
#include "utils/simplecmds.h"
#include "utils/tables.h"

#include "kz_jumpstats.h"
#include "kz/mode/kz_mode.h"
#include "kz/db/kz_db.h"
#include "kz/language/kz_language.h"
#include "utils/uuid.h"

#include "vendor/sql_mm/src/public/sql_mm.h"
#include "tier0/memdbgon.h"

// clang-format off
static_global const char *jstopColumnKeys[] = {
	"Jumpstats Top - Rank",
	"Jumpstats Top - Player",
	"Jumpstats Top - Distance",
	"Jumpstats Top - Block",
	"Jumpstats Top - Strafes",
	"Jumpstats Top - Sync",
	"Jumpstats Top - ID"
};

static_global const char *jstatsColumnKeys[] = {
	"Jumpstats PB - Type",
	"Jumpstats PB - Block",
	"Jumpstats PB - Distance",
	"Jumpstats PB - Strafes",
	"Jumpstats PB - Sync",
	"Jumpstats PB - ID"
};
// clang-format on

static_function bool ParseJumpType(const char *str, JumpType &out)
{
	for (i32 i = JumpType_LongJump; i <= JS_MAX_SAVED_JUMPTYPE; i++)
	{
		if (!V_stricmp(str, jumpTypeShortStr[i]) || !V_stricmp(str, jumpTypeStr[i]))
		{
			out = (JumpType)i;
			return true;
		}
	}
	return false;
}

SCMD(kz_jstop, SCFL_JUMPSTATS)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!KZDatabaseService::IsReady())
	{
		player->languageService->PrintChat(true, false, "Jumpstats - Database Not Ready");
		return MRES_SUPERCEDE;
	}

	JumpType jumpType = JumpType_LongJump;
	bool isBlock = false;
	KZModeManager::ModePluginInfo modeInfo = KZ::mode::GetModeInfo(player->modeService);

	for (i32 i = 1; i < args->ArgC(); i++)
	{
		const char *arg = args->Arg(i);
		JumpType parsedType;
		if (ParseJumpType(arg, parsedType))
		{
			jumpType = parsedType;
			continue;
		}
		if (!V_stricmp(arg, "block") || !V_stricmp(arg, "bl"))
		{
			isBlock = true;
			continue;
		}
		if (!V_stricmp(arg, "noblock") || !V_stricmp(arg, "nb"))
		{
			isBlock = false;
			continue;
		}
		KZModeManager::ModePluginInfo parsedMode = KZ::mode::GetModeInfo(CUtlString(arg));
		if (parsedMode.databaseID > 0)
		{
			modeInfo = parsedMode;
		}
	}

	if (modeInfo.databaseID <= 0)
	{
		player->languageService->PrintChat(true, false, "Jumpstats - Invalid Mode");
		return MRES_SUPERCEDE;
	}

	CPlayerUserId userID = player->GetClient()->GetUserID();
	std::string modeName = modeInfo.longModeName.Get();
	std::string typeName = jumpTypeStr[jumpType];

	auto onSuccess = [userID, jumpType, isBlock, modeName, typeName](std::vector<ISQLQuery *> queries)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
		if (!player)
		{
			return;
		}
		ISQLResult *result = queries[0]->GetResultSet();
		if (!result || result->GetRowCount() == 0)
		{
			player->languageService->PrintChat(true, false, "Jumpstats Top - No Records");
			return;
		}

		CUtlString headers[KZ_ARRAYSIZE(jstopColumnKeys)];
		for (u32 i = 0; i < KZ_ARRAYSIZE(jstopColumnKeys); i++)
		{
			headers[i] = player->languageService->PrepareMessage(jstopColumnKeys[i]).c_str();
		}
		const char *blockLabel = isBlock ? player->languageService->PrepareMessage("Jumpstats Top - Block Mode").c_str()
										 : player->languageService->PrepareMessage("Jumpstats Top - Distance Mode").c_str();
		utils::Table<KZ_ARRAYSIZE(jstopColumnKeys)> table(
			player->languageService->PrepareMessage("Jumpstats Top - Title", typeName.c_str(), modeName.c_str(), blockLabel).c_str(), headers);

		u32 row = 0;
		while (result->FetchRow())
		{
			CUtlString rank, distance, block, strafes, sync;
			rank.Format("%u", row + 1);
			distance.Format("%.4f", result->GetInt(4) / JS_DB_SCALE);
			block.Format("%i", result->GetInt(3));
			strafes.Format("%i", result->GetInt(5));
			sync.Format("%.1f%%", result->GetInt(6) / JS_DB_SCALE * 100.0);
			table.SetRow(row, rank, result->GetString(2), distance, block, strafes, sync, result->GetString(0));
			row++;
		}

		player->PrintConsole(false, false, table.GetSeparator("="));
		player->PrintConsole(false, false, table.GetTitle());
		player->PrintConsole(false, false, table.GetHeader());
		for (u32 i = 0; i < table.GetNumEntries(); i++)
		{
			player->PrintConsole(false, false, table.GetLine(i));
		}
		player->PrintConsole(false, false, table.GetSeparator("="));
		player->languageService->PrintChat(true, false, "Jumpstats Top - Check Console");
	};

	KZDatabaseService::QueryJumpstatTop(jumpType, modeInfo.databaseID, isBlock, 20, onSuccess, KZDatabaseService::OnGenericTxnFailure);
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_jumptop, kz_jstop);

SCMD(kz_jumpstats, SCFL_JUMPSTATS)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!KZDatabaseService::IsReady())
	{
		player->languageService->PrintChat(true, false, "Jumpstats - Database Not Ready");
		return MRES_SUPERCEDE;
	}

	KZModeManager::ModePluginInfo modeInfo = KZ::mode::GetModeInfo(player->modeService);
	if (args->ArgC() > 1)
	{
		KZModeManager::ModePluginInfo parsedMode = KZ::mode::GetModeInfo(CUtlString(args->Arg(1)));
		if (parsedMode.databaseID > 0)
		{
			modeInfo = parsedMode;
		}
	}
	if (modeInfo.databaseID <= 0)
	{
		player->languageService->PrintChat(true, false, "Jumpstats - Invalid Mode");
		return MRES_SUPERCEDE;
	}

	CPlayerUserId userID = player->GetClient()->GetUserID();
	std::string modeName = modeInfo.longModeName.Get();

	auto onSuccess = [userID, modeName](std::vector<ISQLQuery *> queries)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
		if (!player)
		{
			return;
		}

		CUtlString headers[KZ_ARRAYSIZE(jstatsColumnKeys)];
		for (u32 i = 0; i < KZ_ARRAYSIZE(jstatsColumnKeys); i++)
		{
			headers[i] = player->languageService->PrepareMessage(jstatsColumnKeys[i]).c_str();
		}
		utils::Table<KZ_ARRAYSIZE(jstatsColumnKeys)> table(player->languageService->PrepareMessage("Jumpstats PB - Title", modeName.c_str()).c_str(),
														   headers);

		u32 row = 0;
		// Distance PBs (non-block)
		ISQLResult *result = queries[0]->GetResultSet();
		if (result)
		{
			while (result->FetchRow())
			{
				JumpType jt = (JumpType)result->GetInt(1);
				CUtlString distance, strafes, sync;
				distance.Format("%.4f", result->GetInt(2) / JS_DB_SCALE);
				strafes.Format("%i", result->GetInt(3));
				sync.Format("%.1f%%", result->GetInt(4) / JS_DB_SCALE * 100.0);
				table.SetRow(row, jumpTypeStr[jt], "-", distance, strafes, sync, result->GetString(0));
				row++;
			}
		}
		// Block PBs
		result = queries[1]->GetResultSet();
		if (result)
		{
			while (result->FetchRow())
			{
				JumpType jt = (JumpType)result->GetInt(1);
				CUtlString block, distance, strafes, sync;
				block.Format("%i", result->GetInt(2));
				distance.Format("%.4f", result->GetInt(3) / JS_DB_SCALE);
				strafes.Format("%i", result->GetInt(4));
				sync.Format("%.1f%%", result->GetInt(5) / JS_DB_SCALE * 100.0);
				table.SetRow(row, jumpTypeStr[jt], block, distance, strafes, sync, result->GetString(0));
				row++;
			}
		}

		if (row == 0)
		{
			player->languageService->PrintChat(true, false, "Jumpstats Top - No Records");
			return;
		}

		player->PrintConsole(false, false, table.GetSeparator("="));
		player->PrintConsole(false, false, table.GetTitle());
		player->PrintConsole(false, false, table.GetHeader());
		for (u32 i = 0; i < table.GetNumEntries(); i++)
		{
			player->PrintConsole(false, false, table.GetLine(i));
		}
		player->PrintConsole(false, false, table.GetSeparator("="));
		player->languageService->PrintChat(true, false, "Jumpstats Top - Check Console");
	};

	KZDatabaseService::QueryJumpstatPBs(player->GetSteamId64(), modeInfo.databaseID, onSuccess, KZDatabaseService::OnGenericTxnFailure);
	return MRES_SUPERCEDE;
}

CON_COMMAND_F(kz_deletejump, "Delete a jumpstat from the database by its UUID.", FCVAR_NONE)
{
	if (args.ArgC() != 2)
	{
		KZ_LOG_INFO(LogChannel::DB, "Usage: kz_deletejump <UUID>\n");
		return;
	}
	if (!KZDatabaseService::IsReady())
	{
		KZ_LOG_WARN(LogChannel::DB, "Database is not ready.\n");
		return;
	}
	if (!UUID_t::FromString(args.Arg(1)))
	{
		KZ_LOG_WARN(LogChannel::DB, "Invalid UUID: %s\n", args.Arg(1));
		return;
	}
	KZDatabaseService::DeleteJump(args.Arg(1));
	KZ_LOG_INFO(LogChannel::DB, "Deleting jumpstat %s.\n", args.Arg(1));
}

CON_COMMAND_F(kz_deletealljumps, "Delete all jumpstats of a player from the database by their SteamID64.", FCVAR_NONE)
{
	if (args.ArgC() != 2)
	{
		KZ_LOG_INFO(LogChannel::DB, "Usage: kz_deletealljumps <SteamID64>\n");
		return;
	}
	if (!KZDatabaseService::IsReady())
	{
		KZ_LOG_WARN(LogChannel::DB, "Database is not ready.\n");
		return;
	}
	u64 steamID = atoll(args.Arg(1));
	if (steamID == 0)
	{
		KZ_LOG_WARN(LogChannel::DB, "Invalid SteamID64: %s\n", args.Arg(1));
		return;
	}
	KZDatabaseService::DeleteAllJumps(steamID);
	KZ_LOG_INFO(LogChannel::DB, "Deleting all jumpstats of %llu.\n", steamID);
}

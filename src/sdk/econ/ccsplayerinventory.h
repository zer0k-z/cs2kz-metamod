#pragma once
#include "utils/schema.h"
#include "utlmap.h"
#include "utils/virtual.h"
#include "base_gcmessages.pb.h"

class CEconItemView;

class CEconGameAccountClient
{
	virtual ~CEconGameAccountClient() = 0;

public:
	CSOEconGameAccountClient m_msgObject;
};

class CSharedObjectTypeCache
{
	virtual ~CSharedObjectTypeCache() = 0;

public:
	CUtlVectorFixedGrowable<CEconGameAccountClient *, 1> m_vecObjects;
	int m_nTypeID;
};

static_assert(offsetof(CSharedObjectTypeCache, m_nTypeID) == 40, "m_nTypeID offset is incorrect");

typedef uint64 itemid_t;

struct SOID_t
{
	uint64 m_id; // CSteamID
	uint32 m_type;
	uint32 m_padding;
};

struct CCSPlayerInventory
{
	void **__vtable;
	int m_nTargetRecipe;
	SOID_t m_OwnerID;

	struct
	{
		CUtlVector<CEconItemView *> m_vecInventoryItems;
		CUtlMap<itemid_t, CEconItemView *, int, CDefLess<itemid_t>> m_mapItemIDToItemView;
	} m_Items;

	int m_iPendingRequests;
	bool m_bGotItemsFromSteam;
	bool m_bHasTestItems;
	bool m_bIsListeningToSOCache;

	class CGCClientSharedObjectCache
	{
		virtual ~CGCClientSharedObjectCache() = 0;
		static const i32 TYPE_ID = 7; // k_EEconTypeGameAccountClient
		static const i32 STATE_ELEVATED = 5;

		uint64 m_ulVersion;
		CUtlVector<CSharedObjectTypeCache *> m_CacheObjects;

	public:
		CSharedObjectTypeCache *FindBaseTypeCache(int nClassID)
		{
			// Binary search through m_CacheObjects to find matching nClassID
			int left = 0;
			int right = m_CacheObjects.Count() - 1;

			FOR_EACH_VEC(m_CacheObjects, i)
			{
				CSharedObjectTypeCache *pCache = m_CacheObjects[i];
				if (pCache->m_nTypeID == nClassID)
				{
					return pCache;
				}
			}

			return nullptr; // Not found
		}

		bool HasElevatedStatus()
		{
			if (!this)
			{
				return false;
			}
			CSharedObjectTypeCache *typeCache = FindBaseTypeCache(TYPE_ID);
			if (!typeCache)
			{
				return false;
			}
			if (typeCache->m_vecObjects.Count() == 0)
			{
				return false;
			}
			return typeCache->m_vecObjects[0]->m_msgObject.elevated_state() == STATE_ELEVATED;
		}
	} *m_pSOCache;

	CUtlVector<void *> m_vecListeners;

	int m_nActiveQuestID;

	struct
	{
		itemid_t itemID;
		uint16 definitionIndex;
	} m_loadoutItems[4][57];
};

#include "kz_mode_vnlc.h"
#include "utils/interfaces.h"

#define MODE_NAME_SHORT "VNL-C"
#define MODE_NAME       "VanillaCustom"

#define SPEED_NORMAL 250.0f
#include "cs_usercmd.pb.h"
#include "utils/addresses.h"
#include "utils/interfaces.h"
#include "utils/gameconfig.h"
#include "version.h"

KZCustomVanillaModePlugin g_KZCustomVanillaModePlugin;

CGameConfig *g_pGameConfig = NULL;
KZUtils *g_pKZUtils = NULL;
KZModeManager *g_pModeManager = NULL;
ModeServiceFactory g_ModeFactory = [](KZPlayer *player) -> KZModeService * { return new KZCustomVanillaModeService(player); };
PLUGIN_EXPOSE(KZCustomVanillaModePlugin, g_KZCustomVanillaModePlugin);

bool KZCustomVanillaModePlugin::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();
	// Load mode
	int success;
	g_pModeManager = (KZModeManager *)g_SMAPI->MetaFactory(KZ_MODE_MANAGER_INTERFACE, &success, 0);
	if (success == META_IFACE_FAILED)
	{
		V_snprintf(error, maxlen, "Failed to find %s interface", KZ_MODE_MANAGER_INTERFACE);
		return false;
	}
	g_pKZUtils = (KZUtils *)g_SMAPI->MetaFactory(KZ_UTILS_INTERFACE, &success, 0);
	if (success == META_IFACE_FAILED)
	{
		V_snprintf(error, maxlen, "Failed to find %s interface", KZ_UTILS_INTERFACE);
		return false;
	}
	modules::Initialize();
	if (!interfaces::Initialize(ismm, error, maxlen))
	{
		V_snprintf(error, maxlen, "Failed to initialize interfaces");
		return false;
	}

	if (nullptr == (g_pGameConfig = g_pKZUtils->GetGameConfig()))
	{
		V_snprintf(error, maxlen, "Failed to get game config");
		return false;
	}

	if (!g_pModeManager->RegisterMode(g_PLID, MODE_NAME_SHORT, MODE_NAME, g_ModeFactory))
	{
		V_snprintf(error, maxlen, "Failed to register mode");
		return false;
	}

	return true;
}

bool KZCustomVanillaModePlugin::Unload(char *error, size_t maxlen)
{
	g_pModeManager->UnregisterMode(MODE_NAME);
	return true;
}

bool KZCustomVanillaModePlugin::Pause(char *error, size_t maxlen)
{
	g_pModeManager->UnregisterMode(MODE_NAME);
	return true;
}

bool KZCustomVanillaModePlugin::Unpause(char *error, size_t maxlen)
{
	if (!g_pModeManager->RegisterMode(g_PLID, MODE_NAME_SHORT, MODE_NAME, g_ModeFactory))
	{
		return false;
	}
	return true;
}

const char *KZCustomVanillaModePlugin::GetLicense()
{
	return "GPLv3";
}

const char *KZCustomVanillaModePlugin::GetVersion()
{
	return VERSION_STRING;
}

const char *KZCustomVanillaModePlugin::GetDate()
{
	return __DATE__;
}

const char *KZCustomVanillaModePlugin::GetLogTag()
{
	return "KZ-Mode-VanillaCustom";
}

const char *KZCustomVanillaModePlugin::GetAuthor()
{
	return "zer0.k";
}

const char *KZCustomVanillaModePlugin::GetDescription()
{
	return "Custom vanilla mode plugin for CS2KZ";
}

const char *KZCustomVanillaModePlugin::GetName()
{
	return "CS2KZ-Mode-Vanilla-Custom";
}

const char *KZCustomVanillaModePlugin::GetURL()
{
	return "https://github.com/KZGlobalTeam/cs2kz-metamod";
}

CGameEntitySystem *GameEntitySystem()
{
	return g_pKZUtils->GetGameEntitySystem();
}

/*
	Actual mode stuff.
*/

const char *KZCustomVanillaModeService::GetModeName()
{
	return MODE_NAME;
}

const char *KZCustomVanillaModeService::GetModeShortName()
{
	return MODE_NAME_SHORT;
}

DistanceTier KZCustomVanillaModeService::GetDistanceTier(JumpType jumpType, f32 distance)
{
	// No tiers given for 'Invalid' jumps.
	// clang-format off
	if (jumpType == JumpType_Invalid
		|| jumpType == JumpType_FullInvalid
		|| jumpType == JumpType_Fall
		|| jumpType == JumpType_Other
		|| distance > 500.0f)
	// clang-format on
	{
		return DistanceTier_None;
	}

	// Get highest tier distance that the jump beats
	DistanceTier tier = DistanceTier_None;
	while (tier + 1 < DISTANCETIER_COUNT && distance >= distanceTiers[jumpType][tier])
	{
		tier = (DistanceTier)(tier + 1);
	}

	return tier;
}

const char **KZCustomVanillaModeService::GetModeConVarValues()
{
	return modeCvarValues;
}

void KZCustomVanillaModeService::Reset()
{
	this->airMoving = {};
	this->tpmTriggerFixOrigins.RemoveAll();
}

static_function void ClipVelocity(Vector &in, Vector &normal, Vector &out)
{
	// Determine how far along plane to slide based on incoming direction.
	f32 backoff = DotProduct(in, normal);

	for (i32 i = 0; i < 3; i++)
	{
		f32 change = normal[i] * backoff;
		out[i] = in[i] - change;
	}

	float adjust = -0.0078125f;
	out -= (normal * adjust);
}

void KZCustomVanillaModeService::OnDuckPost()
{
	this->player->UpdateTriggerTouchList();
}

// We don't actually do anything here aside from predicting triggerfix.
#define MAX_CLIP_PLANES 5

void KZCustomVanillaModeService::OnTryPlayerMove(Vector *pFirstDest, trace_t *pFirstTrace)
{
	this->tpmTriggerFixOrigins.RemoveAll();

	Vector velocity, origin;
	int bumpcount, numbumps;
	Vector dir;
	float d;
	int numplanes;
	Vector planes[MAX_CLIP_PLANES];
	Vector primal_velocity, original_velocity;
	Vector new_velocity;
	int i, j;
	trace_t pm;
	Vector end;

	float time, time_left, allFraction;

	numbumps = 4; // Bump up to four times

	numplanes = 0; //  and not sliding along any planes

	this->player->GetOrigin(&origin);
	this->player->GetVelocity(&velocity);
	g_pKZUtils->InitGameTrace(&pm);

	VectorCopy(velocity, original_velocity); // Store original velocity
	VectorCopy(velocity, primal_velocity);

	allFraction = 0;
	time_left = g_pKZUtils->GetGlobals()->frametime; // Total time for this movement operation.

	new_velocity.Init();
	bbox_t bounds;
	this->player->GetBBoxBounds(&bounds);

	CTraceFilterPlayerMovementCS filter;
	g_pKZUtils->InitPlayerMovementTraceFilter(filter, this->player->GetPlayerPawn(),
											  this->player->GetPlayerPawn()->m_pCollision()->m_collisionAttribute().m_nInteractsWith,
											  COLLISION_GROUP_PLAYER_MOVEMENT);

	for (bumpcount = 0; bumpcount < numbumps; bumpcount++)
	{
		if (velocity.Length() == 0.0)
		{
			break;
		}
		time = time_left;
		// Assume we can move all the way from the current origin to the
		//  end point.
		VectorMA(origin, time, velocity, end);

		// If their velocity Z is 0, then we can avoid an extra trace here during WalkMove.
		if (pFirstDest && (end == *pFirstDest))
		{
			pm = *pFirstTrace;
		}
		else
		{
			g_pKZUtils->TracePlayerBBox(origin, end, bounds, &filter, pm);
		}

		allFraction += pm.m_flFraction;

		// If we moved some portion of the total distance, then
		//  copy the end position into the pmove.origin and
		//  zero the plane counter.
		if (pm.m_flFraction * velocity.Length() > 0.03125)
		{
			// There's a precision issue with terrain tracing that can cause a swept box to successfully trace
			// when the end position is stuck in the triangle.  Re-run the test with an uswept box to catch that
			// case until the bug is fixed.
			// If we detect getting stuck, don't allow the movement
			trace_t stuck;
			g_pKZUtils->InitGameTrace(&stuck);
			g_pKZUtils->TracePlayerBBox(pm.m_vEndPos, pm.m_vEndPos, bounds, &filter, stuck);
			// actually covered some distance
			origin = (pm.m_vEndPos);
			VectorCopy(velocity, original_velocity);
			numplanes = 0;
		}

		// If we covered the entire distance, we are done
		//  and can return.

		// Triggerfix related
		this->tpmTriggerFixOrigins.AddToTail(pm.m_vEndPos);

		if (pm.m_flFraction == 1)
		{
			break; // moved the entire distance
		}
		// Reduce amount of m_flFrameTime left by total time left * fraction
		//  that we covered.
		time_left -= pm.m_flFraction * time_left;

		// Did we run out of planes to clip against?
		if (numplanes >= MAX_CLIP_PLANES)
		{
			// this shouldn't really happen
			//  Stop our movement if so.
			VectorCopy(vec3_origin, velocity);
			break;
		}

		// Set up next clipping plane
		VectorCopy(pm.m_vHitNormal, planes[numplanes]);
		numplanes++;

		// modify original_velocity so it parallels all of the clip planes
		//

		// reflect player velocity
		// Only give this a try for first impact plane because you can get yourself stuck in an acute corner by jumping
		// in place
		//  and pressing forward and nobody was really using this bounce/reflection feature anyway...
		if (numplanes == 1 && this->player->GetPlayerPawn()->m_MoveType() == MOVETYPE_WALK
			&& this->player->GetPlayerPawn()->m_hGroundEntity().Get() == NULL)
		{
			ClipVelocity(original_velocity, planes[0], new_velocity);

			VectorCopy(new_velocity, velocity);
			VectorCopy(new_velocity, original_velocity);
		}
		else
		{
			for (i = 0; i < numplanes; i++)
			{
				ClipVelocity(original_velocity, planes[i], velocity);

				for (j = 0; j < numplanes; j++)
				{
					if (j != i)
					{
						// Are we now moving against this plane?
						if (velocity.Dot(planes[j]) < 0)
						{
							break; // not ok
						}
					}
				}

				if (j == numplanes) // Didn't have to clip, so we're ok
				{
					break;
				}
			}

			// Did we go all the way through plane set
			if (i == numplanes)
			{ // go along the crease
				if (numplanes != 2)
				{
					VectorCopy(vec3_origin, velocity);
					break;
				}
				CrossProduct(planes[0], planes[1], dir);
				dir.NormalizeInPlace();
				d = dir.Dot(velocity);
				VectorScale(dir, d, velocity);
			}

			//
			// if original velocity is against the original velocity, stop dead
			// to avoid tiny occilations in sloping corners
			//
			d = velocity.Dot(primal_velocity);
			if (d <= 0)
			{
				// Con_DPrintf("Back\n");
				VectorCopy(vec3_origin, velocity);
				break;
			}
		}
	}

	if (allFraction == 0)
	{
		VectorCopy(vec3_origin, velocity);
	}
}

void KZCustomVanillaModeService::OnTryPlayerMovePost(Vector *pFirstDest, trace_t *pFirstTrace)
{
	if (this->airMoving)
	{
		if (this->tpmTriggerFixOrigins.Count() > 1)
		{
			bbox_t bounds;
			this->player->GetBBoxBounds(&bounds);
			for (int i = 0; i < this->tpmTriggerFixOrigins.Count() - 1; i++)
			{
				this->player->TouchTriggersAlongPath(this->tpmTriggerFixOrigins[i], this->tpmTriggerFixOrigins[i + 1], bounds);
			}
		}
		this->player->UpdateTriggerTouchList();
	}
}

void KZCustomVanillaModeService::OnAirMove()
{
	this->airMoving = true;
}

void KZCustomVanillaModeService::OnAirMovePost()
{
	this->airMoving = false;
}

// Only touch timer triggers on full ticks.
bool KZCustomVanillaModeService::OnTriggerStartTouch(CBaseTrigger *trigger)
{
	if (!trigger->IsEndZone() && !trigger->IsStartZone())
	{
		return true;
	}
	f32 time = g_pKZUtils->GetGlobals()->curtime * ENGINE_FIXED_TICK_RATE;
	if (fabs(roundf(time) - time) < 0.001f)
	{
		return true;
	}

	return false;
}

bool KZCustomVanillaModeService::OnTriggerTouch(CBaseTrigger *trigger)
{
	if (!trigger->IsEndZone() && !trigger->IsStartZone())
	{
		return true;
	}
	f32 time = g_pKZUtils->GetGlobals()->curtime * ENGINE_FIXED_TICK_RATE;
	if (fabs(roundf(time) - time) < 0.001f)
	{
		return true;
	}
	return false;
}

bool KZCustomVanillaModeService::OnTriggerEndTouch(CBaseTrigger *trigger)
{
	if (!trigger->IsStartZone())
	{
		return true;
	}
	f32 time = g_pKZUtils->GetGlobals()->curtime * ENGINE_FIXED_TICK_RATE;
	if (fabs(roundf(time) - time) < 0.001f)
	{
		return true;
	}
	return false;
}

void KZCustomVanillaModeService::OnProcessMovement()
{
	this->player->GetPlayerPawn()->m_flVelocityModifier(1.0f);
}

void KZCustomVanillaModeService::OnProcessMovementPost()
{
	this->player->UpdateTriggerTouchList();
	this->player->GetPlayerPawn()->m_flVelocityModifier(this->originalMaxSpeed >= 0 ? 250.0f / this->originalMaxSpeed : 1.0f);
}

// Only happens when triggerfix happens.
void KZCustomVanillaModeService::OnTeleport(const Vector *newPosition, const QAngle *newAngles, const Vector *newVelocity)
{
	if (!this->player->processingMovement)
	{
		return;
	}
	if (newPosition)
	{
		this->player->currentMoveData->m_vecAbsOrigin = *newPosition;
	}
	if (newVelocity)
	{
		this->player->currentMoveData->m_vecVelocity = *newVelocity;
	}
}

void KZCustomVanillaModeService::OnStartTouchGround()
{
	bbox_t bounds;
	this->player->GetBBoxBounds(&bounds);
	Vector ground = this->player->landingOrigin;
	ground.z = this->player->GetGroundPosition() - 0.03125f;
	this->player->TouchTriggersAlongPath(this->player->landingOrigin, ground, bounds);
}

void KZCustomVanillaModeService::Cleanup()
{
	if (this->player->GetPlayerPawn())
	{
		this->player->GetPlayerPawn()->m_flVelocityModifier(1.0f);
	}
}

META_RES KZCustomVanillaModeService::GetPlayerMaxSpeed(f32 &maxSpeed)
{
	this->originalMaxSpeed = maxSpeed;
	maxSpeed = 250.0f;
	return MRES_SUPERCEDE;
}

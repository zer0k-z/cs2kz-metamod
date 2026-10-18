#include "kz_bhoptest.h"
#include "cs2kz.h"
#include "kz/mode/kz_mode.h"
#include "kz/style/kz_style.h"
#include "sdk/usercmd.h"
#include "sdk/services.h"
#include "sdk/entity/ccsplayercontroller.h"
#include "utils/simplecmds.h"
#include "sdk/tracefilter.h"
#include "sdk/navphysicsinterface.h"

#include <cmath>
#include <ctime>
#include <filesystem>

#include "tier0/memdbgon.h"

CConVar<bool> kz_bhoptest_enable("kz_bhoptest_enable", FCVAR_NONE,
								 "Allow kz_bhoptest, a debug tool that spawns bots, teleports players and overwrites their inputs.", false);

#define TICK_MS              (ENGINE_FIXED_TICK_INTERVAL * 1000.0)
#define JUMP_PRESS_TICK_NONE -1024
#define MAX_TEST_BOTS        16
#define MAX_CMD_EVENTS       16
#define PROGRESS_INTERVAL    100

static_global CHandle<CCSPlayerController> g_testBots[MAX_TEST_BOTS];

static_function f64 SnapToSubtickGrid(f64 ticks)
{
	// The server snaps every subtick `when` to 1/64 of a tick, so only inject times already on that grid.
	return std::round(ticks * 64.0) / 64.0;
}

static_function f32 Length2D(const Vector &v)
{
	return sqrtf(v.x * v.x + v.y * v.y);
}

// Messages go to the player who ran the command, or to the server console for headless runs.
static_function void Reply(KZPlayer *player, const char *format, ...)
{
	char buffer[512];
	va_list args;
	va_start(args, format);
	V_vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	if (player && player->IsInGame())
	{
		player->PrintChat(true, false, "%s", buffer);
	}
	else
	{
		META_CONPRINTF("%s\n", buffer);
	}
}

KZBhopTestService::~KZBhopTestService()
{
	this->Stop(nullptr);
}

f64 KZBhopTestService::Rel(i32 tick, f32 frac) const
{
	if (tick <= JUMP_PRESS_TICK_NONE)
	{
		return NAN;
	}
	return (f64)(tick - this->t0Tick) + frac;
}

f32 KZBhopTestService::GetGravity()
{
	return this->player->GetCvarValueFromModeStyles(MODECVAR_SV_GRAVITY)->m_fl32Value;
}

f32 KZBhopTestService::GetOffsetMs() const
{
	if (this->config.mode != Mode::Offset)
	{
		return NAN;
	}
	return this->config.fromMs + (this->trialIndex / this->config.repeat) * this->config.stepMs;
}

f32 KZBhopTestService::GetGapMs() const
{
	if (this->config.mode != Mode::Scroll)
	{
		return NAN;
	}
	// Batches run every gap for the first fall speed, then every gap for the next one, and so on.
	return this->config.gapsMs[(this->trialIndex / this->config.trials) % this->config.numGaps];
}

f32 KZBhopTestService::GetTargetFallSpeed() const
{
	if (this->config.mode != Mode::Scroll || this->config.numFallSpeeds == 0)
	{
		return NAN;
	}
	return this->config.fallSpeeds[(this->trialIndex / this->config.trials) / this->config.numGaps];
}

f64 KZBhopTestService::GetBaseHeight()
{
	f32 fallSpeed = this->GetTargetFallSpeed();
	if (std::isnan(fallSpeed))
	{
		return this->config.height;
	}
	return (f64)fallSpeed * fallSpeed / (2.0 * this->GetGravity());
}

void KZBhopTestService::Print(const char *format, ...)
{
	char buffer[512];
	va_list args;
	va_start(args, format);
	V_vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	if (this->owner && this->owner->IsInGame())
	{
		this->owner->PrintConsole(false, false, "%s", buffer);
	}
	else
	{
		META_CONPRINTF("%s", buffer);
	}
}

bool KZBhopTestService::Start(const Config &config, KZPlayer *owner)
{
	this->Stop(nullptr);

	if (this->player->GetCvarValueFromModeStyles(MODECVAR_SV_LEGACY_JUMP)->m_bValue)
	{
		Reply(owner, "kz_bhoptest: %s uses sv_legacy_jump 1, switch to a mode with the modern jump.", this->player->GetName());
		return false;
	}

	this->config = config;
	this->owner = owner;
	this->rng.seed((u64)time(nullptr) ^ ((u64)this->player->GetPlayerSlot().Get() << 32));
	if (config.mode == Mode::Offset)
	{
		i32 numOffsets = (i32)std::floor((config.toMs - config.fromMs) / config.stepMs + 1e-4) + 1;
		this->numTrials = numOffsets * config.repeat;
	}
	else
	{
		this->numTrials = config.numGaps * MAX(1, config.numFallSpeeds) * config.trials;
	}

	std::filesystem::path logDir = std::filesystem::path(g_SMAPI->GetBaseDir()) / "addons" / "cs2kz" / "logs";
	std::error_code ec;
	std::filesystem::create_directories(logDir, ec);

	char name[128];
	V_snprintf(name, sizeof(name), "bhoptest_%lld_%d", (long long)time(nullptr), this->player->GetPlayerSlot().Get());
	std::filesystem::path summary = logDir / (std::string(name) + ".csv");
	V_strncpy(this->summaryPath, summary.string().c_str(), sizeof(this->summaryPath));

	this->summaryFile = fopen(summary.string().c_str(), "w");
	if (!this->summaryFile)
	{
		Reply(owner, "kz_bhoptest: could not open %s", this->summaryPath);
		return false;
	}
	fprintf(this->summaryFile, "trial,mode,height,gravity,hspeed,offset_ms,gap_ms,jitter_ms,extra,extra_offset_ms,ref_t,band_t,contact_t,"
							   "num_presses,first_press_t,last_press_t,extra_t,grounded,grounded_t,jumped,jump_t,landed_changed,landed_t,"
							   "usable_t,actual_t,walkmove_before_jump,speed_landed,speed_pre,speed_post,restored,capped,hover_z,outcome,"
							   "target_fall_speed,predicted_fall_speed,landed_fall_speed,launch_fall_speed\n");

	if (config.trace)
	{
		std::filesystem::path trace = logDir / (std::string(name) + "_trace.csv");
		this->traceFile = fopen(trace.string().c_str(), "w");
		if (this->traceFile)
		{
			fprintf(this->traceFile, "trial,t_start,t_end,grounded_pre,grounded_post,z_pre,z_post,vz_pre,vz_post,speed_post,"
									 "landed_t,usable_t,actual_t,checked_jump,called_onjump,jumped,walkmove,touched_ground,jump_button_state\n");
		}
		std::filesystem::path cmds = logDir / (std::string(name) + "_cmds.csv");
		this->cmdFile = fopen(cmds.string().c_str(), "w");
		if (this->cmdFile)
		{
			fprintf(this->cmdFile, "trial,phase,cmd,jump_state,events\n");
		}
	}

	this->trialIndex = 0;
	this->heldButtons = 0;
	this->BeginSettle();
	return true;
}

void KZBhopTestService::Stop(const char *reason)
{
	bool wasActive = this->phase != Phase::Idle;
	this->phase = Phase::Idle;
	this->call = {};
	if (this->summaryFile)
	{
		fclose(this->summaryFile);
		this->summaryFile = nullptr;
	}
	if (this->traceFile)
	{
		fclose(this->traceFile);
		this->traceFile = nullptr;
	}
	if (this->cmdFile)
	{
		fclose(this->cmdFile);
		this->cmdFile = nullptr;
	}
	if (wasActive && reason)
	{
		Reply(this->owner, "kz_bhoptest: %s stopped (%s) after %d/%d trials.", this->player->GetName(), reason, this->trialIndex, this->numTrials);
		this->Print("kz_bhoptest: output %s\n", this->summaryPath);
	}
}

void KZBhopTestService::BeginSettle()
{
	this->phase = Phase::Settle;
	this->settleCmd = 0;
	this->haveT0 = false;
	this->awaitT0 = false;
	this->call = {};
}

void KZBhopTestService::AddEvent(f64 time, u64 button, bool pressed)
{
	i32 cmd = (i32)std::floor(time);
	this->schedule.AddToTail({cmd, (f32)(time - cmd), button, pressed});
}

void KZBhopTestService::BuildSchedule(const f64 *pressTimes, i32 count)
{
	// Like a real scroll step: press and release in the same command at the same `when`, so the release adds no split.
	// A release in a command of its own (e.g. at `when` 0 of the next one) stops the server from seeing the next press.
	for (i32 i = 0; i < count; i++)
	{
		this->AddEvent(pressTimes[i], IN_JUMP, true);
		this->AddEvent(pressTimes[i], IN_JUMP, false);
	}
}

bool KZBhopTestService::BeginTrial()
{
	f64 g = this->GetGravity();
	std::uniform_real_distribution<f64> uniform(0.0, 1.0);

	this->schedule.RemoveAll();
	this->trialHeight = this->config.height;
	this->launchVz = 0.0;
	f64 pressTimes[256];
	i32 count = 0;

	if (this->config.mode == Mode::Offset)
	{
		this->contactTime = std::sqrt(2.0 * this->trialHeight / g) * ENGINE_FIXED_TICK_RATE;
		// For crouchbugs, offsets are relative to the moment the (ducked) feet are 10 units above the floor,
		// the middle of the 9-11 unit range where unducking lands the player.
		this->refTime =
			this->config.extra == Extra::Unduck ? std::sqrt(2.0 * (this->trialHeight - 10.0) / g) * ENGINE_FIXED_TICK_RATE : this->contactTime;
		pressTimes[count++] = SnapToSubtickGrid(this->refTime + this->GetOffsetMs() / TICK_MS);
		this->extraTime = SnapToSubtickGrid(this->refTime + this->config.extraOffsetMs / TICK_MS);
		if (pressTimes[0] < 0.0 || (this->config.extra != Extra::None && this->extraTime < 0.0))
		{
			this->Print("kz_bhoptest: trial %d skipped, input would happen before the launch.\n", this->trialIndex);
			return false;
		}
	}
	else
	{
		// Randomize where in the tick the landing happens without changing the fall speed at contact: start the fall
		// with a random downward speed of up to one tick of gravity and shorten the drop to match. The contact time then
		// moves by a uniform fraction of a tick while the last part of the fall (and the 2 unit band) stays identical.
		f64 contactSpeed = std::sqrt(2.0 * g * this->GetBaseHeight());
		f64 shiftTicks = uniform(this->rng);
		f64 startSpeed = g * shiftTicks / ENGINE_FIXED_TICK_RATE;
		this->launchVz = -startSpeed;
		this->trialHeight = (contactSpeed * contactSpeed - startSpeed * startSpeed) / (2.0 * g);
		this->contactTime = (contactSpeed - startSpeed) / g * ENGINE_FIXED_TICK_RATE;
		this->refTime = this->contactTime;

		f64 gapTicks = this->GetGapMs() / TICK_MS;
		f64 jitterTicks = this->config.jitterMs / TICK_MS;
		f64 t = this->contactTime - this->config.preTicks + uniform(this->rng) * gapTicks;
		while (t < this->contactTime + this->config.postTicks && count < (i32)KZ_ARRAYSIZE(pressTimes))
		{
			f64 snapped = SnapToSubtickGrid(t);
			if (snapped >= 0.0 && (count == 0 || snapped > pressTimes[count - 1]))
			{
				pressTimes[count++] = snapped;
			}
			t += gapTicks + (jitterTicks > 0.0 ? (uniform(this->rng) * 2.0 - 1.0) * jitterTicks : 0.0);
		}
		if (count == 0)
		{
			return false;
		}
	}

	// Time to fall to 2 units above the floor, starting at launchVz (negative is downward).
	f64 v0 = -this->launchVz;
	this->bandTime = (std::sqrt(v0 * v0 + 2.0 * g * (this->trialHeight - 2.0)) - v0) / g * ENGINE_FIXED_TICK_RATE;
	this->numPresses = count;
	this->firstPressTime = pressTimes[0];
	this->lastPressTime = pressTimes[count - 1];
	this->BuildSchedule(pressTimes, count);

	if (this->config.mode == Mode::Offset)
	{
		if (this->config.extra == Extra::Strafe)
		{
			this->AddEvent(this->extraTime, IN_MOVELEFT, true);
			this->AddEvent(std::floor(this->extraTime) + 1, IN_MOVELEFT, false);
		}
		else if (this->config.extra == Extra::Unduck)
		{
			this->AddEvent(this->extraTime, IN_DUCK, false);
		}
	}

	// Stable insertion sort by command, then by `when`, keeping same-time releases before presses.
	for (i32 i = 1; i < this->schedule.Count(); i++)
	{
		ScheduledEvent e = this->schedule[i];
		i32 j = i - 1;
		while (j >= 0 && (this->schedule[j].cmd > e.cmd || (this->schedule[j].cmd == e.cmd && this->schedule[j].when > e.when)))
		{
			this->schedule[j + 1] = this->schedule[j];
			j--;
		}
		this->schedule[j + 1] = e;
	}

	this->lastInputCmd = this->schedule.Count() ? this->schedule.Tail().cmd : 0;
	this->timeoutCmd = MAX(this->lastInputCmd, (i32)std::ceil(this->contactTime)) + 40;

	Vector origin;
	this->player->GetOrigin(&origin);
	this->floorZ = origin.z;

	this->phase = Phase::Trial;
	this->trialCmd = 0;
	this->result = {};
	return true;
}

void KZBhopTestService::FinishTrial()
{
	CCSPlayerModernJump &modernJump = this->player->GetMoveServices()->m_ModernJump();
	if (!this->result.jumped)
	{
		this->result.landedChanged =
			modernJump.m_nLastLandedTick() != this->landedTickAtLaunch || modernJump.m_flLastLandedFrac() != this->landedFracAtLaunch;
		this->result.landedTime = this->Rel(modernJump.m_nLastLandedTick(), modernJump.m_flLastLandedFrac());
	}

	bool restored = false;
	bool capped = false;
	const char *outcome = this->Classify(&restored, &capped);

	static const char *modeNames[] = {"offset", "scroll"};
	static const char *extraNames[] = {"none", "strafe", "unduck"};
	const TrialResult &r = this->result;
	if (this->summaryFile)
	{
		fprintf(this->summaryFile,
				"%d,%s,%.4f,%.1f,%.2f,%.4f,%.4f,%.4f,%s,%.4f,%.6f,%.6f,%.6f,%d,%.6f,%.6f,%.6f,%d,%.6f,%d,%.6f,%d,%.6f,%.6f,%.6f,%d,%.3f,%.3f,%.3f,%d,"
				"%d,%.4f,%s,%.1f,%.3f,%.3f,%.3f\n",
				this->trialIndex, modeNames[(i32)this->config.mode], this->trialHeight, this->GetGravity(), this->config.hspeed, this->GetOffsetMs(),
				this->GetGapMs(), this->config.mode == Mode::Scroll ? this->config.jitterMs : NAN, extraNames[(i32)this->config.extra],
				this->config.extraOffsetMs, this->refTime, this->bandTime, this->contactTime, this->numPresses, this->firstPressTime,
				this->lastPressTime, this->config.extra == Extra::None ? NAN : this->extraTime, r.grounded, r.grounded ? r.groundedTime : NAN,
				r.jumped, r.jumped ? r.jumpTime : NAN, r.landedChanged, r.landedTime, r.usableTime, r.actualTime, r.walkMoved, r.speedLanded,
				r.speedPre, r.speedPost, restored, capped, r.hoverZ, outcome, this->GetTargetFallSpeed(),
				std::sqrt(this->launchVz * this->launchVz + 2.0 * this->GetGravity() * this->trialHeight), -modernJump.m_flLastLandedVelocityZ(),
				-this->launchVz);
		fflush(this->summaryFile);
	}
	if (this->config.mode == Mode::Offset)
	{
		this->Print("kz_bhoptest: trial %d offset %+.3fms -> %s\n", this->trialIndex, this->GetOffsetMs(), outcome);
	}
	else if ((this->trialIndex + 1) % PROGRESS_INTERVAL == 0)
	{
		this->Print("kz_bhoptest: %s %d/%d trials\n", this->player->GetName(), this->trialIndex + 1, this->numTrials);
	}

	this->trialIndex++;
	if (this->trialIndex >= this->numTrials)
	{
		this->Stop("done");
		return;
	}
	this->BeginSettle();
}

const char *KZBhopTestService::Classify(bool *restored, bool *capped) const
{
	const TrialResult &r = this->result;
	if (!r.jumped)
	{
		return r.grounded ? "none" : "never_grounded";
	}

	*restored = r.speedPost > r.speedPre + 0.01f;
	*capped = r.speedPost < r.speedPre - 0.01f;

	if (!r.walkMoved)
	{
		if (!r.landedChanged)
		{
			return "no_landing_immediate";
		}
		// The jump fired in the segment that starts at the press itself.
		bool pressStartedSegment = std::fabs(r.actualTime - r.jumpTime) < 1e-3;
		return pressStartedSegment ? "perfect_frame" : "perfect_buffered";
	}
	if (!r.landedChanged)
	{
		return "no_landing_after_walkmove";
	}
	if (*restored)
	{
		return "restored";
	}
	if (std::fabs(r.jumpTime - (r.landedTime + 1.0)) < 1e-3)
	{
		return "tick_after_landing";
	}
	return "late";
}

void KZBhopTestService::WriteCommand(PlayerCommand *cmd, InputEvent *events, i32 numEvents)
{
	u64 before = this->heldButtons;
	u64 after = before;
	u64 changed = 0;
	for (i32 i = 0; i < numEvents; i++)
	{
		changed |= events[i].button;
		if (events[i].pressed)
		{
			after |= events[i].button;
		}
		else
		{
			after &= ~events[i].button;
		}
	}

	// EInButtonState = bit0 + 2 * bit1 + 4 * bit2, with bit0 = held at the end of the command.
	u64 states[3] = {after, 0, 0};
	for (u64 bit = 1; bit && bit <= changed; bit <<= 1)
	{
		if (!(changed & bit))
		{
			continue;
		}
		bool wasDown = before & bit;
		bool isDown = after & bit;
		if (wasDown != isDown)
		{
			states[1] |= bit; // IN_BUTTON_UP_DOWN or IN_BUTTON_DOWN_UP
		}
		else
		{
			states[2] |= bit; // IN_BUTTON_UP_DOWN_UP or IN_BUTTON_DOWN_UP_DOWN
		}
	}

	auto base = cmd->mutable_base();
	base->set_forwardmove(0.0f);
	// Only report a held strafe key for whole commands, the subtick step carries the edge itself.
	base->set_leftmove((before & after & IN_MOVELEFT) ? 1.0f : 0.0f);
	base->set_upmove(0.0f);
	base->mutable_viewangles()->set_x(this->config.startAngles.x);
	base->mutable_viewangles()->set_y(this->config.startAngles.y);
	base->mutable_viewangles()->set_z(0.0f);

	base->mutable_buttons_pb()->set_buttonstate1(states[0]);
	base->mutable_buttons_pb()->set_buttonstate2(states[1]);
	base->mutable_buttons_pb()->set_buttonstate3(states[2]);
	for (i32 i = 0; i < 3; i++)
	{
		cmd->buttonstates.m_pButtonStates[i] = states[i];
	}

	base->clear_subtick_moves();
	for (i32 i = 0; i < numEvents; i++)
	{
		auto move = base->add_subtick_moves();
		move->set_when(events[i].when);
		move->set_button(events[i].button);
		move->set_pressed(events[i].pressed);
	}

	this->heldButtons = after;

	if (this->cmdFile)
	{
		// jump_state is the EInButtonState of IN_JUMP we sent (bit0 + 2 * bit1 + 4 * bit2).
		i32 jumpState = !!(states[0] & IN_JUMP) + 2 * !!(states[1] & IN_JUMP) + 4 * !!(states[2] & IN_JUMP);
		fprintf(this->cmdFile, "%d,%s,%d,%d,", this->trialIndex, this->phase == Phase::Trial ? "trial" : "settle",
				this->phase == Phase::Trial ? this->trialCmd : this->settleCmd, jumpState);
		for (i32 i = 0; i < numEvents; i++)
		{
			fprintf(this->cmdFile, "%s%.6f:%llx:%d", i ? "|" : "", events[i].when, (unsigned long long)events[i].button, events[i].pressed);
		}
		fprintf(this->cmdFile, "\n");
	}
}

void KZBhopTestService::OnPlayerRunCommandPre(PlayerCommand *cmd)
{
	if (this->phase == Phase::Idle)
	{
		return;
	}

	CCSPlayerPawn *pawn = this->player->GetPlayerPawn();
	if (!pawn || !this->player->IsAlive())
	{
		this->Stop("target is not alive");
		return;
	}

	InputEvent events[MAX_CMD_EVENTS];
	i32 numEvents = 0;

	if (this->phase == Phase::Trial)
	{
		if (this->trialCmd > 0 && ((this->result.jumped && this->trialCmd > this->lastInputCmd) || this->trialCmd > this->timeoutCmd))
		{
			this->FinishTrial();
			// This command now belongs to the settle phase (or the test just ended).
			this->OnPlayerRunCommandPre(cmd);
			return;
		}

		if (this->trialCmd == 0)
		{
			Vector origin = this->config.startPos;
			origin.z = this->floorZ + this->trialHeight;
			Vector forward;
			AngleVectors(QAngle(0.0f, this->config.startAngles.y, 0.0f), &forward);
			Vector velocity = forward * this->config.hspeed;
			velocity.z = (f32)this->launchVz;
			this->player->Teleport(&origin, &this->config.startAngles, &velocity);
			pawn->m_fFlags(pawn->m_fFlags() & ~FL_ONGROUND);
			pawn->m_hGroundEntity(nullptr);

			CCSPlayerModernJump &modernJump = this->player->GetMoveServices()->m_ModernJump();
			this->landedTickAtLaunch = modernJump.m_nLastLandedTick();
			this->landedFracAtLaunch = modernJump.m_flLastLandedFrac();
			this->awaitT0 = true;
		}

		FOR_EACH_VEC(this->schedule, i)
		{
			const ScheduledEvent &e = this->schedule[i];
			if (e.cmd == this->trialCmd && numEvents < MAX_CMD_EVENTS)
			{
				events[numEvents++] = {e.when, e.button, e.pressed};
			}
		}

		this->WriteCommand(cmd, events, numEvents);
		this->trialCmd++;
		return;
	}

	// Settle: stand still at the start position so the previous trial's jump state wears off.
	if (this->settleCmd == 0)
	{
		Vector zero(0.0f, 0.0f, 0.0f);
		this->player->Teleport(&this->config.startPos, &this->config.startAngles, &zero);
		u64 keep = this->config.extra == Extra::Unduck ? IN_DUCK : 0;
		for (u64 bit = 1; bit && bit <= this->heldButtons; bit <<= 1)
		{
			if ((this->heldButtons & bit) && !(keep & bit) && numEvents < MAX_CMD_EVENTS - 1)
			{
				events[numEvents++] = {0.0f, bit, false};
			}
		}
		if (keep && !(this->heldButtons & keep))
		{
			events[numEvents++] = {0.0f, keep, true};
		}
	}
	this->WriteCommand(cmd, events, numEvents);
	this->settleCmd++;

	if (this->settleCmd >= this->config.settleTicks)
	{
		if (pawn->m_fFlags() & FL_ONGROUND)
		{
			while (!this->BeginTrial())
			{
				this->trialIndex++;
				if (this->trialIndex >= this->numTrials)
				{
					this->Stop("done");
					return;
				}
			}
		}
		else if (this->settleCmd > this->config.settleTicks + 64)
		{
			this->Stop("target never settled on the ground");
		}
	}
}

void KZBhopTestService::OnProcessMovement()
{
	this->call.active = false;
	if (this->phase != Phase::Trial || !this->player->currentMoveData)
	{
		return;
	}
	CMoveData *mv = this->player->currentMoveData;
	if (this->awaitT0)
	{
		this->t0Tick = mv->m_nTickCount;
		this->haveT0 = true;
		this->awaitT0 = false;
	}
	if (!this->haveT0)
	{
		return;
	}

	this->call = {};
	this->call.active = true;
	this->call.tick = mv->m_nTickCount;
	this->call.startFrac = mv->m_flSubtickStartFraction;
	this->call.endFrac = mv->m_flSubtickEndFraction;
	this->call.groundedPre = !!(this->player->GetPlayerPawn()->m_fFlags() & FL_ONGROUND);
	this->call.zPre = mv->m_vecAbsOrigin.z - this->floorZ;
	this->call.velPre = mv->m_vecVelocity;
	CInButtonState &buttons = this->player->GetMoveServices()->m_nButtons();
	this->call.jumpButtonState =
		!!(buttons.m_pButtonStates[0] & IN_JUMP) + 2 * !!(buttons.m_pButtonStates[1] & IN_JUMP) + 4 * !!(buttons.m_pButtonStates[2] & IN_JUMP);
}

void KZBhopTestService::OnProcessMovementPost()
{
	if (!this->call.active)
	{
		return;
	}
	this->call.active = false;
	if (!this->traceFile)
	{
		return;
	}

	CMoveData *mv = this->player->currentMoveData;
	CCSPlayerModernJump &modernJump = this->player->GetMoveServices()->m_ModernJump();
	bool groundedPost = !!(this->player->GetPlayerPawn()->m_fFlags() & FL_ONGROUND);
	fprintf(this->traceFile, "%d,%.6f,%.6f,%d,%d,%.4f,%.4f,%.3f,%.3f,%.3f,%.6f,%.6f,%.6f,%d,%d,%d,%d,%d,%d\n", this->trialIndex,
			this->Rel(this->call.tick, this->call.startFrac), this->Rel(this->call.tick, this->call.endFrac), this->call.groundedPre, groundedPost,
			this->call.zPre, mv->m_vecAbsOrigin.z - this->floorZ, this->call.velPre.z, mv->m_vecVelocity.z, Length2D(mv->m_vecVelocity),
			this->Rel(modernJump.m_nLastLandedTick(), modernJump.m_flLastLandedFrac()),
			this->Rel(modernJump.m_nLastUsableJumpPressTick(), modernJump.m_flLastUsableJumpPressFrac()),
			this->Rel(modernJump.m_nLastActualJumpPressTick(), modernJump.m_flLastActualJumpPressFrac()), this->call.checkedJump,
			this->call.calledOnJump, this->call.jumped, this->call.walkMoved, this->call.touchedGround, this->call.jumpButtonState);
}

void KZBhopTestService::OnCheckJumpButtonModern()
{
	if (this->call.active)
	{
		this->call.checkedJump = true;
	}
}

void KZBhopTestService::OnStartTouchGround()
{
	if (!this->call.active)
	{
		return;
	}
	this->call.touchedGround = true;
	if (!this->result.grounded)
	{
		this->result.grounded = true;
		// Grounding from the duck code happens before the jump check, grounding from PostMove after it.
		this->result.groundedTime = this->Rel(this->call.tick, this->call.checkedJump ? this->call.endFrac : this->call.startFrac);
	}
}

void KZBhopTestService::OnWalkMove()
{
	if (!this->call.active)
	{
		return;
	}
	this->call.walkMoved = true;
	if (this->result.grounded && !this->result.jumped)
	{
		this->result.walkMoved = true;
	}
}

void KZBhopTestService::OnJumpModern()
{
	if (!this->call.active)
	{
		return;
	}
	this->call.calledOnJump = true;
	CMoveData *mv = this->player->currentMoveData;
	CCSPlayerModernJump &modernJump = this->player->GetMoveServices()->m_ModernJump();
	this->preJumpOutWishVel = mv->m_outWishVel;
	this->preJumpSpeed = Length2D(mv->m_vecVelocity);
	this->preJumpZ = mv->m_vecAbsOrigin.z - this->floorZ;
	this->preJumpUsable = this->Rel(modernJump.m_nLastUsableJumpPressTick(), modernJump.m_flLastUsableJumpPressFrac());
	this->preJumpActual = this->Rel(modernJump.m_nLastActualJumpPressTick(), modernJump.m_flLastActualJumpPressFrac());
}

void KZBhopTestService::OnJumpModernPost()
{
	if (!this->call.active)
	{
		return;
	}
	CMoveData *mv = this->player->currentMoveData;
	// OnJump returns early without changing anything when it decides not to jump.
	if (this->preJumpOutWishVel == mv->m_outWishVel)
	{
		return;
	}
	this->call.jumped = true;
	if (this->result.jumped)
	{
		return;
	}

	CCSPlayerModernJump &modernJump = this->player->GetMoveServices()->m_ModernJump();
	TrialResult &r = this->result;
	r.jumped = true;
	r.jumpTime = this->Rel(this->call.tick, this->call.startFrac);
	r.speedPre = this->preJumpSpeed;
	r.speedPost = Length2D(mv->m_vecVelocity);
	r.hoverZ = this->preJumpZ;
	r.usableTime = this->preJumpUsable;
	r.actualTime = this->preJumpActual;
	r.landedChanged = modernJump.m_nLastLandedTick() != this->landedTickAtLaunch || modernJump.m_flLastLandedFrac() != this->landedFracAtLaunch;
	r.landedTime = this->Rel(modernJump.m_nLastLandedTick(), modernJump.m_flLastLandedFrac());
	r.speedLanded = sqrtf(modernJump.m_flLastLandedVelocityX() * modernJump.m_flLastLandedVelocityX()
						  + modernJump.m_flLastLandedVelocityY() * modernJump.m_flLastLandedVelocityY());
}

// ============================================================
// Test bots
// ============================================================

static_function KZPlayer *GetTestBot(i32 index)
{
	CCSPlayerController *controller = g_testBots[index].Get();
	if (!controller)
	{
		return nullptr;
	}
	KZPlayer *bot = g_pKZPlayerManager->ToPlayer(controller);
	return bot && bot->IsAlive() ? bot : nullptr;
}

// Spawned directly through CreateBot like the replay bot, since bot_add doesn't work on every map.
static_function i32 SpawnTestBots(i32 count)
{
	i32 total = 0;
	for (i32 i = 0; i < MAX_TEST_BOTS && total < count; i++)
	{
		if (g_testBots[i].Get())
		{
			total++;
			continue;
		}
		BotProfile *profile = BotProfile::PersistentProfile(CS_TEAM_T);
		CCSPlayerController *controller = profile ? g_pKZUtils->CreateBot(profile, CS_TEAM_T) : nullptr;
		if (!controller)
		{
			break;
		}
		g_testBots[i] = controller;
		KZPlayer *bot = g_pKZPlayerManager->ToPlayer(controller);
		KZ::misc::JoinTeam(bot, CS_TEAM_CT, false);
		// CS2 kicks bots that don't have spectator pending team when another player joins.
		controller->m_iPendingTeamNum(1);
		total++;
	}
	return total;
}

static_function void KickTestBots()
{
	for (i32 i = 0; i < MAX_TEST_BOTS; i++)
	{
		CCSPlayerController *controller = g_testBots[i].Get();
		if (!controller)
		{
			continue;
		}
		KZPlayer *bot = g_pKZPlayerManager->ToPlayer(controller);
		if (bot && bot->bhopTestService)
		{
			bot->bhopTestService->Stop(nullptr);
		}
		interfaces::pEngine->KickClient(controller->GetPlayerSlot(), "bye bot", NETWORK_DISCONNECT_KICKED);
		g_testBots[i].Term();
	}
}

// ============================================================
// Command
// ============================================================

CConVar<Vector> kz_bhoptest_origin("kz_bhoptest_origin", FCVAR_NONE,
								   "Point above the floor where kz_bhoptest runs; the floor is found by tracing down from it. "
								   "0 0 0 uses the position of the player running the command.",
								   Vector(0.0f, 0.0f, 0.0f));
CConVar<f32> kz_bhoptest_yaw("kz_bhoptest_yaw", FCVAR_NONE, "Launch direction (yaw) used together with kz_bhoptest_origin.", 0.0f);

#define FLOOR_TRACE_DISTANCE 8192.0f

// Sweeps a standing player hull down from `start` and returns the origin it would rest at on the floor below.
static_function bool FindFloor(KZPlayer *target, const Vector &start, Vector &floor)
{
	CCSPlayerPawn *pawn = target->GetPlayerPawn();
	if (!pawn)
	{
		return false;
	}
	trace_t tr;
	Vector mins(-16.0f, -16.0f, 0.0f);
	Vector maxs(16.0f, 16.0f, 72.0f);
	Vector end = start;
	end.z -= FLOOR_TRACE_DISTANCE;
	CTraceFilterPlayerMovementCS filter(pawn);
	INavPhysicsInterface::TraceShape(Ray_t(mins, maxs), start, end, &filter, &tr);
	if (tr.m_bStartInSolid || !tr.DidHit())
	{
		return false;
	}
	floor = tr.m_vEndPos;
	return true;
}

static_function void PrintUsage(KZPlayer *player)
{
	static const char *lines[] = {
		"kz_bhoptest bots <n>         spawn test bots (max 16)",
		"kz_bhoptest kickbots         remove test bots",
		"kz_bhoptest stop             stop all running tests",
		"kz_bhoptest [key=value ...]  start a test on every test bot",
		"  (kz_bhoptest_server takes the same arguments from the server console)",
		"  target=bots|self     default: all test bots (self only works for players)",
		"  kzmode=<name>        mode the bots are switched to (default Vanilla)",
		"  height=<u>           drop height above the floor (default 42.25, ~260 u/s at 800 gravity)",
		"  fallspeed=<u/s>      alternative to height, fall speed at contact",
		"  hspeed=<u/s>         horizontal speed at launch (default 275, keep it under the 286 bhop cap)",
		"  settle=<ticks>       ticks standing still between trials (default 16)",
		"  trace=0|1            also write a per-subtick trace file (default 0)",
		" offset mode:",
		"  from=<ms> to=<ms> step=<ms>   jump press offsets relative to the reference time (default -10 6 0.25)",
		"  repeat=<n>           trials per offset (default 1)",
		"  extra=none|strafe|unduck      extra input (default none)",
		"  extraoffset=<ms>     extra input time relative to the reference time (default 0)",
		"  Reference time: predicted floor contact, or for extra=unduck the moment the ducked feet are 10u above the floor.",
		" scroll mode:",
		"  gaps=<ms>[,<ms>...]  time between presses, one batch of trials per value",
		"  fallspeeds=<u/s>[,...]  fall speeds at contact; every gap is run at every fall speed (default: use height)",
		"  jitter=<ms>          random +- change to each gap (default 0)",
		"  trials=<n>           landings per gap per bot (default 500)",
		"  Each landing happens at a random point in the tick, and the press train starts at a random phase,",
		"  3 ticks before contact until 1.5 ticks after it.",
		"Position: kz_bhoptest_origin <x y z> (a point above flat floor) and kz_bhoptest_yaw <yaw>.",
		"If kz_bhoptest_origin is 0 0 0, the position of the player running the command is used.",
		"All bots run on the same spot (players don't collide). Leave ~300 units of clear floor ahead.",
	};
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(lines); i++)
	{
		if (player && player->IsInGame())
		{
			player->PrintConsole(false, false, "%s", lines[i]);
		}
		else
		{
			META_CONPRINTF("%s\n", lines[i]);
		}
	}
}

// `caller` is null when the command comes from the server console.
static_function void RunBhopTestCommand(KZPlayer *caller, const CCommand *args)
{
	if (args->ArgC() >= 2 && !V_stricmp(args->Arg(1), "stop"))
	{
		for (i32 i = 0; i <= MAXPLAYERS; i++)
		{
			KZPlayer *other = g_pKZPlayerManager->ToPlayer(i);
			if (other && other->bhopTestService && other->bhopTestService->IsActive())
			{
				other->bhopTestService->Stop("stopped by command");
			}
		}
		return;
	}
	if (args->ArgC() >= 2 && !V_stricmp(args->Arg(1), "bots"))
	{
		i32 count = args->ArgC() >= 3 ? V_StringToInt32(args->Arg(2), 10) : 10;
		count = MAX(1, MIN(count, MAX_TEST_BOTS));
		Reply(caller, "kz_bhoptest: %d test bots.", SpawnTestBots(count));
		return;
	}
	if (args->ArgC() >= 2 && !V_stricmp(args->Arg(1), "kickbots"))
	{
		KickTestBots();
		Reply(caller, "kz_bhoptest: test bots removed.");
		return;
	}
	if (args->ArgC() >= 2 && (!V_stricmp(args->Arg(1), "help") || !V_stricmp(args->Arg(1), "?")))
	{
		PrintUsage(caller);
		return;
	}

	KZBhopTestService::Config config;
	bool useSelf = false;
	bool modeGiven = false;
	f32 fallSpeed = -1.0f;
	char modeName[64] = "Vanilla";
	for (i32 i = 1; i < args->ArgC(); i++)
	{
		const char *arg = args->Arg(i);
		const char *eq = V_strstr(arg, "=");
		if (!eq)
		{
			PrintUsage(caller);
			return;
		}
		char key[32];
		V_strncpy(key, arg, MIN((i32)sizeof(key), (i32)(eq - arg) + 1));
		const char *value = eq + 1;

		if (!V_stricmp(key, "target"))
		{
			useSelf = !V_stricmp(value, "self");
		}
		else if (!V_stricmp(key, "kzmode"))
		{
			V_strncpy(modeName, value, sizeof(modeName));
		}
		else if (!V_stricmp(key, "mode"))
		{
			modeGiven = true;
			config.mode = !V_stricmp(value, "scroll") ? KZBhopTestService::Mode::Scroll : KZBhopTestService::Mode::Offset;
		}
		else if (!V_stricmp(key, "height"))
		{
			config.height = V_StringToFloat32(value, config.height);
		}
		else if (!V_stricmp(key, "fallspeed"))
		{
			fallSpeed = V_StringToFloat32(value, -1.0f);
		}
		else if (!V_stricmp(key, "hspeed"))
		{
			config.hspeed = V_StringToFloat32(value, config.hspeed);
		}
		else if (!V_stricmp(key, "settle"))
		{
			config.settleTicks = V_StringToInt32(value, config.settleTicks);
		}
		else if (!V_stricmp(key, "trace"))
		{
			config.trace = V_StringToInt32(value, 0) != 0;
		}
		else if (!V_stricmp(key, "from"))
		{
			config.fromMs = V_StringToFloat32(value, config.fromMs);
		}
		else if (!V_stricmp(key, "to"))
		{
			config.toMs = V_StringToFloat32(value, config.toMs);
		}
		else if (!V_stricmp(key, "step"))
		{
			config.stepMs = V_StringToFloat32(value, config.stepMs);
		}
		else if (!V_stricmp(key, "repeat"))
		{
			config.repeat = V_StringToInt32(value, config.repeat);
		}
		else if (!V_stricmp(key, "extra"))
		{
			if (!V_stricmp(value, "strafe"))
			{
				config.extra = KZBhopTestService::Extra::Strafe;
			}
			else if (!V_stricmp(value, "unduck"))
			{
				config.extra = KZBhopTestService::Extra::Unduck;
			}
			else
			{
				config.extra = KZBhopTestService::Extra::None;
			}
		}
		else if (!V_stricmp(key, "extraoffset"))
		{
			config.extraOffsetMs = V_StringToFloat32(value, config.extraOffsetMs);
		}
		else if (!V_stricmp(key, "gaps") || !V_stricmp(key, "gap"))
		{
			CSplitString gaps(value, ",");
			config.numGaps = 0;
			FOR_EACH_VEC(gaps, j)
			{
				if (config.numGaps < KZBhopTestService::MAX_GAPS)
				{
					config.gapsMs[config.numGaps++] = V_StringToFloat32(gaps[j], 0.0f);
				}
			}
			if (!modeGiven)
			{
				config.mode = KZBhopTestService::Mode::Scroll;
			}
		}
		else if (!V_stricmp(key, "fallspeeds"))
		{
			CSplitString speeds(value, ",");
			config.numFallSpeeds = 0;
			FOR_EACH_VEC(speeds, j)
			{
				if (config.numFallSpeeds < KZBhopTestService::MAX_GAPS)
				{
					config.fallSpeeds[config.numFallSpeeds++] = V_StringToFloat32(speeds[j], 0.0f);
				}
			}
		}
		else if (!V_stricmp(key, "jitter"))
		{
			config.jitterMs = V_StringToFloat32(value, config.jitterMs);
		}
		else if (!V_stricmp(key, "trials"))
		{
			config.trials = V_StringToInt32(value, config.trials);
		}
		else
		{
			PrintUsage(caller);
			return;
		}
	}

	if (config.settleTicks < 1
		|| (config.mode == KZBhopTestService::Mode::Offset && (config.stepMs <= 0.0f || config.toMs < config.fromMs || config.repeat < 1)))
	{
		PrintUsage(caller);
		return;
	}
	if (config.mode == KZBhopTestService::Mode::Scroll)
	{
		bool validGaps = config.numGaps > 0;
		for (i32 i = 0; i < config.numGaps; i++)
		{
			validGaps &= config.gapsMs[i] > 0.5f;
		}
		// Below ~80 u/s the drop is only a few units, so the player would start inside the 2 unit ground range.
		bool validSpeeds = true;
		for (i32 i = 0; i < config.numFallSpeeds; i++)
		{
			validSpeeds &= config.fallSpeeds[i] >= 80.0f;
		}
		if (!validGaps || !validSpeeds || config.trials < 1 || config.extra != KZBhopTestService::Extra::None)
		{
			Reply(caller, "kz_bhoptest: scroll mode needs gaps=<ms>[,<ms>...] above 0.5ms, fallspeeds of at least 80, trials>=1 and no extra input.");
			return;
		}
	}
	else if (config.numFallSpeeds > 0)
	{
		Reply(caller, "kz_bhoptest: fallspeeds= only works in scroll mode, use fallspeed= for offset mode.");
		return;
	}

	KZPlayer *targets[MAX_TEST_BOTS];
	i32 numTargets = 0;
	if (useSelf)
	{
		if (!caller)
		{
			Reply(caller, "kz_bhoptest: target=self only works when a player runs the command.");
			return;
		}
		targets[numTargets++] = caller;
	}
	else
	{
		for (i32 i = 0; i < MAX_TEST_BOTS; i++)
		{
			KZPlayer *bot = GetTestBot(i);
			if (bot)
			{
				targets[numTargets++] = bot;
			}
		}
	}
	if (numTargets == 0)
	{
		Reply(caller, "kz_bhoptest: no test bots, spawn them with kz_bhoptest bots <n>%s.", caller ? " or use target=self" : "");
		return;
	}
	for (i32 i = 0; i < numTargets; i++)
	{
		if (targets[i]->bhopTestService->IsActive())
		{
			Reply(caller, "kz_bhoptest: a test is already running on %s, use kz_bhoptest stop.", targets[i]->GetName());
			return;
		}
	}

	// Where to run: the convar if set, otherwise the player running the command.
	Vector traceStart = kz_bhoptest_origin.Get();
	f32 yaw = kz_bhoptest_yaw.Get();
	if (traceStart.IsZero())
	{
		if (!caller || !caller->GetPlayerPawn() || !(caller->GetPlayerPawn()->m_fFlags() & FL_ONGROUND))
		{
			Reply(caller, "kz_bhoptest: set kz_bhoptest_origin (and kz_bhoptest_yaw), or run it while standing on flat ground.");
			return;
		}
		QAngle callerAngles;
		caller->GetOrigin(&traceStart);
		caller->GetAngles(&callerAngles);
		yaw = callerAngles.y;
	}
	// Start slightly above the given point so a point exactly on the floor still works.
	traceStart.z += 1.0f;
	Vector floor;
	if (!FindFloor(targets[0], traceStart, floor))
	{
		Reply(caller, "kz_bhoptest: no floor found below %.1f %.1f %.1f (or the start point is inside something).", traceStart.x, traceStart.y,
			  traceStart.z - 1.0f);
		return;
	}

	i32 started = 0;
	for (i32 i = 0; i < numTargets; i++)
	{
		KZPlayer *target = targets[i];
		if (target != caller)
		{
			if (!g_pKZModeManager->SwitchToMode(target, modeName, true, true, false))
			{
				Reply(caller, "kz_bhoptest: could not switch %s to mode %s.", target->GetName(), modeName);
				continue;
			}
			g_pKZStyleManager->ClearStyles(target, true, false);
		}
		else if (V_stricmp(target->modeService->GetModeName(), modeName) && V_stricmp(target->modeService->GetModeShortName(), modeName))
		{
			Reply(caller, "kz_bhoptest: you are in %s, not %s. Your own mode is not changed.", target->modeService->GetModeName(), modeName);
		}

		KZBhopTestService::Config targetConfig = config;
		if (fallSpeed > 0.0f)
		{
			f32 gravity = target->GetCvarValueFromModeStyles(MODECVAR_SV_GRAVITY)->m_fl32Value;
			targetConfig.height = fallSpeed * fallSpeed / (2.0f * gravity);
		}
		if (targetConfig.extra == KZBhopTestService::Extra::Unduck && targetConfig.height <= 11.0f)
		{
			Reply(caller, "kz_bhoptest: extra=unduck needs a height above 11 units.");
			return;
		}
		// Players don't collide with each other, so every bot uses the same spot.
		targetConfig.startPos = floor;
		targetConfig.startAngles = QAngle(0.0f, yaw, 0.0f);
		started += target->bhopTestService->Start(targetConfig, caller) ? 1 : 0;
	}

	if (useSelf)
	{
		Reply(caller, "kz_bhoptest: testing on yourself, your inputs will be overwritten until it finishes.");
	}
	Reply(caller, "kz_bhoptest: started on %d player(s) at %.1f %.1f %.1f yaw %.1f, mode %s. Output goes to addons/cs2kz/logs/bhoptest_*.csv",
		  started, floor.x, floor.y, floor.z, yaw, useSelf ? caller->modeService->GetModeName() : modeName);
}

SCMD(kz_bhoptest, SCFL_HIDDEN)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!kz_bhoptest_enable.Get())
	{
		Reply(player, "kz_bhoptest is disabled (kz_bhoptest_enable 0).");
		return true;
	}
	RunBhopTestCommand(player, args);
	return true;
}

CON_COMMAND_F(kz_bhoptest_server, "Run kz_bhoptest from the server console (see kz_bhoptest_server help).", FCVAR_NONE)
{
	RunBhopTestCommand(nullptr, &args);
}

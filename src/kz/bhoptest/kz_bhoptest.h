#pragma once

#include "kz/kz.h"
#include <random>

class PlayerCommand;

// Debug tool: repeatedly drops a player onto flat ground and injects jump presses at controlled
// subtick times relative to the predicted landing, then logs what the modern jump code did with them.
class KZBhopTestService : public KZBaseService
{
public:
	using KZBaseService::KZBaseService;

	enum class Mode
	{
		// One jump press per trial, swept over a range of offsets from the predicted landing.
		Offset,
		// A steady train of jump presses with a random phase, landing at a random point in the tick.
		Scroll,
	};

	enum class Extra
	{
		None,
		// A press/release of IN_MOVELEFT at the extra offset, to add a subtick split.
		Strafe,
		// Duck is held from the settle phase and released at the extra offset (crouchbug/jumpbug).
		Unduck,
	};

	static constexpr i32 MAX_GAPS = 32;

	struct Config
	{
		Mode mode = Mode::Offset;
		Vector startPos;
		QAngle startAngles;
		f32 height = 42.25f;
		f32 hspeed = 275.0f;
		i32 settleTicks = 16;
		bool trace = false;

		// Offset mode
		f32 fromMs = -10.0f;
		f32 toMs = 6.0f;
		f32 stepMs = 0.25f;
		i32 repeat = 1;
		Extra extra = Extra::None;
		f32 extraOffsetMs = 0.0f;

		// Scroll mode
		f32 gapsMs[MAX_GAPS] {};
		i32 numGaps = 0;
		// Optional list of fall speeds at contact; every gap is run at every fall speed. Empty = use `height`.
		f32 fallSpeeds[MAX_GAPS] {};
		i32 numFallSpeeds = 0;
		f32 jitterMs = 0.0f;
		i32 trials = 500;
		// The press train runs from this many ticks before the predicted contact to this many after it.
		f32 preTicks = 3.0f;
		f32 postTicks = 1.5f;
	};

	~KZBhopTestService() override;

	virtual void Reset() override
	{
		this->Stop(nullptr);
	}

	bool Start(const Config &config, KZPlayer *owner);
	void Stop(const char *reason);

	bool IsActive() const
	{
		return this->phase != Phase::Idle;
	}

	void OnPlayerRunCommandPre(PlayerCommand *cmd);
	void OnProcessMovement();
	void OnProcessMovementPost();
	void OnCheckJumpButtonModern();
	void OnJumpModern();
	void OnJumpModernPost();
	void OnWalkMove();
	void OnStartTouchGround();

private:
	enum class Phase
	{
		Idle,
		Settle,
		Trial,
	};

	struct InputEvent
	{
		f32 when;
		u64 button;
		bool pressed;
	};

	struct ScheduledEvent
	{
		i32 cmd;
		f32 when;
		u64 button;
		bool pressed;
	};

	// All times are in ticks relative to the start of the launch command's movement.
	struct TrialResult
	{
		bool grounded;
		f64 groundedTime;
		bool walkMoved;
		bool jumped;
		f64 jumpTime;
		bool landedChanged;
		f64 landedTime;
		f64 usableTime;
		f64 actualTime;
		f32 speedLanded;
		f32 speedPre;
		f32 speedPost;
		f32 hoverZ;
	};

	struct CallState
	{
		bool active;
		i32 tick;
		f32 startFrac;
		f32 endFrac;
		bool groundedPre;
		f32 zPre;
		Vector velPre;
		bool checkedJump;
		bool calledOnJump;
		bool jumped;
		bool walkMoved;
		bool touchedGround;
		// IN_JUMP bit of the three server-side button state words at the start of the segment.
		u8 jumpButtonState;
	};

	Phase phase = Phase::Idle;
	Config config {};
	KZPlayer *owner {};
	std::mt19937_64 rng;

	FILE *summaryFile {};
	FILE *traceFile {};
	FILE *cmdFile {};
	char summaryPath[512] {};

	i32 numTrials {};
	i32 trialIndex {};
	i32 settleCmd {};
	i32 trialCmd {};
	u64 heldButtons {};

	f32 floorZ {};
	f64 trialHeight {};
	// Vertical velocity at launch (negative is downward).
	f64 launchVz {};
	f64 refTime {};
	f64 contactTime {};
	f64 bandTime {};
	f64 extraTime {};
	i32 numPresses {};
	f64 firstPressTime {};
	f64 lastPressTime {};
	CUtlVector<ScheduledEvent> schedule;
	i32 lastInputCmd {};
	i32 timeoutCmd {};

	bool awaitT0 {};
	bool haveT0 {};
	i32 t0Tick {};
	i32 landedTickAtLaunch {};
	f32 landedFracAtLaunch {};

	TrialResult result {};
	CallState call {};

	Vector preJumpOutWishVel;
	f32 preJumpSpeed {};
	f32 preJumpZ {};
	f64 preJumpUsable {};
	f64 preJumpActual {};

	f32 GetGravity();
	f32 GetOffsetMs() const;
	f32 GetGapMs() const;
	f32 GetTargetFallSpeed() const;
	f64 GetBaseHeight();
	void BeginSettle();
	bool BeginTrial();
	void BuildSchedule(const f64 *pressTimes, i32 count);
	void AddEvent(f64 time, u64 button, bool pressed);
	void FinishTrial();
	const char *Classify(bool *restored, bool *capped) const;
	void WriteCommand(PlayerCommand *cmd, InputEvent *events, i32 numEvents);
	f64 Rel(i32 tick, f32 frac) const;
	void Print(const char *format, ...);
};

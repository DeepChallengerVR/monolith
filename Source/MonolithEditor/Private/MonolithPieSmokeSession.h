#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/WeakObjectPtr.h"

class UWorld;
class APawn;
class FMonolithLogCapture;

// One timestamped sample of the named AnimInstance variables on the tracked pawn.
// Values are stored loosely (float OR bool) keyed by the variable name; the poll
// report summarises min/max/last per name.
struct FPieSmokeSampleVar
{
	FString Name;
	bool bIsBool = false;
	double NumberValue = 0.0; // valid when !bIsBool
	bool BoolValue = false;   // valid when bIsBool
};

struct FPieSmokeSample
{
	double TimeSeconds = 0.0; // seconds since the session's marker
	TArray<FPieSmokeSampleVar> Vars;
	// Clip variant only: path of the viewport frame captured at this sample (empty otherwise).
	FString FramePath;
	// #7 per-frame validity: a captured frame that is all-black or a single uniform colour
	// is treated as an unrendered (invalid) capture. Only meaningful when FramePath is set.
	bool bFrameValid = false;
	bool bFrameUniform = false; // single solid colour (incl. all-black) => not a real render
};

// #8 staged startup-Python / console hooks. Each stage fires AT MOST ONCE from the
// per-frame observer (reusing the probe firing mechanism), except PrePie which fires
// synchronously before PIE starts. Empty Python+Console => the stage is inert.
//   PrePie       : before StartPieInternal (driven synchronously by the handler).
//   OnBeginPlay  : first observer tick where the PIE world HasBegunPlay.
//   AfterNTicks  : first observer tick at/after FireAfterTicks observer ticks.
//   BeforeCapture: first observer tick a clip frame is about to be captured.
struct FPieSmokeStage
{
	FString Python;
	TArray<FString> Console;
	int32 FireAfterTicks = 0; // AfterNTicks only
	bool bFired = false;
	double FiredAtSeconds = -1.0;
	bool bPythonOk = false;
	FString PythonOutput;
};

// #8 the four staged hooks, keyed by lifecycle moment. Stored on the session; fired by
// the observer (PrePie excepted — the handler runs it synchronously before PIE start).
struct FPieSmokeStages
{
	FPieSmokeStage PrePie;
	FPieSmokeStage OnBeginPlay;
	FPieSmokeStage AfterNTicks;
	FPieSmokeStage BeforeCapture;
	bool bAny = false; // true if any stage carries a script (skip observer work otherwise)
};

// #9 clip runtime-identity snapshot. Cached the first sampled tick a valid AnimInstance
// is found, then re-checked each sampled tick so anim_class_changed can be detected.
struct FPieSmokeRuntimeIdentity
{
	bool bResolved = false;             // a target actor + skel comp were found at least once
	FString ActorName;
	FString ActorClass;
	FString SkelCompName;
	FString AnimInstanceClassPath;      // Anim->GetClass()->GetPathName()
	FString MeshAnimClassPath;          // SkelComp->GetAnimClass()->GetPathName()
	FString AnimationMode;              // single_node | anim_blueprint | custom | none
	bool bAnimClassChanged = false;     // MeshAnimClassPath differed across sampled ticks

	// #9 optional assert: when ExpectedAnimClass is set, bExpectedMismatch flags when the
	// live MeshAnimClassPath does not contain it (substring, path-or-name tolerant).
	FString ExpectedAnimClass;
	bool bExpectedChecked = false;
	bool bExpectedMismatch = false;
};

enum class EPieSmokeStatus : uint8
{
	Running,
	Complete,
	Stopped,
	Error,
};

// #3 grouped log-pattern semantics. Each group is a list of case-insensitive
// substring patterns matched against post-marker log lines.
//   MustAbsent  : any match fails ok (the legacy flat-array behaviour).
//   MustPresent : every pattern must match >= 1 for ok.
//   ObserveOnly : counted + reported, never affects ok.
//   Warn        : counted + surfaced in a warnings list, never affects ok.
struct FPieSmokeLogGroups
{
	TArray<FString> MustAbsent;
	TArray<FString> MustPresent;
	TArray<FString> ObserveOnly;
	TArray<FString> Warn;
};

// #4 a delayed in-session probe: fire `Python` (and/or `Console`) against the LIVE
// PIE world once session-elapsed reaches AtSeconds. Stored per session with a fired
// flag so the per-frame observer runs each probe exactly once.
struct FPieSmokeProbe
{
	double AtSeconds = 0.0;
	FString Python;
	TArray<FString> Console;
	bool bFired = false;
	double FiredAtSeconds = -1.0;
	bool bPythonOk = false;
	FString PythonOutput; // CommandResult / exception trace, when available
};

// A single async PIE-smoke session. Advanced exclusively by the frame observer
// (FTSTicker callback) running inside the editor's REAL frame — never by an MCP
// handler. All access is game-thread-only, so no locking is required.
struct FPieSmokeSession
{
	FString Id;
	EPieSmokeStatus Status = EPieSmokeStatus::Running;

	double StartTimeSeconds = 0.0;   // FPlatformTime::Seconds() at the marker emit
	double DurationSeconds = 5.0;
	double LastObservedSeconds = 0.0;

	FString Marker;
	FString MapName;
	TArray<FString> LogPatterns; // legacy flat list (kept for back-compat reporting)
	TArray<FString> SampleVarNames;

	// #3 grouped patterns (flat LogPatterns are mirrored into LogGroups.MustAbsent).
	FPieSmokeLogGroups LogGroups;

	// #10 teardown-vs-active-runtime bucketing. The first log line containing
	// IgnoreAfterPattern (default "BeginTearingDown") splits post-marker entries into
	// an active-runtime bucket (before) and a teardown bucket (after). ok is computed
	// from the active-runtime bucket only. When bTeardownAllowed is true, teardown-bucket
	// hits never affect ok (they are merely reported).
	FString IgnoreAfterPattern = TEXT("BeginTearingDown");
	bool bTeardownAllowed = true;

	// #4 delayed in-session probes, fired by the per-frame observer.
	TArray<FPieSmokeProbe> Probes;

	// #11 explicit lifecycle string, derived at report time from (Status, bPieActive,
	// resident PIE world). One of: running | capture-complete-pie-open |
	// teardown-started | teardown-complete | stopped-by-tool.
	bool bStoppedByTool = false;     // set by Stop() so lifecycle reports stopped-by-tool
	bool bTeardownStarted = false;   // set when RequestEndPlayMap is driven for this set

	// Resolved lazily on the first tick where the PIE pawn exists.
	TWeakObjectPtr<APawn> TargetPawn;
	// Optional pawn-class name filter (resolves a pawn whose class name contains this).
	FString PawnClassFilter;

	TArray<FPieSmokeSample> Samples;

	bool bPieActive = true;          // cleared by the EndPIE / PrePIEEnded delegate
	bool bReady = false;             // PIE world valid + HasBegunPlay seen at least once
	FString ErrorReason;

	// --- Clip variant (capture_pie_movement_clip) ---
	bool bCaptureFrames = false;
	double CaptureInterval = 0.25;
	double LastCaptureSeconds = -1.0; // last sample-relative time a frame was captured
	FString OutputDir;
	int32 CaptureFrameIndex = 0;
	bool bCaptureDeferred = false;    // set if viewport capture is unavailable in this build path

	// #7 capture-validity reporting (clip variant). The view target is resolved/applied at
	// session begin; the per-frame counts are tallied as frames are captured.
	FString ViewTargetActorRequest;   // optional view_target_actor param (name substring)
	FString ViewTargetActorResolved;  // actor we actually SetViewTarget'd (empty = none)
	FString ViewTargetActorClass;     // its class name
	FString ActiveViewTargetName;     // PC->GetViewTarget() name at session begin
	FString ActiveViewTargetClass;
	int32 ValidFrames = 0;            // captured frames that looked like a real render
	int32 InvalidFrames = 0;         // all-black / single-uniform-colour frames

	// #8 staged startup hooks.
	FPieSmokeStages Stages;
	int32 ObserverTickCount = 0;     // increments each AdvanceSession call (AfterNTicks gate)

	// #9 clip runtime-identity snapshot (cached + re-checked across ticks).
	FPieSmokeRuntimeIdentity Identity;
};

// Process-lifetime registry of running/finished PIE-smoke sessions plus the single
// shared frame observer. Game-thread-only (MCP calls + the ticker all run there) so
// the maps need no synchronisation. Implemented as a Meyers singleton.
class FPieSmokeSessionManager
{
public:
	static FPieSmokeSessionManager& Get();

	// Create + register a session, install the frame observer + EndPIE delegate if
	// not already present, and return the new session id.
	FString CreateSession(FPieSmokeSession&& Session);

	// Look up a session by id (nullptr if unknown).
	FPieSmokeSession* Find(const FString& SessionId);

	// Force-stop a session (RequestEndPlayMap + mark Stopped). When SessionId is
	// empty, stops every running session. Returns the number stopped.
	int32 Stop(const FString& SessionId);

	// True if any session is still Running.
	bool HasRunningSessions() const;

	// The shared log-capture pointer (post-marker counts read from it).
	void SetLogCapture(FMonolithLogCapture* InCapture) { LogCapture = InCapture; }
	FMonolithLogCapture* GetLogCapture() const { return LogCapture; }

private:
	FPieSmokeSessionManager() = default;

	// FTSTicker callback — fires on the editor's real frame (non-reentrant). Returns
	// true while the observer should keep ticking, false to self-unregister.
	bool OnFrameTick(float DeltaTime);

	// Advance one running session for this frame: resolve pawn, sample anim vars,
	// optionally capture a frame, and finish it when its duration elapses.
	void AdvanceSession(FPieSmokeSession& Session);

	// Bind / unbind the editor PIE-end delegates (marks sessions inactive so the
	// observer stops dereferencing a torn-down PIE world).
	void EnsureObserver();
	void TeardownObserverIfIdle();

	void OnPieEnded(const bool bIsSimulating);

	TMap<FString, FPieSmokeSession> Sessions;
	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle EndPieHandle;
	FDelegateHandle PrePieEndedHandle;
	bool bObserverActive = false;

	FMonolithLogCapture* LogCapture = nullptr;
	uint32 NextSessionSerial = 1;
};

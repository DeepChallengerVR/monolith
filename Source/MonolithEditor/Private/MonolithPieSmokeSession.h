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
};

enum class EPieSmokeStatus : uint8
{
	Running,
	Complete,
	Stopped,
	Error,
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
	TArray<FString> LogPatterns;
	TArray<FString> SampleVarNames;

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

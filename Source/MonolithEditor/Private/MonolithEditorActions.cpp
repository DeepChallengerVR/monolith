#include "MonolithEditorActions.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "EditorAssetLibrary.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"

#if PLATFORM_WINDOWS
#include "ILiveCodingModule.h"
#endif

// Capture action includes
#include "ProceduralMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "AdvancedPreviewScene.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraWorldManager.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "ImageUtils.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AutomatedAssetImportData.h"
#include "Engine/Texture2D.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "TextureResource.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimationAsset.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
// asset_type=widget (Phase 1 expansion)
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/Widget.h"
#include "Framework/Application/SlateApplication.h"
#include "WidgetBlueprint.h"
#include "Slate/WidgetRenderer.h"
#include "RenderDeferredCleanup.h"
#include "UObject/SavePackage.h"
#include "LevelEditorViewport.h"
#include "PixelFormat.h"
#include "ObjectTools.h"
// delete_assets: unattended-guard pattern so non-interactive deletes never raise
// a modal Slate dialog (which would freeze the game thread / in-process MCP server).
#include "CoreGlobals.h"                     // GIsRunningUnattendedScript
#include "UObject/Package.h"                 // UPackage::SetDirtyFlag
#include "Subsystems/AssetEditorSubsystem.h" // CloseAllEditorsForAsset

// Scripting action includes (HOFF 7)
#include "IPythonScriptPlugin.h"
#include "PythonScriptTypes.h"
#include "LevelEditorSubsystem.h"
#include "Editor.h"

// run_console_command needs world / PC access
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"

// PIE pre-flight compile gate + list_errored_blueprints: iterate loaded UBlueprints
// and test the same {BS_Error, bDisplayCompilePIEWarning} pair the engine's
// ResolveDirtyBlueprints uses (PlayLevel.cpp) to decide whether to raise the blocking
// "unresolved compiler errors" PIE prompt. GIsRunningUnattendedScript (CoreGlobals.h,
// included above) + TGuardValue (UnrealTemplate.h) suppress that modal in suppress mode.
#include "UObject/UObjectIterator.h"
#include "Engine/Blueprint.h"
#include "Templates/UnrealTemplate.h"

// start_pie needs the level-editor module + asset viewport to pin PIE to in-viewport mode
#include "LevelEditor.h"
#include "IAssetViewport.h"
#include "Modules/ModuleManager.h"
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"

// run_pie_smoke / poll_pie_smoke / stop_pie_smoke / capture_pie_movement_clip:
// async session-based PIE smoke advanced by the editor's real frame loop.
#include "MonolithPieSmokeSession.h"
#include "Animation/AnimInstance.h"
#include "GameFramework/Pawn.h"
#include "Misc/ScopeExit.h"           // ON_SCOPE_EXIT (always-unbind the PostPIEStarted handle)

// create_nav_harness_map: actor spawning + reflective property set + registry dispatch
#include "Engine/StaticMeshActor.h"
#include "Camera/CameraActor.h"
#include "Engine/TargetPoint.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"
#include "UObject/SoftObjectPath.h"

// --- Compile state ---

FMonolithLogCapture* FMonolithEditorActions::CachedLogCapture = nullptr;
double FMonolithEditorActions::LastCompileTimestamp = 0.0;
FString FMonolithEditorActions::LastCompileResult = TEXT("none");
bool FMonolithEditorActions::bIsCompiling = false;
bool FMonolithEditorActions::bPatchApplied = false;
double FMonolithEditorActions::LastCompileEndTimestamp = 0.0;

// --- Log capture ---

void FMonolithLogCapture::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	FScopeLock ScopeLock(&Lock);

	FMonolithLogEntry Entry;
	Entry.Timestamp = FPlatformTime::Seconds();
	Entry.Category = Category;
	Entry.Verbosity = Verbosity;
	Entry.Message = V;

	if (RingBuffer.Num() < MaxEntries)
	{
		RingBuffer.Add(MoveTemp(Entry));
	}
	else
	{
		RingBuffer[WriteIndex] = MoveTemp(Entry);
		bWrapped = true;
	}
	WriteIndex = (WriteIndex + 1) % MaxEntries;

	switch (Verbosity)
	{
	case ELogVerbosity::Fatal: ++TotalFatal; break;
	case ELogVerbosity::Error: ++TotalError; break;
	case ELogVerbosity::Warning: ++TotalWarning; break;
	case ELogVerbosity::Display:
	case ELogVerbosity::Log: ++TotalLog; break;
	case ELogVerbosity::Verbose:
	case ELogVerbosity::VeryVerbose: ++TotalVerbose; break;
	default: break;
	}
}

TArray<FMonolithLogEntry> FMonolithLogCapture::GetRecentEntries(int32 Count) const
{
	FScopeLock ScopeLock(&Lock);
	TArray<FMonolithLogEntry> Result;

	int32 Total = RingBuffer.Num();
	int32 Start = bWrapped ? WriteIndex : 0;
	int32 Num = FMath::Min(Count, Total);
	int32 Begin = bWrapped ? (WriteIndex - Num + Total) % Total : FMath::Max(0, Total - Num);

	for (int32 i = 0; i < Num; ++i)
	{
		int32 Idx = (Begin + i) % Total;
		Result.Add(RingBuffer[Idx]);
	}
	return Result;
}

TArray<FMonolithLogEntry> FMonolithLogCapture::SearchEntries(const FString& Pattern, const FString& CategoryFilter, ELogVerbosity::Type MaxVerbosity, int32 Limit) const
{
	FScopeLock ScopeLock(&Lock);
	TArray<FMonolithLogEntry> Result;

	FString PatternLower = Pattern.ToLower();
	int32 Total = RingBuffer.Num();
	int32 Start = bWrapped ? WriteIndex : 0;

	for (int32 i = 0; i < Total && Result.Num() < Limit; ++i)
	{
		int32 Idx = (Start + i) % Total;
		const FMonolithLogEntry& Entry = RingBuffer[Idx];

		if (Entry.Verbosity > MaxVerbosity) continue;
		if (!CategoryFilter.IsEmpty() && Entry.Category != FName(*CategoryFilter)) continue;
		if (!PatternLower.IsEmpty() && !Entry.Message.ToLower().Contains(PatternLower)) continue;

		Result.Add(Entry);
	}
	return Result;
}

TArray<FString> FMonolithLogCapture::GetActiveCategories() const
{
	FScopeLock ScopeLock(&Lock);
	TSet<FString> Categories;
	for (const FMonolithLogEntry& Entry : RingBuffer)
	{
		Categories.Add(Entry.Category.ToString());
	}
	return Categories.Array();
}

int32 FMonolithLogCapture::GetCountByVerbosity(ELogVerbosity::Type Verbosity) const
{
	FScopeLock ScopeLock(&Lock);
	switch (Verbosity)
	{
	case ELogVerbosity::Fatal: return TotalFatal;
	case ELogVerbosity::Error: return TotalError;
	case ELogVerbosity::Warning: return TotalWarning;
	case ELogVerbosity::Log: return TotalLog;
	case ELogVerbosity::Verbose: return TotalVerbose;
	default: return 0;
	}
}

int32 FMonolithLogCapture::GetTotalCount() const
{
	FScopeLock ScopeLock(&Lock);
	return TotalFatal + TotalError + TotalWarning + TotalLog + TotalVerbose;
}

TArray<FMonolithLogEntry> FMonolithLogCapture::GetEntriesSince(double SinceTimestamp, const TArray<FName>& CategoryFilter, ELogVerbosity::Type MaxVerbosity, int32 Limit) const
{
	FScopeLock ScopeLock(&Lock);
	TArray<FMonolithLogEntry> Result;

	int32 Total = RingBuffer.Num();
	int32 Start = bWrapped ? WriteIndex : 0;

	for (int32 i = 0; i < Total && Result.Num() < Limit; ++i)
	{
		int32 Idx = (Start + i) % Total;
		const FMonolithLogEntry& Entry = RingBuffer[Idx];

		if (Entry.Timestamp < SinceTimestamp) continue;
		if (Entry.Verbosity > MaxVerbosity) continue;
		if (CategoryFilter.Num() > 0 && !CategoryFilter.Contains(Entry.Category)) continue;

		Result.Add(Entry);
	}
	return Result;
}

int32 FMonolithLogCapture::CountErrorsSince(double SinceTimestamp) const
{
	FScopeLock ScopeLock(&Lock);
	int32 Count = 0;
	int32 Total = RingBuffer.Num();
	int32 Start = bWrapped ? WriteIndex : 0;

	for (int32 i = 0; i < Total; ++i)
	{
		int32 Idx = (Start + i) % Total;
		const FMonolithLogEntry& Entry = RingBuffer[Idx];
		if (Entry.Timestamp >= SinceTimestamp && Entry.Verbosity <= ELogVerbosity::Error)
		{
			++Count;
		}
	}
	return Count;
}

// --- Helpers ---

static FString VerbosityToString(ELogVerbosity::Type V)
{
	switch (V)
	{
	case ELogVerbosity::Fatal: return TEXT("fatal");
	case ELogVerbosity::Error: return TEXT("error");
	case ELogVerbosity::Warning: return TEXT("warning");
	case ELogVerbosity::Display: return TEXT("display");
	case ELogVerbosity::Log: return TEXT("log");
	case ELogVerbosity::Verbose: return TEXT("verbose");
	case ELogVerbosity::VeryVerbose: return TEXT("very_verbose");
	default: return TEXT("unknown");
	}
}

static ELogVerbosity::Type StringToVerbosity(const FString& S)
{
	if (S == TEXT("fatal")) return ELogVerbosity::Fatal;
	if (S == TEXT("error")) return ELogVerbosity::Error;
	if (S == TEXT("warning")) return ELogVerbosity::Warning;
	if (S == TEXT("display")) return ELogVerbosity::Display;
	if (S == TEXT("verbose")) return ELogVerbosity::Verbose;
	if (S == TEXT("very_verbose")) return ELogVerbosity::VeryVerbose;
	return ELogVerbosity::Log;
}

static TSharedPtr<FJsonObject> LogEntryToJson(const FMonolithLogEntry& Entry)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("timestamp"), Entry.Timestamp);
	Obj->SetStringField(TEXT("category"), Entry.Category.ToString());
	Obj->SetStringField(TEXT("verbosity"), VerbosityToString(Entry.Verbosity));
	Obj->SetStringField(TEXT("message"), Entry.Message);
	return Obj;
}

// --- Live Coding delegate ---

void FMonolithEditorActions::InitLiveCodingDelegate()
{
#if PLATFORM_WINDOWS
	ILiveCodingModule* LC = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (LC)
	{
		LC->GetOnPatchCompleteDelegate().AddStatic(&FMonolithEditorActions::OnLiveCodingPatchComplete);
	}
#endif
}

void FMonolithEditorActions::OnLiveCodingPatchComplete()
{
	bIsCompiling = false;
	bPatchApplied = true;
	LastCompileResult = TEXT("success");
	LastCompileEndTimestamp = FPlatformTime::Seconds();
}

static FString TimestampToIso(double PlatformSeconds)
{
	if (PlatformSeconds <= 0.0) return TEXT("never");
	FDateTime Now = FDateTime::UtcNow();
	double CurrentSeconds = FPlatformTime::Seconds();
	double Delta = CurrentSeconds - PlatformSeconds;
	FDateTime EventTime = Now - FTimespan::FromSeconds(Delta);
	return EventTime.ToIso8601();
}

// --- Registration ---

void FMonolithEditorActions::RegisterActions(FMonolithLogCapture* LogCapture)
{
	CachedLogCapture = LogCapture;
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	// Hand the shared log capture to the async PIE-smoke session manager so poll/stop
	// can compute post-marker pattern counts from the same ring buffer.
	FPieSmokeSessionManager::Get().SetLogCapture(LogCapture);

	Registry.RegisterAction(TEXT("editor"), TEXT("trigger_build"),
		TEXT("Trigger a Live Coding compile"),
		FMonolithActionHandler::CreateStatic(&HandleTriggerBuild),
		FParamSchemaBuilder()
			.Optional(TEXT("wait"), TEXT("bool"), TEXT("Block until compile finishes"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("live_compile"),
		TEXT("Trigger a Live Coding compile (alias for trigger_build)"),
		FMonolithActionHandler::CreateStatic(&HandleTriggerBuild),
		FParamSchemaBuilder()
			.Optional(TEXT("wait"), TEXT("bool"), TEXT("Block until compile finishes"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_build_errors"),
		TEXT("Get build errors and warnings"),
		FMonolithActionHandler::CreateStatic(&HandleGetBuildErrors),
		FParamSchemaBuilder()
			.Optional(TEXT("since"), TEXT("number"), TEXT("Only errors from the last N seconds ago"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Filter to a specific log category"))
			.Optional(TEXT("compile_only"), TEXT("bool"), TEXT("Filter to compile categories only"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_build_status"),
		TEXT("Check compile status: compiling, last_result, last_compile_time, errors_since_compile, patch_applied"),
		FMonolithActionHandler::CreateStatic(&HandleGetBuildStatus),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_build_summary"),
		TEXT("Get summary of last build (errors, warnings, time)"),
		FMonolithActionHandler::CreateStatic(&HandleGetBuildSummary),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("search_build_output"),
		TEXT("Search build log output by pattern"),
		FMonolithActionHandler::CreateStatic(&HandleSearchBuildOutput),
		FParamSchemaBuilder()
			.Required(TEXT("pattern"), TEXT("string"), TEXT("Search pattern"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results to return"), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_recent_logs"),
		TEXT("Get recent editor log entries"),
		FMonolithActionHandler::CreateStatic(&HandleGetRecentLogs),
		FParamSchemaBuilder()
			.Optional(TEXT("count"), TEXT("integer"), TEXT("Number of entries to return"), TEXT("50"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("search_logs"),
		TEXT("Search log entries by category, verbosity, and text pattern"),
		FMonolithActionHandler::CreateStatic(&HandleSearchLogs),
		FParamSchemaBuilder()
			.Optional(TEXT("pattern"), TEXT("string"), TEXT("Text pattern to search for"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Log category filter"))
			.Optional(TEXT("verbosity"), TEXT("string"), TEXT("Max verbosity level (error, warning, log, verbose)"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results to return"), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("tail_log"),
		TEXT("Get last N log lines"),
		FMonolithActionHandler::CreateStatic(&HandleTailLog),
		FParamSchemaBuilder()
			.Optional(TEXT("count"), TEXT("integer"), TEXT("Number of lines to return"), TEXT("50"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_log_categories"),
		TEXT("List active log categories"),
		FMonolithActionHandler::CreateStatic(&HandleGetLogCategories),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_log_stats"),
		TEXT("Get log statistics by verbosity level"),
		FMonolithActionHandler::CreateStatic(&HandleGetLogStats),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_compile_output"),
		TEXT("Get structured compile report: result, time, log lines from compile categories, error/warning counts, patch status"),
		FMonolithActionHandler::CreateStatic(&HandleGetCompileOutput),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_crash_context"),
		TEXT("Get last crash/ensure context information"),
		FMonolithActionHandler::CreateStatic(&HandleGetCrashContext),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("run_console_command"),
		TEXT("Execute a console command. Routes to the first PIE PlayerController found (multi-client PIE not disambiguated); falls back to GEngine->Exec when no PIE session is active."),
		FMonolithActionHandler::CreateStatic(&HandleRunConsoleCommand),
		FParamSchemaBuilder()
			.Required(TEXT("command"), TEXT("string"), TEXT("Console command string (e.g. 'BowLoop 1', 'WalkLoop', 'Cam3P 1')"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("start_pie"),
		TEXT("Start a Play-In-Editor session (equivalent to pressing Cmd+P in the editor)."),
		FMonolithActionHandler::CreateStatic(&HandleStartPIE),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("stop_pie"),
		TEXT("Stop the active Play-In-Editor session."),
		FMonolithActionHandler::CreateStatic(&HandleStopPIE),
		MakeShared<FJsonObject>());

	// --- Package state (F1: warband-harness plan 2026-06-04) ---

	Registry.RegisterAction(TEXT("editor"), TEXT("list_dirty_packages"),
		TEXT("Report loaded packages with unsaved changes (UPackage::IsDirty), optionally scoped to one or more /Game path prefixes. Returns per-package {package, is_map, disk_path, transient}. Use to audit what a save_packages call would touch."),
		FMonolithActionHandler::CreateStatic(&HandleListDirtyPackages),
		FParamSchemaBuilder()
			.Optional(TEXT("scope_paths"), TEXT("array"), TEXT("Array of /Game path prefixes to filter by (e.g. [\"/Game/Tests/Monolith/Warband\"]). Omit for all dirty packages. Transient/in-memory packages are excluded unless include_transient=true."))
			.Optional(TEXT("include_transient"), TEXT("bool"), TEXT("Include /Engine/Transient and other non-disk packages. Default false."), TEXT("false"))
			.Optional(TEXT("include_maps"), TEXT("bool"), TEXT("Include dirty map packages (UPackage::ContainsMap). Default true."), TEXT("true"))
			.Optional(TEXT("include_content"), TEXT("bool"), TEXT("Include dirty non-map (content) packages. Default true."), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("save_packages"),
		TEXT("Save the requested packages to disk (UPackage::SavePackage + FSavePackageArgs). When fail_on_unrequested_dirty=true, errors before saving anything if any dirty package exists outside the requested set (within scope_paths if given). Returns per-package save status."),
		FMonolithActionHandler::CreateStatic(&HandleSavePackages),
		FParamSchemaBuilder()
			.Required(TEXT("packages"), TEXT("array"), TEXT("Array of long package names (e.g. [\"/Game/Tests/Monolith/Warband/DA_Foo\"]) to save."))
			.Optional(TEXT("fail_on_unrequested_dirty"), TEXT("bool"), TEXT("If true, abort (saving nothing) when a dirty package outside the request set is found. Default false."), TEXT("false"))
			.Optional(TEXT("scope_paths"), TEXT("array"), TEXT("Path prefixes that bound the unrequested-dirty pre-scan (only used with fail_on_unrequested_dirty). Omit to scan all dirty packages."))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("If true, report which packages WOULD be saved (per-package would_save status) without writing anything to disk. Default false."), TEXT("false"))
			.Build());

	// --- PIE smoke + capture (F2/F3: warband-harness plan 2026-06-04) ---

	Registry.RegisterAction(TEXT("editor"), TEXT("run_pie_smoke"),
		TEXT("Start an ASYNC PIE smoke session on a map and RETURN IMMEDIATELY. Loads the map, starts PIE (synchronously), emits a UE_LOG marker, and registers a session that the editor's REAL frame loop advances over real frames (sampling the target pawn's AnimInstance vars). Returns {session_id, status:'running', started:true}. Poll progress / the final report with poll_pie_smoke; force-end with stop_pie_smoke. Does NOT block the editor frame (the old synchronous pump re-entered UWorld::Tick and crashed)."),
		FMonolithActionHandler::CreateStatic(&HandleRunPieSmoke),
		FParamSchemaBuilder()
			.OptionalAssetPath(TEXT("map"), TEXT("Level asset path to load before PIE (e.g. /Game/Tests/Monolith/Maps/M_Harness). Omit to use the current editor level."))
			.Optional(TEXT("marker"), TEXT("string"), TEXT("Marker token emitted to the log; post-marker pattern matching counts only lines after it. Default WARBAND_SMOKE."), TEXT("WARBAND_SMOKE"))
			.Optional(TEXT("duration"), TEXT("number"), TEXT("Seconds the editor loop advances PIE before the session auto-completes (clamped 0-120). Default 5."), TEXT("5"))
			.Optional(TEXT("sample_vars"), TEXT("array"), TEXT("AnimInstance variable names sampled each frame. Default [GroundSpeed, bShouldMove, DesiredYawDelta]."))
			.Optional(TEXT("pawn_class"), TEXT("string"), TEXT("Substring of the target pawn's class name to sample (resolves a matching pawn). Omit to use the first player controller's pawn."))
			.Optional(TEXT("console_script"), TEXT("array"), TEXT("Console command strings run on the PIE world at start (e.g. [\"WalkLoop\"])."))
			.Optional(TEXT("python_script"), TEXT("string"), TEXT("Python source run via IPythonScriptPlugin at start."))
			.Optional(TEXT("log_patterns"), TEXT("array"), TEXT("Extra case-insensitive substrings to count after the marker, in addition to the default set."))
			.Optional(TEXT("on_compile_errors"), TEXT("string"), TEXT("Policy when loaded Blueprints have unresolved compile errors: \"refuse\" (default, safe) returns an error + the offending {name,path} list and does NOT start PIE; \"suppress\" starts PIE anyway and silences the engine's blocking compile-error modal (which would otherwise freeze the editor + MCP server)."), TEXT("refuse"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("list_errored_blueprints"),
		TEXT("Read-only scan of all loaded Blueprints for unresolved compile errors (status==BS_Error && bDisplayCompilePIEWarning) — the exact condition the engine tests before raising its blocking PIE compile-error modal. Returns {count, blueprints:[{name, path}]}. Run this before run_pie_smoke to know whether PIE will be refused / blocked."),
		FMonolithActionHandler::CreateStatic(&HandleListErroredBlueprints),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("poll_pie_smoke"),
		TEXT("Poll an async PIE-smoke session by id. Returns {status (running/complete/stopped/error), elapsed_seconds, sample_count, pie_active, summarized per-var min/max/last, post_marker_counts:{pattern:count}}. When status==complete it includes the full report (all samples + captured frame paths for the clip variant). Does not advance PIE — the editor frame loop does that."),
		FMonolithActionHandler::CreateStatic(&HandlePollPieSmoke),
		FParamSchemaBuilder()
			.Required(TEXT("session_id"), TEXT("string"), TEXT("Session id returned by run_pie_smoke / capture_pie_movement_clip."))
			.Optional(TEXT("include_samples"), TEXT("bool"), TEXT("If true, include the full per-frame sample array even before completion. Default false (summary only)."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("stop_pie_smoke"),
		TEXT("Force-stop a PIE-smoke session (RequestEndPlayMap + mark stopped) and return its final report. With no session_id, stops ALL running sessions. Also serves as cleanup — the shared frame observer self-unregisters once no sessions remain running."),
		FMonolithActionHandler::CreateStatic(&HandleStopPieSmoke),
		FParamSchemaBuilder()
			.Optional(TEXT("session_id"), TEXT("string"), TEXT("Session to stop. Omit to stop every running session."))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("capture_pie_movement_clip"),
		TEXT("Start an async PIE-smoke session (same model as run_pie_smoke) that ALSO captures a PIE viewport frame every capture_interval seconds into output_path, plus per-frame AnimInstance sampling. Returns {session_id, status:'running', started:true} immediately; poll_pie_smoke returns the sampled values + captured frame paths. If viewport capture is unavailable during PIE the session continues and poll reports capture_deferred."),
		FMonolithActionHandler::CreateStatic(&HandleCapturePieMovementClip),
		FParamSchemaBuilder()
			.OptionalAssetPath(TEXT("map"), TEXT("Level asset path to load before PIE. Omit to use the current editor level."))
			.Optional(TEXT("marker"), TEXT("string"), TEXT("Log marker token. Default WARBAND_CLIP."), TEXT("WARBAND_CLIP"))
			.Optional(TEXT("duration"), TEXT("number"), TEXT("Seconds the editor loop advances PIE before the session auto-completes (clamped 0-120). Default 5."), TEXT("5"))
			.Optional(TEXT("capture_interval"), TEXT("number"), TEXT("Seconds between captured frames (clamped 0.05-5). Default 0.25."), TEXT("0.25"))
			.Optional(TEXT("sample_vars"), TEXT("array"), TEXT("AnimInstance variable names sampled each frame. Default [GroundSpeed, bShouldMove, DesiredYawDelta]."))
			.Optional(TEXT("pawn_class"), TEXT("string"), TEXT("Substring of the target pawn's class name to sample. Omit to use the first player controller's pawn."))
			.Optional(TEXT("console_script"), TEXT("array"), TEXT("Console commands run on the PIE world at start (drive the movement)."))
			.Optional(TEXT("python_script"), TEXT("string"), TEXT("Python source run at start."))
			.OptionalDiskPath(TEXT("output_path"), TEXT("Directory for frame PNGs. Default Saved/Screenshots/Monolith/PieClip/<timestamp>/"))
			.Optional(TEXT("log_patterns"), TEXT("array"), TEXT("Extra case-insensitive substrings to count after the marker."))
			.Build());

	// --- Nav harness map builder (F4: warband-harness plan 2026-06-04) ---

	Registry.RegisterAction(TEXT("editor"), TEXT("create_nav_harness_map"),
		TEXT("Build a navigation test map from a JSON spec: blank UWorld, floor, nav bounds, camera, target points, and BP/actor instances with UPROPERTY defaults (incl. FSoftObjectPath). All spawned actors get a SetFolderPath. Rebuilds + validates nav via runtime `ai` dispatch and saves. Writes to a throwaway map path only."),
		FMonolithActionHandler::CreateStatic(&HandleCreateNavHarnessMap),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("path"), TEXT("Asset path for the new UWorld (e.g. /Game/Tests/Monolith/Maps/M_NavHarness)."))
			.Optional(TEXT("floor"), TEXT("object"), TEXT("{location:[x,y,z], scale:[x,y,z], mesh:\"/Engine/BasicShapes/Plane\"}. Omitted = a default 50x50m plane at origin."))
			.Optional(TEXT("nav_bounds"), TEXT("object"), TEXT("{location:[x,y,z], extent:[x,y,z]} for the NavMeshBoundsVolume. Omitted = bounds sized to the floor."))
			.Optional(TEXT("camera"), TEXT("object"), TEXT("{location:[x,y,z], rotation:[p,y,r]} for a spawned ACameraActor."))
			.Optional(TEXT("target_points"), TEXT("array"), TEXT("[{name:\"start\", location:[x,y,z]}, ...] spawned as ATargetPoint actors; also used as nav validation points."))
			.Optional(TEXT("actors"), TEXT("array"), TEXT("[{class:\"/Game/.../BP_Foo.BP_Foo_C\", location:[x,y,z], rotation:[p,y,r], folder:\"Harness\", properties:{Prop:value, SoftRefProp:\"/Game/...\"}}, ...]"))
			.Optional(TEXT("validate_pairs"), TEXT("array"), TEXT("[{from:\"start\", to:\"goal\"}, ...] target-point name pairs that must have a nav path."))
			.Optional(TEXT("nav_timeout"), TEXT("number"), TEXT("Seconds to wait for nav generation (passed to ai.rebuild_navigation). Default 30."), TEXT("30"))
			.Build());

	// --- Capture actions ---

	Registry.RegisterAction(TEXT("editor"), TEXT("capture_scene_preview"),
		TEXT("Capture a screenshot of an asset (Niagara, material, static_mesh, skeletal_mesh, widget) rendered in a preview scene"),
		FMonolithActionHandler::CreateStatic(&HandleCaptureScenePreview),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path to preview"))
			.Required(TEXT("asset_type"), TEXT("string"), TEXT("niagara | material | static_mesh | skeletal_mesh | widget"))
			.Optional(TEXT("preview_mesh"), TEXT("string"), TEXT("Mesh for materials: plane, sphere, cube"), TEXT("plane"))
			.Optional(TEXT("seek_time"), TEXT("number"), TEXT("Advance Niagara sim or skeletal animation to this time (seconds)"), TEXT("0.0"))
			.OptionalAssetPath(TEXT("animation_path"), TEXT("skeletal_mesh only: UAnimSequence to pose with at seek_time"))
			.Optional(TEXT("scale"), TEXT("number"), TEXT("widget only: DPI multiplier (>=0.01)"), TEXT("1.0"))
			.Optional(TEXT("camera"), TEXT("object"), TEXT("{location:[x,y,z], rotation:[p,y,r], fov:60}"))
			.Optional(TEXT("resolution"), TEXT("array"), TEXT("[width, height]"), TEXT("[512,512]"))
			.OptionalDiskPath(TEXT("output_path"), TEXT("Output PNG path (absolute or relative to project)"))
			.Build());

	// --- Inspect actions (Phase 2: 2026-05-26-monolith-editor-preview-expansion plan) ---
	// Pure reflection / source-mip-read. No render path. Bodies in
	// MonolithEditorInspectActions.cpp.

	Registry.RegisterAction(TEXT("editor"), TEXT("inspect_material_pbr"),
		TEXT("Inspect a UMaterialInterface's PBR parameter set. Returns scalar/vector/texture parameter lists plus heuristic classification of base color / normal / roughness / metallic textures and ORM / ARM / MRA channel-packing detection. Pure reflection — no render, no thumbnail. Use this when capture_scene_preview's pixel output isn't enough and you need the actual parameter values."),
		FMonolithActionHandler::CreateStatic(&HandleInspectMaterialPBR),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("UMaterialInterface asset path (e.g. /Game/Materials/M_Foo or /Game/Materials/MI_Foo)"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("inspect_texture_channels"),
		TEXT("Inspect a UTexture2D's R/G/B/A channel statistics (min/max/mean per channel) plus format/dimensions/sRGB/alpha. Optional per-channel split PNGs for visual debugging. Reads source mip 0 directly — bypasses runtime mip selection and compression. Useful for ORM/ARM channel-packing audits, alpha-coverage checks, and verifying source authoring against runtime appearance."),
		FMonolithActionHandler::CreateStatic(&HandleInspectTextureChannels),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("UTexture2D asset path (e.g. /Game/Textures/T_Foo)"))
			.Optional(TEXT("emit_splits"), TEXT("bool"), TEXT("If true, emit 4 grayscale PNGs (R/G/B/A) under output_dir. Default false (stats only)."), TEXT("false"))
			.OptionalDiskPath(TEXT("output_dir"), TEXT("Output directory for split PNGs (default: Saved/Tests/Monolith/InspectTexture/<TextureName>/)"))
			.Build());

	// --- Composite-capture actions (Phase 3: 2026-05-26-monolith-editor-preview-expansion plan) ---
	// Multi-asset / show-flag overlay captures. Bodies live in
	// MonolithEditorPreviewActions.cpp.

	Registry.RegisterAction(TEXT("editor"), TEXT("capture_material_grid"),
		TEXT("Render N materials side-by-side on identical preview meshes in ONE scene, ONE camera, ONE PNG. Shares lighting + HDRI across all cells (the value-add over N separate captures). Auto-grid layout via ceil(sqrt(N)) columns unless overridden. Use when comparing material variations visually — e.g. tweaking roughness across MI tiers, A/B-testing a master vs an instance, or auditing a packs's hero/variant materials."),
		FMonolithActionHandler::CreateStatic(&HandleCaptureMaterialGrid),
		FParamSchemaBuilder()
			.Required(TEXT("material_paths"), TEXT("array"), TEXT("Array of UMaterialInterface asset paths (1..16). Each becomes one grid cell."))
			.OptionalDiskPath(TEXT("output_path"), TEXT("Output PNG path. Default: Saved/Screenshots/Monolith/CaptureMaterialGrid/<timestamp>.png"))
			.Optional(TEXT("resolution"), TEXT("array"), TEXT("[width, height] total grid PNG size. Default [1024, 1024]."), TEXT("[1024,1024]"))
			.Optional(TEXT("columns"), TEXT("integer"), TEXT("Grid columns. Default: ceil(sqrt(material_count))."))
			.Optional(TEXT("preview_mesh"), TEXT("string"), TEXT("Mesh per cell: plane | sphere | cube. Default sphere."), TEXT("sphere"))
			.Optional(TEXT("camera"), TEXT("object"), TEXT("{location:[x,y,z], rotation:[p,y,r], fov:60} — overrides auto-framing"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("capture_with_overlay"),
		TEXT("Render a static mesh with an FEngineShowFlags overlay (wireframe | normals | uv_density | lightmap_density | shader_complexity). Useful for visual debugging — overdraw audits, UV-density checks, lightmap-density layout review, shader-complexity heatmaps. Static-mesh only in v1 (skeletal/material flavours can be added later)."),
		FMonolithActionHandler::CreateStatic(&HandleCaptureWithOverlay),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("UStaticMesh asset path (e.g. /Engine/BasicShapes/Cube)"))
			.Required(TEXT("mode"), TEXT("string"), TEXT("Overlay: wireframe | normals | uv_density | lightmap_density | shader_complexity"))
			.OptionalDiskPath(TEXT("output_path"), TEXT("Output PNG path. Default: Saved/Screenshots/Monolith/CaptureWithOverlay/<timestamp>.png"))
			.Optional(TEXT("resolution"), TEXT("array"), TEXT("[width, height]. Default [512, 512]."), TEXT("[512,512]"))
			.Optional(TEXT("camera"), TEXT("object"), TEXT("{location:[x,y,z], rotation:[p,y,r], fov:60}"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("capture_sequence_frames"),
		TEXT("Capture multiple frames of an animated effect at specified timestamps"),
		FMonolithActionHandler::CreateStatic(&HandleCaptureSequenceFrames),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path to preview"))
			.Required(TEXT("asset_type"), TEXT("string"), TEXT("niagara"))
			.Required(TEXT("timestamps"), TEXT("array"), TEXT("Array of capture times in seconds"))
			.Optional(TEXT("camera"), TEXT("object"), TEXT("{location:[x,y,z], rotation:[p,y,r], fov:60}"))
			.Optional(TEXT("resolution"), TEXT("array"), TEXT("[width, height]"), TEXT("[512,512]"))
			.OptionalDiskPath(TEXT("output_dir"), TEXT("Output directory for frame PNGs"))
			.Optional(TEXT("filename_prefix"), TEXT("string"), TEXT("Prefix for frame files"), TEXT("frame"))
			.Optional(TEXT("persistent"), TEXT("bool"), TEXT("Use persistent component (preserves ribbons/accumulation). Default: false (per-frame recreate)."))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("import_texture"),
		TEXT("Import an external image as a UTexture2D with configurable settings"),
		FMonolithActionHandler::CreateStatic(&HandleImportTexture),
		FParamSchemaBuilder()
			.RequiredDiskPath(TEXT("source_path"), TEXT("Absolute path to source image (PNG, TGA, EXR, HDR)"))
			.Required(TEXT("destination"), TEXT("string"), TEXT("UE asset path for imported texture"))
			.Optional(TEXT("settings"), TEXT("object"), TEXT("{compression, srgb, tiling, max_size, lod_group}"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("stitch_flipbook"),
		TEXT("Stitch individual frame images into a flipbook atlas texture and import as UTexture2D"),
		FMonolithActionHandler::CreateStatic(&HandleStitchFlipbook),
		FParamSchemaBuilder()
			.Required(TEXT("frame_paths"), TEXT("array"), TEXT("Ordered array of absolute file paths to frame PNGs"))
			.RequiredAssetPath(TEXT("dest_path"), TEXT("UE asset path for the output texture (e.g. /Game/AgentTraining/Textures/T_FB_001)"))
			.Required(TEXT("grid"), TEXT("array"), TEXT("[columns, rows] grid layout (e.g. [4, 4] for 16 frames)"))
			.Optional(TEXT("srgb"), TEXT("bool"), TEXT("sRGB color space (true for color, false for masks)"), TEXT("true"))
			.Optional(TEXT("no_mipmaps"), TEXT("bool"), TEXT("Disable mipmap generation to prevent atlas bleed"), TEXT("true"))
			.Optional(TEXT("delete_sources"), TEXT("bool"), TEXT("Delete source PNG files after successful stitch"), TEXT("true"))
			.Optional(TEXT("lod_group"), TEXT("string"), TEXT("Texture LOD group"), TEXT("TEXTUREGROUP_Effects"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("delete_assets"),
		TEXT("Delete UE assets by path. Optional safety: restrict to allowed path prefixes"),
		FMonolithActionHandler::CreateStatic(&HandleDeleteAssets),
		FParamSchemaBuilder()
			.Required(TEXT("asset_paths"), TEXT("array"), TEXT("Array of UE asset paths to delete"))
			.Optional(TEXT("allowed_prefixes"), TEXT("array"), TEXT("If set, only paths starting with one of these prefixes can be deleted (e.g. [\"/Game/AgentTraining/\"])"))
			.Optional(TEXT("force"), TEXT("bool"), TEXT("When true, force-delete even if referenced (nulls referencers, including EXTERNAL ones, silently). Default false: soft-delete after closing open asset editors. Use allowed_prefixes as a sandbox when force=true."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_viewport_info"),
		TEXT("Get current editor viewport camera position, rotation, FOV, and resolution"),
		FMonolithActionHandler::CreateStatic(&HandleGetViewportInfo),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("capture_system_gif"),
		TEXT("Capture a Niagara system as a sequence of PNG frames with optional GIF encoding via ffmpeg or python"),
		FMonolithActionHandler::CreateStatic(&HandleCaptureSystemGif),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Optional(TEXT("duration_seconds"), TEXT("number"), TEXT("Capture duration in seconds (default: 2.0)"))
			.Optional(TEXT("fps"), TEXT("integer"), TEXT("Frames per second (default: 15)"))
			.Optional(TEXT("resolution"), TEXT("integer"), TEXT("Output resolution width/height in pixels (default: 256)"))
			.OptionalDiskPath(TEXT("output_path"), TEXT("Output directory (default: Saved/Screenshots/Monolith/GIF_<timestamp>)"))
			.Optional(TEXT("encoder"), TEXT("string"), TEXT("frames_only (default), ffmpeg, or python — opt-in GIF encoding"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("list_automation_tests"),
		TEXT("List all registered automation tests, optionally filtered by prefix"),
		FMonolithActionHandler::CreateStatic(&HandleListAutomationTests),
		FParamSchemaBuilder()
			.Optional(TEXT("prefix"), TEXT("string"), TEXT("Filter tests whose full path starts with this prefix (e.g. 'MazeLegends.Bow')"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("run_automation_tests"),
		TEXT("Run automation tests by prefix in the running editor (no PIE, no separate process). Returns success/passed/failed counts and per-test errors."),
		FMonolithActionHandler::CreateStatic(&HandleRunAutomationTests),
		FParamSchemaBuilder()
			.Required(TEXT("prefix"), TEXT("string"), TEXT("Run tests whose full path starts with this prefix (e.g. 'MazeLegends.Bow')"))
			.Optional(TEXT("max_tests"), TEXT("integer"), TEXT("Hard cap on number of tests to run (default: 200)"))
			.Build());

	// --- Scripting actions (HOFF 7) ---

	Registry.RegisterAction(TEXT("editor"), TEXT("run_python"),
		TEXT("Execute a Python command, statement, or file via IPythonScriptPlugin::ExecPythonCommandEx. Returns success, stdout/stderr captured by Python, and (for evaluate_statement mode) the evaluated result."),
		FMonolithActionHandler::CreateStatic(&HandleRunPython),
		FParamSchemaBuilder()
			.Required(TEXT("command"), TEXT("string"), TEXT("Python source. May be inline code, a single statement, or a file path with optional space-separated args (when mode=execute_file)."))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("Execution mode: execute_file (default — multi-statement script or file with args), execute_statement (single stmt, prints result), evaluate_statement (single expr, returns result in 'result')."), TEXT("execute_file"))
			.Optional(TEXT("unattended"), TEXT("bool"), TEXT("Set GIsRunningUnattendedScript=true to suppress UI dialogs."), TEXT("false"))
			.Optional(TEXT("file_scope"), TEXT("string"), TEXT("Scope for execute_file: private (isolated locals/globals — default), public (shared with REPL console)."), TEXT("private"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("load_level"),
		TEXT("Close the current persistent level (without saving) and load the specified level by /Game/... asset path. Wraps ULevelEditorSubsystem::LoadLevel."),
		FMonolithActionHandler::CreateStatic(&HandleLoadLevel),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("path"), TEXT("Asset path of the level to load (e.g. /Game/Maps/L_Backyard). Must exist."))
			.Build());

	InitLiveCodingDelegate();
}

// --- Build actions ---

FMonolithActionResult FMonolithEditorActions::HandleTriggerBuild(const TSharedPtr<FJsonObject>& Params)
{
#if PLATFORM_WINDOWS
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (!LiveCoding)
	{
		return FMonolithActionResult::Error(TEXT("Live Coding module not available"));
	}

	if (!LiveCoding->IsEnabledForSession() && !LiveCoding->IsEnabledByDefault())
	{
		LiveCoding->EnableByDefault(true);
		LiveCoding->EnableForSession(true);
	}

	if (LiveCoding->IsCompiling())
	{
		return FMonolithActionResult::Error(TEXT("A compile is already in progress"));
	}

	bool bWait = false;
	if (Params->HasField(TEXT("wait")))
	{
		bWait = Params->GetBoolField(TEXT("wait"));
	}

	LastCompileTimestamp = FPlatformTime::Seconds();
	bIsCompiling = true;
	bPatchApplied = false;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	if (bWait)
	{
		ELiveCodingCompileResult CompileResult;
		bool bStarted = LiveCoding->Compile(ELiveCodingCompileFlags::WaitForCompletion, &CompileResult);

		bIsCompiling = false;
		LastCompileEndTimestamp = FPlatformTime::Seconds();
		Root->SetBoolField(TEXT("started"), bStarted);

		FString ResultStr;
		switch (CompileResult)
		{
		case ELiveCodingCompileResult::Success: ResultStr = TEXT("success"); bPatchApplied = true; break;
		case ELiveCodingCompileResult::NoChanges: ResultStr = TEXT("no_changes"); break;
		case ELiveCodingCompileResult::Failure: ResultStr = TEXT("failure"); break;
		case ELiveCodingCompileResult::Cancelled: ResultStr = TEXT("cancelled"); break;
		case ELiveCodingCompileResult::CompileStillActive: ResultStr = TEXT("compile_still_active"); break;
		case ELiveCodingCompileResult::NotStarted: ResultStr = TEXT("not_started"); break;
		default: ResultStr = TEXT("unknown"); break;
		}
		LastCompileResult = ResultStr;
		Root->SetStringField(TEXT("result"), ResultStr);
	}
	else
	{
		LiveCoding->Compile();
		LastCompileResult = TEXT("in_progress");
		Root->SetBoolField(TEXT("started"), true);
		Root->SetStringField(TEXT("result"), TEXT("in_progress"));
	}

	return FMonolithActionResult::Success(Root);
#else
	return FMonolithActionResult::Error(TEXT("Live Coding is only available on Windows"));
#endif
}

FMonolithActionResult FMonolithEditorActions::HandleGetBuildErrors(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ErrorsArr;
	TArray<TSharedPtr<FJsonValue>> WarningsArr;

	// Determine time window
	double SinceTimestamp = LastCompileTimestamp; // Default: since last compile
	if (Params->HasField(TEXT("since")))
	{
		double SecondsAgo = Params->GetNumberField(TEXT("since"));
		SinceTimestamp = FPlatformTime::Seconds() - SecondsAgo;
	}

	// Build category filter
	TArray<FName> CategoryFilter;
	bool bCompileOnly = false;
	if (Params->HasField(TEXT("compile_only")))
	{
		bCompileOnly = Params->GetBoolField(TEXT("compile_only"));
	}
	if (bCompileOnly)
	{
		CategoryFilter.Add(FName(TEXT("LogLiveCoding")));
		CategoryFilter.Add(FName(TEXT("LogCompile")));
		CategoryFilter.Add(FName(TEXT("LogLinker")));
	}
	else if (Params->HasField(TEXT("category")))
	{
		CategoryFilter.Add(FName(*Params->GetStringField(TEXT("category"))));
	}

	if (CachedLogCapture)
	{
		TArray<FMonolithLogEntry> Entries = CachedLogCapture->GetEntriesSince(
			SinceTimestamp, CategoryFilter, ELogVerbosity::Warning, 500);

		for (const FMonolithLogEntry& Entry : Entries)
		{
			if (Entry.Verbosity <= ELogVerbosity::Error)
			{
				TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
				ErrObj->SetStringField(TEXT("message"), Entry.Message);
				ErrObj->SetStringField(TEXT("category"), Entry.Category.ToString());
				ErrObj->SetStringField(TEXT("verbosity"), VerbosityToString(Entry.Verbosity));
				ErrorsArr.Add(MakeShared<FJsonValueObject>(ErrObj));
			}
			else if (Entry.Verbosity == ELogVerbosity::Warning)
			{
				TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
				WarnObj->SetStringField(TEXT("message"), Entry.Message);
				WarnObj->SetStringField(TEXT("category"), Entry.Category.ToString());
				WarningsArr.Add(MakeShared<FJsonValueObject>(WarnObj));
			}
		}
	}

	Root->SetNumberField(TEXT("error_count"), ErrorsArr.Num());
	Root->SetArrayField(TEXT("errors"), ErrorsArr);
	Root->SetNumberField(TEXT("warning_count"), WarningsArr.Num());
	Root->SetArrayField(TEXT("warnings"), WarningsArr);
	Root->SetStringField(TEXT("since"), TimestampToIso(SinceTimestamp));

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleGetBuildStatus(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

#if PLATFORM_WINDOWS
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (LiveCoding)
	{
		bool bCurrentlyCompiling = LiveCoding->IsCompiling();

		// Update tracked state if LC reports done but we haven't caught it yet
		if (bIsCompiling && !bCurrentlyCompiling)
		{
			bIsCompiling = false;
			if (LastCompileEndTimestamp < LastCompileTimestamp)
			{
				LastCompileEndTimestamp = FPlatformTime::Seconds();
			}
		}

		Root->SetBoolField(TEXT("live_coding_available"), true);
		Root->SetBoolField(TEXT("live_coding_started"), LiveCoding->HasStarted());
		Root->SetBoolField(TEXT("live_coding_enabled"), LiveCoding->IsEnabledForSession());
		Root->SetBoolField(TEXT("compiling"), bCurrentlyCompiling);
	}
	else
	{
		Root->SetBoolField(TEXT("live_coding_available"), false);
		Root->SetBoolField(TEXT("compiling"), false);
	}
#else
	Root->SetBoolField(TEXT("live_coding_available"), false);
	Root->SetBoolField(TEXT("compiling"), false);
#endif

	Root->SetStringField(TEXT("last_result"), LastCompileResult);
	Root->SetStringField(TEXT("last_compile_time"), TimestampToIso(LastCompileTimestamp));
	Root->SetBoolField(TEXT("patch_applied"), bPatchApplied);

	if (CachedLogCapture && LastCompileTimestamp > 0.0)
	{
		Root->SetNumberField(TEXT("errors_since_compile"), CachedLogCapture->CountErrorsSince(LastCompileTimestamp));
	}
	else
	{
		Root->SetNumberField(TEXT("errors_since_compile"), 0);
	}

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleGetBuildSummary(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	// Get error/warning counts from log capture
	int32 ErrorCount = 0;
	int32 WarningCount = 0;

	if (CachedLogCapture)
	{
		ErrorCount = CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Error);
		WarningCount = CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Warning);
	}

	Root->SetNumberField(TEXT("total_errors"), ErrorCount);
	Root->SetNumberField(TEXT("total_warnings"), WarningCount);

#if PLATFORM_WINDOWS
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (LiveCoding)
	{
		Root->SetBoolField(TEXT("compiling"), LiveCoding->IsCompiling());
		Root->SetBoolField(TEXT("live_coding_started"), LiveCoding->HasStarted());
	}
#endif

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleSearchBuildOutput(const TSharedPtr<FJsonObject>& Params)
{
	FString Pattern = Params->GetStringField(TEXT("pattern"));
	if (Pattern.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: pattern"));
	}

	int32 Limit = 100;
	if (Params->HasField(TEXT("limit")))
	{
		Limit = static_cast<int32>(Params->GetNumberField(TEXT("limit")));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Matches;

	if (CachedLogCapture)
	{
		// Search for compile-related messages matching the pattern
		TArray<FMonolithLogEntry> Entries = CachedLogCapture->SearchEntries(
			Pattern, TEXT(""), ELogVerbosity::VeryVerbose, Limit);

		for (const FMonolithLogEntry& Entry : Entries)
		{
			Matches.Add(MakeShared<FJsonValueObject>(LogEntryToJson(Entry)));
		}
	}

	Root->SetStringField(TEXT("pattern"), Pattern);
	Root->SetNumberField(TEXT("match_count"), Matches.Num());
	Root->SetArrayField(TEXT("matches"), Matches);

	return FMonolithActionResult::Success(Root);
}

// --- Compile output ---

FMonolithActionResult FMonolithEditorActions::HandleGetCompileOutput(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	Root->SetStringField(TEXT("last_result"), LastCompileResult);
	Root->SetStringField(TEXT("last_compile_time"), TimestampToIso(LastCompileTimestamp));
	Root->SetStringField(TEXT("last_compile_end_time"), TimestampToIso(LastCompileEndTimestamp));
	Root->SetBoolField(TEXT("patch_applied"), bPatchApplied);
	Root->SetBoolField(TEXT("compiling"), bIsCompiling);

	int32 ErrorCount = 0;
	int32 WarningCount = 0;
	TArray<TSharedPtr<FJsonValue>> LogLines;

	if (CachedLogCapture && LastCompileTimestamp > 0.0)
	{
		// Get all log lines from compile-related categories since last compile
		TArray<FName> CompileCategories;
		CompileCategories.Add(FName(TEXT("LogLiveCoding")));
		CompileCategories.Add(FName(TEXT("LogCompile")));
		CompileCategories.Add(FName(TEXT("LogLinker")));

		TArray<FMonolithLogEntry> Entries = CachedLogCapture->GetEntriesSince(
			LastCompileTimestamp, CompileCategories, ELogVerbosity::VeryVerbose, 500);

		for (const FMonolithLogEntry& Entry : Entries)
		{
			LogLines.Add(MakeShared<FJsonValueObject>(LogEntryToJson(Entry)));
			if (Entry.Verbosity <= ELogVerbosity::Error) ++ErrorCount;
			else if (Entry.Verbosity == ELogVerbosity::Warning) ++WarningCount;
		}
	}

	Root->SetNumberField(TEXT("error_count"), ErrorCount);
	Root->SetNumberField(TEXT("warning_count"), WarningCount);
	Root->SetNumberField(TEXT("log_line_count"), LogLines.Num());
	Root->SetArrayField(TEXT("compile_log"), LogLines);

	return FMonolithActionResult::Success(Root);
}

// --- Log actions ---

FMonolithActionResult FMonolithEditorActions::HandleGetRecentLogs(const TSharedPtr<FJsonObject>& Params)
{
	int32 Count = 100;
	if (Params->HasField(TEXT("count")))
	{
		Count = static_cast<int32>(Params->GetNumberField(TEXT("count")));
	}
	else if (Params->HasField(TEXT("max")))
	{
		Count = static_cast<int32>(Params->GetNumberField(TEXT("max")));
	}
	Count = FMath::Clamp(Count, 1, 1000);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> LogArr;

	if (CachedLogCapture)
	{
		TArray<FMonolithLogEntry> Entries = CachedLogCapture->GetRecentEntries(Count);
		for (const FMonolithLogEntry& Entry : Entries)
		{
			LogArr.Add(MakeShared<FJsonValueObject>(LogEntryToJson(Entry)));
		}
	}

	Root->SetNumberField(TEXT("count"), LogArr.Num());
	Root->SetArrayField(TEXT("entries"), LogArr);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleSearchLogs(const TSharedPtr<FJsonObject>& Params)
{
	FString Pattern = Params->GetStringField(TEXT("pattern"));
	FString Category = Params->GetStringField(TEXT("category"));
	FString VerbosityStr = Params->GetStringField(TEXT("verbosity"));
	ELogVerbosity::Type MaxVerbosity = VerbosityStr.IsEmpty() ? ELogVerbosity::VeryVerbose : StringToVerbosity(VerbosityStr);

	int32 Limit = 200;
	if (Params->HasField(TEXT("limit")))
	{
		Limit = static_cast<int32>(Params->GetNumberField(TEXT("limit")));
	}
	Limit = FMath::Clamp(Limit, 1, 2000);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> LogArr;

	if (CachedLogCapture)
	{
		TArray<FMonolithLogEntry> Entries = CachedLogCapture->SearchEntries(Pattern, Category, MaxVerbosity, Limit);
		for (const FMonolithLogEntry& Entry : Entries)
		{
			LogArr.Add(MakeShared<FJsonValueObject>(LogEntryToJson(Entry)));
		}
	}

	Root->SetNumberField(TEXT("match_count"), LogArr.Num());
	Root->SetArrayField(TEXT("entries"), LogArr);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleTailLog(const TSharedPtr<FJsonObject>& Params)
{
	int32 Count = 50;
	if (Params->HasField(TEXT("count")))
	{
		Count = static_cast<int32>(Params->GetNumberField(TEXT("count")));
	}
	Count = FMath::Clamp(Count, 1, 500);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Lines;

	if (CachedLogCapture)
	{
		TArray<FMonolithLogEntry> Entries = CachedLogCapture->GetRecentEntries(Count);
		for (const FMonolithLogEntry& Entry : Entries)
		{
			FString Line = FString::Printf(TEXT("[%s][%s] %s"),
				*Entry.Category.ToString(),
				*VerbosityToString(Entry.Verbosity),
				*Entry.Message);
			Lines.Add(MakeShared<FJsonValueString>(Line));
		}
	}

	Root->SetNumberField(TEXT("count"), Lines.Num());
	Root->SetArrayField(TEXT("lines"), Lines);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleGetLogCategories(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> CatArr;

	if (CachedLogCapture)
	{
		TArray<FString> Categories = CachedLogCapture->GetActiveCategories();
		Categories.Sort();
		for (const FString& Cat : Categories)
		{
			CatArr.Add(MakeShared<FJsonValueString>(Cat));
		}
	}

	Root->SetNumberField(TEXT("count"), CatArr.Num());
	Root->SetArrayField(TEXT("categories"), CatArr);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleGetLogStats(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	if (CachedLogCapture)
	{
		Root->SetNumberField(TEXT("total"), CachedLogCapture->GetTotalCount());
		Root->SetNumberField(TEXT("fatal"), CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Fatal));
		Root->SetNumberField(TEXT("error"), CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Error));
		Root->SetNumberField(TEXT("warning"), CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Warning));
		Root->SetNumberField(TEXT("log"), CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Log));
		Root->SetNumberField(TEXT("verbose"), CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Verbose));
	}
	else
	{
		Root->SetNumberField(TEXT("total"), 0);
		Root->SetStringField(TEXT("status"), TEXT("log_capture_not_initialized"));
	}

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleGetCrashContext(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	// Check for crash log file on disk
	FString CrashLogPath = FPaths::ProjectLogDir() / TEXT("CrashContext.runtime-xml");
	bool bHasCrashLog = FPaths::FileExists(CrashLogPath);
	Root->SetBoolField(TEXT("has_crash_context"), bHasCrashLog);

	if (bHasCrashLog)
	{
		FString CrashXml;
		if (FFileHelper::LoadFileToString(CrashXml, *CrashLogPath))
		{
			// Truncate if very large
			if (CrashXml.Len() > 4096)
			{
				CrashXml = CrashXml.Left(4096) + TEXT("...(truncated)");
			}
			Root->SetStringField(TEXT("crash_xml"), CrashXml);
		}
	}

	// Also check ensure log
	FString EnsureLogPath = FPaths::ProjectLogDir() / TEXT("Ensures.log");
	if (FPaths::FileExists(EnsureLogPath))
	{
		FString EnsureLog;
		if (FFileHelper::LoadFileToString(EnsureLog, *EnsureLogPath))
		{
			if (EnsureLog.Len() > 4096)
			{
				EnsureLog = EnsureLog.Right(4096);
			}
			Root->SetStringField(TEXT("ensure_log"), EnsureLog);
		}
	}

	// Provide recent errors/fatals from log capture
	if (CachedLogCapture)
	{
		TArray<FMonolithLogEntry> ErrorEntries = CachedLogCapture->SearchEntries(
			TEXT(""), TEXT(""), ELogVerbosity::Error, 20);
		TArray<TSharedPtr<FJsonValue>> RecentErrors;
		for (const FMonolithLogEntry& Entry : ErrorEntries)
		{
			RecentErrors.Add(MakeShared<FJsonValueObject>(LogEntryToJson(Entry)));
		}
		Root->SetArrayField(TEXT("recent_errors"), RecentErrors);
	}

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// run_console_command — execute a console command on the active world
// ---------------------------------------------------------------------------
FMonolithActionResult FMonolithEditorActions::HandleRunConsoleCommand(const TSharedPtr<FJsonObject>& Params)
{
	FString Command;
	if (!Params->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required field: command"));
	}

	// Resolve a target world: prefer an active PIE world (so exec functions on
	// the player character actually fire), fall back to the editor world.
	UWorld* TargetWorld = nullptr;
	FString WorldType = TEXT("none");
	if (GEditor)
	{
		for (const FWorldContext& Context : GEditor->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE && Context.World())
			{
				TargetWorld = Context.World();
				WorldType = TEXT("pie");
				break;
			}
		}
		if (!TargetWorld)
		{
			TargetWorld = GEditor->GetEditorWorldContext().World();
			WorldType = TEXT("editor");
		}
	}

	if (!TargetWorld)
	{
		return FMonolithActionResult::Error(TEXT("No usable world found (no PIE active and no editor world)"));
	}

	// Prefer the player controller's command path so exec UFUNCTIONs on the
	// possessed pawn (BowLoop, WalkLoop, Cam3P, …) get dispatched. Fall back
	// to the world-level Exec for cheats that don't need a PC.
	bool bExecutedViaPC = false;
	if (APlayerController* PC = TargetWorld->GetFirstPlayerController())
	{
		PC->ConsoleCommand(Command, /*bWriteToLog=*/true);
		bExecutedViaPC = true;
	}
	else
	{
		if (!GEngine)
		{
			return FMonolithActionResult::Error(TEXT("GEngine is null — run_console_command requires engine context."));
		}
		GEngine->Exec(TargetWorld, *Command);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("command"), Command);
	Root->SetStringField(TEXT("world"), WorldType);
	Root->SetBoolField(TEXT("via_player_controller"), bExecutedViaPC);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// start_pie / stop_pie — drive Play-In-Editor sessions programmatically
// ---------------------------------------------------------------------------
UWorld* FMonolithEditorActions::FindActivePieWorld()
{
	if (!GEditor)
	{
		return nullptr;
	}
	for (const FWorldContext& Context : GEditor->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE && Context.World())
		{
			return Context.World();
		}
	}
	return nullptr;
}

namespace
{
	// One errored Blueprint discovered by the PIE pre-flight scan.
	struct FErroredBlueprintEntry
	{
		FString Name;
		FString Path;
	};

	// Scan every loaded UBlueprint for the same condition the engine's
	// ResolveDirtyBlueprints (PlayLevel.cpp) tests before raising the blocking
	// "unresolved compiler errors" PIE prompt: status == BS_Error AND the BP still
	// wants to warn on PIE (bDisplayCompilePIEWarning). Starting PIE on such a world
	// pops a Slate modal that runs a nested loop on the game thread, starving the
	// in-process MCP HTTP server until a human clicks.
	void ScanErroredBlueprints(TArray<FErroredBlueprintEntry>& OutErrored)
	{
		for (TObjectIterator<UBlueprint> It; It; ++It)
		{
			UBlueprint* Blueprint = *It;
			if (!IsValid(Blueprint))
			{
				continue;
			}
			if (Blueprint->Status == BS_Error && Blueprint->bDisplayCompilePIEWarning)
			{
				FErroredBlueprintEntry Entry;
				Entry.Name = Blueprint->GetName();
				Entry.Path = Blueprint->GetPathName();
				OutErrored.Add(MoveTemp(Entry));
			}
		}
	}

	// Build a JSON array of {name, path} from a scan result.
	TArray<TSharedPtr<FJsonValue>> ErroredBlueprintsToJson(const TArray<FErroredBlueprintEntry>& Errored)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Reserve(Errored.Num());
		for (const FErroredBlueprintEntry& Entry : Errored)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Entry.Name);
			Obj->SetStringField(TEXT("path"), Entry.Path);
			Items.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Items;
	}
}

bool FMonolithEditorActions::StartPieInternal(FString& OutError, bool bSuppressModals)
{
	if (!GUnrealEd)
	{
		OutError = TEXT("GUnrealEd not available");
		return false;
	}

	// Reject if a PIE session is already running so we don't queue duplicates.
	if (FindActivePieWorld())
	{
		OutError = TEXT("PIE already running");
		return false;
	}

	// Pin to in-viewport mode so the action is independent of the user's
	// last-used PIE flavour (Simulate / NewWindow / etc.).
	FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
	TSharedPtr<IAssetViewport> ActiveLevelViewport = LevelEditorModule.GetFirstActiveViewport();
	if (!ActiveLevelViewport.IsValid())
	{
		OutError = TEXT("No active level viewport — cannot pin PIE to in-viewport mode.");
		return false;
	}

	FRequestPlaySessionParams SessionParams;
	SessionParams.WorldType = EPlaySessionWorldType::PlayInEditor;
	SessionParams.DestinationSlateViewport = ActiveLevelViewport;

	// In suppress mode, force the unattended-script global true ONLY around the PIE
	// request. The engine's ShowBlueprintErrorDialog (PlayLevel.cpp) and
	// FSlateApplication::AddModalWindow both early-out on GIsRunningUnattendedScript,
	// so the compile-error prompt resolves to its default instead of blocking the
	// game thread (and with it the in-process MCP server). Self-restoring via RAII;
	// game-thread only, matching the engine's canonical usage. Not blanket-applied to
	// all MCP dispatch — only the one call that can trigger the PIE prompt.
	TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript,
		bSuppressModals ? true : GIsRunningUnattendedScript);

	GUnrealEd->RequestPlaySession(SessionParams);
	GUnrealEd->StartQueuedPlaySessionRequest();
	return true;
}

bool FMonolithEditorActions::StopPieInternal()
{
	if (!GEditor)
	{
		return false;
	}

	const bool bWasRunning = FindActivePieWorld() != nullptr;
	if (bWasRunning)
	{
		GEditor->RequestEndPlayMap();
	}
	return bWasRunning;
}

FMonolithActionResult FMonolithEditorActions::HandleStartPIE(const TSharedPtr<FJsonObject>& Params)
{
	if (!GUnrealEd) return FMonolithActionResult::Error(TEXT("GUnrealEd not available"));

	// Reject if a PIE session is already running so we don't queue duplicates.
	if (FindActivePieWorld())
	{
		TSharedPtr<FJsonObject> AlreadyRunning = MakeShared<FJsonObject>();
		AlreadyRunning->SetBoolField(TEXT("started"), false);
		AlreadyRunning->SetStringField(TEXT("reason"), TEXT("PIE already running"));
		return FMonolithActionResult::Success(AlreadyRunning);
	}

	FString StartError;
	if (!StartPieInternal(StartError))
	{
		return FMonolithActionResult::Error(StartError);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("started"), true);
	Root->SetStringField(TEXT("mode"), TEXT("in_viewport"));
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleStopPIE(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor not available"));

	const bool bWasRunning = StopPieInternal();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("stopped"), bWasRunning);
	return FMonolithActionResult::Success(Root);
}

// --- Capture helpers ---

bool FMonolithEditorActions::RenderAndSaveCapture(
	USceneCaptureComponent2D* CaptureComp,
	UTextureRenderTarget2D* RT,
	int32 ResX, int32 ResY,
	const FString& OutputPath)
{
	if (!CaptureComp || !RT)
	{
		return false;
	}

	// Trigger the capture — submits render commands to the render thread
	CaptureComp->CaptureScene();

	// Use GameThread_GetRenderTargetResource — the non-GameThread variant
	// asserts IsInRenderingThread() which crashes when called from game thread.
	FTextureRenderTargetResource* RTResource = RT->GameThread_GetRenderTargetResource();
	if (!RTResource)
	{
		UE_LOG(LogMonolith, Error, TEXT("CaptureScenePreview: Failed to get RT resource"));
		return false;
	}

	// ReadPixels internally calls FlushRenderingCommands() to synchronize the GPU readback
	TArray<FColor> Pixels;
	bool bReadOk = RTResource->ReadPixels(Pixels);

	if (!bReadOk || Pixels.Num() == 0)
	{
		UE_LOG(LogMonolith, Error, TEXT("CaptureScenePreview: ReadPixels failed (read=%d, count=%d)"),
			bReadOk, Pixels.Num());
		return false;
	}

	// Ensure output directory exists
	FString Dir = FPaths::GetPath(OutputPath);
	IFileManager::Get().MakeDirectory(*Dir, true);

	// Encode as PNG and save
	FImage Image;
	Image.Init(ResX, ResY, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FMemory::Memcpy(Image.RawData.GetData(), Pixels.GetData(), Pixels.Num() * sizeof(FColor));

	return FImageUtils::SaveImageAutoFormat(*OutputPath, Image);
}

bool FMonolithEditorActions::CaptureNiagaraFrame(
	UNiagaraSystem* System,
	float SeekTime,
	const FVector& CameraLocation,
	const FRotator& CameraRotation,
	float FOV,
	int32 ResX, int32 ResY,
	const FString& OutputPath,
	ESceneCaptureSource CaptureSource)
{
	if (!System)
	{
		return false;
	}

	// Create preview scene with black background (no environment lighting)
	// VFX effects (especially fire, emissives) need a dark background to evaluate properly
	FPreviewScene::ConstructionValues CVs;
	CVs.bDefaultLighting = false;
	CVs.LightBrightness = 0.0f;
	CVs.SkyBrightness = 0.0f;
	TSharedPtr<FAdvancedPreviewScene> PreviewScene =
		MakeShareable(new FAdvancedPreviewScene(CVs));
	PreviewScene->SetFloorVisibility(false);
	PreviewScene->SetEnvironmentVisibility(false);

	// Create Niagara component
	UNiagaraComponent* NiagaraComp = NewObject<UNiagaraComponent>(
		GetTransientPackage(), NAME_None, RF_Transient);
	NiagaraComp->CastShadow = false;
	NiagaraComp->bCastDynamicShadow = false;
	NiagaraComp->SetAllowScalability(false);
	NiagaraComp->SetAsset(System);
	NiagaraComp->SetForceSolo(true);
	NiagaraComp->SetAgeUpdateMode(ENiagaraAgeUpdateMode::DesiredAge);
	NiagaraComp->SetCanRenderWhileSeeking(true);
	NiagaraComp->SetMaxSimTime(0.0f);
	NiagaraComp->Activate(true);

	PreviewScene->AddComponent(NiagaraComp, NiagaraComp->GetRelativeTransform());

	// Seek to desired time
	const float SeekDelta = 1.0f / 30.0f;
	UWorld* World = NiagaraComp->GetWorld();

	if (SeekTime > 0.0f)
	{
		NiagaraComp->SetSeekDelta(SeekDelta);
		NiagaraComp->SeekToDesiredAge(SeekTime);

		if (World)
		{
			World->TimeSeconds = SeekTime;
			World->UnpausedTimeSeconds = SeekTime;
			World->RealTimeSeconds = SeekTime;
			World->DeltaRealTimeSeconds = SeekDelta;
			World->DeltaTimeSeconds = SeekDelta;
			World->Tick(ELevelTick::LEVELTICK_PauseTick, 0.0f);
		}

		NiagaraComp->TickComponent(SeekDelta, ELevelTick::LEVELTICK_All, nullptr);

		if (World)
		{
			World->SendAllEndOfFrameUpdates();
			if (FNiagaraWorldManager* WorldManager = FNiagaraWorldManager::Get(World))
			{
				WorldManager->FlushComputeAndDeferredQueues(true);  // Wait for GPU
			}
		}
	}

	// Warm-up ticks: pump the world + component so GPU particle buffers are populated.
	// Runs even at SeekTime==0 — particles need frames to spawn and fill GPU buffers.
	if (World)
	{
		constexpr int32 WarmUpFrames = 3;
		for (int32 i = 0; i < WarmUpFrames; i++)
		{
			World->Tick(ELevelTick::LEVELTICK_PauseTick, SeekDelta);
			NiagaraComp->TickComponent(SeekDelta, ELevelTick::LEVELTICK_All, nullptr);
			World->SendAllEndOfFrameUpdates();
			if (FNiagaraWorldManager* WorldManager = FNiagaraWorldManager::Get(World))
			{
				WorldManager->FlushComputeAndDeferredQueues(true);
			}
			FlushRenderingCommands();
		}
	}

	// Wait for any in-flight shader compilation before capture
	if (GShaderCompilingManager)
	{
		GShaderCompilingManager->FinishAllCompilation();
	}
	FlushRenderingCommands();

	// Create render target
	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	RT->InitAutoFormat(ResX, ResY);
	RT->ClearColor = FLinearColor::Black;
	RT->UpdateResourceImmediate(true);

	// Create scene capture component (same as Baker)
	USceneCaptureComponent2D* CaptureComp = NewObject<USceneCaptureComponent2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	CaptureComp->bTickInEditor = false;
	CaptureComp->SetComponentTickEnabled(false);
	CaptureComp->SetVisibility(true);
	CaptureComp->bCaptureEveryFrame = false;
	CaptureComp->bCaptureOnMovement = false;
	CaptureComp->TextureTarget = RT;
	CaptureComp->CaptureSource = CaptureSource;
	CaptureComp->ProjectionType = ECameraProjectionMode::Perspective;
	CaptureComp->FOVAngle = FOV;
	CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;

	// Register with the preview scene's world (World already declared above)
	CaptureComp->RegisterComponentWithWorld(World);
	CaptureComp->SetWorldLocationAndRotation(CameraLocation, CameraRotation);

	// Capture and save
	bool bSuccess = RenderAndSaveCapture(CaptureComp, RT, ResX, ResY, OutputPath);

	// Cleanup
	CaptureComp->TextureTarget = nullptr;
	CaptureComp->UnregisterComponent();
	PreviewScene->RemoveComponent(NiagaraComp);

	return bSuccess;
}

bool FMonolithEditorActions::CaptureMaterialFrame(
	UMaterialInterface* Material,
	const FString& MeshType,
	const FVector& CameraLocation,
	const FRotator& CameraRotation,
	float FOV,
	int32 ResX, int32 ResY,
	const FString& OutputPath,
	ESceneCaptureSource CaptureSource,
	float UVTiling,
	const FLinearColor& BackgroundColor)
{
	if (!Material)
	{
		return false;
	}

	// Create preview scene
	TSharedPtr<FAdvancedPreviewScene> PreviewScene =
		MakeShareable(new FAdvancedPreviewScene(FPreviewScene::ConstructionValues()));
	PreviewScene->SetFloorVisibility(false);

	UPrimitiveComponent* SpawnedMeshComp = nullptr;

	if (!FMath::IsNearlyEqual(UVTiling, 1.0f) && (MeshType.Equals(TEXT("plane"), ESearchCase::IgnoreCase) || MeshType.IsEmpty()))
	{
		// Build a procedural quad with scaled UVs for tiling preview
		UProceduralMeshComponent* ProcMeshComp = NewObject<UProceduralMeshComponent>(
			GetTransientPackage(), NAME_None, RF_Transient);

		const float HalfSize = 100.0f; // 200x200 cm quad
		TArray<FVector> Vertices;
		Vertices.Add(FVector(-HalfSize, -HalfSize, 0.0f));
		Vertices.Add(FVector( HalfSize, -HalfSize, 0.0f));
		Vertices.Add(FVector( HalfSize,  HalfSize, 0.0f));
		Vertices.Add(FVector(-HalfSize,  HalfSize, 0.0f));

		TArray<int32> Triangles = { 0, 1, 2, 0, 2, 3 };

		TArray<FVector> Normals;
		Normals.Init(FVector::UpVector, 4);

		TArray<FVector2D> UV0;
		UV0.Add(FVector2D(0.0f, 0.0f));
		UV0.Add(FVector2D(UVTiling, 0.0f));
		UV0.Add(FVector2D(UVTiling, UVTiling));
		UV0.Add(FVector2D(0.0f, UVTiling));

		TArray<FColor> VertexColors;
		VertexColors.Init(FColor::White, 4);

		TArray<FProcMeshTangent> Tangents;
		Tangents.Init(FProcMeshTangent(1.0f, 0.0f, 0.0f), 4);

		ProcMeshComp->CreateMeshSection(0, Vertices, Triangles, Normals, UV0, VertexColors, Tangents, false);
		ProcMeshComp->SetMaterial(0, const_cast<UMaterialInterface*>(Material));
		ProcMeshComp->SetRelativeScale3D(FVector(2.0f, 2.0f, 1.0f));
		SpawnedMeshComp = ProcMeshComp;
	}
	else
	{
		// Standard static mesh path
		FString MeshPath;
		if (MeshType.Equals(TEXT("sphere"), ESearchCase::IgnoreCase))
		{
			MeshPath = TEXT("/Engine/BasicShapes/Sphere");
		}
		else if (MeshType.Equals(TEXT("cube"), ESearchCase::IgnoreCase))
		{
			MeshPath = TEXT("/Engine/BasicShapes/Cube");
		}
		else if (MeshType.Equals(TEXT("cylinder"), ESearchCase::IgnoreCase))
		{
			MeshPath = TEXT("/Engine/BasicShapes/Cylinder");
		}
		else // default: plane
		{
			MeshPath = TEXT("/Engine/BasicShapes/Plane");
		}

		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
		if (!Mesh)
		{
			UE_LOG(LogMonolith, Error, TEXT("CaptureMaterialFrame: Failed to load mesh %s"), *MeshPath);
			return false;
		}

		UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(
			GetTransientPackage(), NAME_None, RF_Transient);
		MeshComp->SetStaticMesh(Mesh);
		MeshComp->SetMaterial(0, const_cast<UMaterialInterface*>(Material));
		MeshComp->SetRelativeScale3D(FVector(2.0f, 2.0f, 1.0f));
		SpawnedMeshComp = MeshComp;
	}

	PreviewScene->AddComponent(SpawnedMeshComp, SpawnedMeshComp->GetRelativeTransform());

	// Create render target
	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	RT->InitAutoFormat(ResX, ResY);
	RT->ClearColor = BackgroundColor;
	RT->UpdateResourceImmediate(true);

	// Create scene capture
	USceneCaptureComponent2D* CaptureComp = NewObject<USceneCaptureComponent2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	CaptureComp->bTickInEditor = false;
	CaptureComp->SetComponentTickEnabled(false);
	CaptureComp->SetVisibility(true);
	CaptureComp->bCaptureEveryFrame = false;
	CaptureComp->bCaptureOnMovement = false;
	CaptureComp->TextureTarget = RT;
	CaptureComp->CaptureSource = CaptureSource;
	CaptureComp->ProjectionType = ECameraProjectionMode::Perspective;
	CaptureComp->FOVAngle = FOV;
	CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;

	UWorld* World = PreviewScene->GetWorld();
	CaptureComp->RegisterComponentWithWorld(World);
	CaptureComp->SetWorldLocationAndRotation(CameraLocation, CameraRotation);

	// Capture and save
	bool bSuccess = RenderAndSaveCapture(CaptureComp, RT, ResX, ResY, OutputPath);

	// Cleanup
	CaptureComp->TextureTarget = nullptr;
	CaptureComp->UnregisterComponent();
	PreviewScene->RemoveComponent(SpawnedMeshComp);

	return bSuccess;
}

// --- Capture action handlers ---

FMonolithActionResult FMonolithEditorActions::HandleCaptureScenePreview(
	const TSharedPtr<FJsonObject>& Params)
{
	// Parse required params
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString AssetType = Params->GetStringField(TEXT("asset_type"));

	if (AssetPath.IsEmpty() || AssetType.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path and asset_type are required"));
	}

	// Parse optional params
	float SeekTime = 0.0f;
	if (Params->HasField(TEXT("seek_time")))
	{
		SeekTime = (float)Params->GetNumberField(TEXT("seek_time"));
	}

	FString PreviewMesh = TEXT("plane");
	if (Params->HasField(TEXT("preview_mesh")))
	{
		PreviewMesh = Params->GetStringField(TEXT("preview_mesh"));
	}

	int32 ResX = 512, ResY = 512;
	if (Params->HasField(TEXT("resolution")))
	{
		const TArray<TSharedPtr<FJsonValue>>& ResArray = Params->GetArrayField(TEXT("resolution"));
		if (ResArray.Num() >= 2)
		{
			ResX = (int32)ResArray[0]->AsNumber();
			ResY = (int32)ResArray[1]->AsNumber();
		}
	}

	// Parse camera
	FVector CameraLocation(200.0f, 0.0f, 100.0f);
	FRotator CameraRotation(0.0f, 180.0f, 0.0f);
	float FOV = 60.0f;

	if (Params->HasField(TEXT("camera")))
	{
		const TSharedPtr<FJsonObject>* CameraObj = nullptr;
		TSharedPtr<FJsonObject> ParsedCamera;

		// Handle both object and string-serialized (Claude Code quirk)
		if (!Params->TryGetObjectField(TEXT("camera"), CameraObj))
		{
			FString CameraStr = Params->GetStringField(TEXT("camera"));
			if (!CameraStr.IsEmpty())
			{
				ParsedCamera = FMonolithJsonUtils::Parse(CameraStr);
				CameraObj = &ParsedCamera;
			}
		}

		if (CameraObj && (*CameraObj).IsValid())
		{
			if ((*CameraObj)->HasField(TEXT("location")))
			{
				const TArray<TSharedPtr<FJsonValue>>& Loc = (*CameraObj)->GetArrayField(TEXT("location"));
				if (Loc.Num() >= 3)
				{
					CameraLocation = FVector(Loc[0]->AsNumber(), Loc[1]->AsNumber(), Loc[2]->AsNumber());
				}
			}
			if ((*CameraObj)->HasField(TEXT("rotation")))
			{
				const TArray<TSharedPtr<FJsonValue>>& Rot = (*CameraObj)->GetArrayField(TEXT("rotation"));
				if (Rot.Num() >= 3)
				{
					CameraRotation = FRotator(Rot[0]->AsNumber(), Rot[1]->AsNumber(), Rot[2]->AsNumber());
				}
			}
			if ((*CameraObj)->HasField(TEXT("fov")))
			{
				FOV = (float)(*CameraObj)->GetNumberField(TEXT("fov"));
			}
		}
	}

	// Generate output path
	FString OutputPath;
	if (Params->HasField(TEXT("output_path")))
	{
		OutputPath = Params->GetStringField(TEXT("output_path"));
		if (FPaths::IsRelative(OutputPath))
		{
			OutputPath = FPaths::ProjectDir() / OutputPath;
		}
	}
	else
	{
		FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		FString SafeName = FPaths::GetBaseFilename(AssetPath);
		OutputPath = FPaths::ProjectDir() / TEXT("Saved/Screenshots/Monolith") /
			FString::Printf(TEXT("%s_%s.png"), *Timestamp, *SafeName);
	}

	// UE's FHttpServerModule dispatches handlers on the game thread via FTicker,
	// so we're already on the game thread here. Call capture functions directly.
	check(IsInGameThread());

	double StartTime = FPlatformTime::Seconds();
	bool bSuccess = false;

	if (AssetType.Equals(TEXT("niagara"), ESearchCase::IgnoreCase))
	{
		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *AssetPath);
		if (!System)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Failed to load Niagara system: %s"), *AssetPath));
		}
		bSuccess = CaptureNiagaraFrame(System, SeekTime, CameraLocation, CameraRotation,
			FOV, ResX, ResY, OutputPath);
	}
	else if (AssetType.Equals(TEXT("material"), ESearchCase::IgnoreCase))
	{
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *AssetPath);
		if (!Material)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Failed to load material: %s"), *AssetPath));
		}
		float UVTiling = 1.0f;
		if (Params->HasField(TEXT("uv_tiling")))
		{
			UVTiling = (float)Params->GetNumberField(TEXT("uv_tiling"));
			if (UVTiling <= 0.0f) UVTiling = 1.0f;
		}

		FLinearColor BgColor(0.18f, 0.18f, 0.18f);
		if (Params->HasField(TEXT("background_color")))
		{
			const TArray<TSharedPtr<FJsonValue>>& BgArr = Params->GetArrayField(TEXT("background_color"));
			if (BgArr.Num() >= 3)
			{
				BgColor = FLinearColor(
					(float)BgArr[0]->AsNumber(),
					(float)BgArr[1]->AsNumber(),
					(float)BgArr[2]->AsNumber(),
					BgArr.Num() >= 4 ? (float)BgArr[3]->AsNumber() : 1.0f);
			}
		}

		bSuccess = CaptureMaterialFrame(Material, PreviewMesh, CameraLocation, CameraRotation,
			FOV, ResX, ResY, OutputPath, ESceneCaptureSource::SCS_FinalToneCurveHDR,
			UVTiling, BgColor);
	}
	else if (AssetType.Equals(TEXT("static_mesh"), ESearchCase::IgnoreCase))
	{
		// Load asset.
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
		if (!Mesh)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Failed to load static mesh: %s"), *AssetPath));
		}

		// Allocate transient preview scene (matches CaptureMaterialFrame pattern).
		TSharedPtr<FAdvancedPreviewScene> PreviewScene =
			MakeShareable(new FAdvancedPreviewScene(FPreviewScene::ConstructionValues()));
		PreviewScene->SetFloorVisibility(false);

		UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(
			GetTransientPackage(), NAME_None, RF_Transient);
		MeshComp->SetStaticMesh(Mesh);
		PreviewScene->AddComponent(MeshComp, MeshComp->GetRelativeTransform());

		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
			GetTransientPackage(), NAME_None, RF_Transient);
		RT->InitAutoFormat(ResX, ResY);
		RT->ClearColor = FLinearColor(0.18f, 0.18f, 0.18f);
		RT->UpdateResourceImmediate(true);

		USceneCaptureComponent2D* CaptureComp = NewObject<USceneCaptureComponent2D>(
			GetTransientPackage(), NAME_None, RF_Transient);
		CaptureComp->bTickInEditor = false;
		CaptureComp->SetComponentTickEnabled(false);
		CaptureComp->SetVisibility(true);
		CaptureComp->bCaptureEveryFrame = false;
		CaptureComp->bCaptureOnMovement = false;
		CaptureComp->TextureTarget = RT;
		CaptureComp->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;
		CaptureComp->ProjectionType = ECameraProjectionMode::Perspective;
		CaptureComp->FOVAngle = FOV;
		CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;

		UWorld* World = PreviewScene->GetWorld();
		CaptureComp->RegisterComponentWithWorld(World);
		CaptureComp->SetWorldLocationAndRotation(CameraLocation, CameraRotation);

		bSuccess = RenderAndSaveCapture(CaptureComp, RT, ResX, ResY, OutputPath);

		// Cleanup.
		CaptureComp->TextureTarget = nullptr;
		CaptureComp->UnregisterComponent();
		PreviewScene->RemoveComponent(MeshComp);
	}
	else if (AssetType.Equals(TEXT("skeletal_mesh"), ESearchCase::IgnoreCase))
	{
		USkeletalMesh* SkelMesh = LoadObject<USkeletalMesh>(nullptr, *AssetPath);
		if (!SkelMesh)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Failed to load skeletal mesh: %s"), *AssetPath));
		}

		// Optional animation_path — when present, pose at seek_time. seek_time is
		// already parsed at the top of this function (default 0.0).
		UAnimSequence* AnimSeq = nullptr;
		if (Params->HasField(TEXT("animation_path")))
		{
			FString AnimPath = Params->GetStringField(TEXT("animation_path"));
			if (!AnimPath.IsEmpty())
			{
				AnimSeq = LoadObject<UAnimSequence>(nullptr, *AnimPath);
				if (!AnimSeq)
				{
					return FMonolithActionResult::Error(
						FString::Printf(TEXT("Failed to load animation sequence: %s"), *AnimPath));
				}
			}
		}

		TSharedPtr<FAdvancedPreviewScene> PreviewScene =
			MakeShareable(new FAdvancedPreviewScene(FPreviewScene::ConstructionValues()));
		PreviewScene->SetFloorVisibility(false);

		USkeletalMeshComponent* SkelMeshComp = NewObject<USkeletalMeshComponent>(
			GetTransientPackage(), NAME_None, RF_Transient);
		SkelMeshComp->SetSkeletalMesh(SkelMesh);

		if (AnimSeq)
		{
			// Pair-and-evaluate posing per UE 5.7 contract: PlayAnimation puts the
			// component into single-node-instance mode + assigns the asset, then
			// SetPosition forces evaluation at the target time without ticking.
			SkelMeshComp->PlayAnimation(AnimSeq, /*bLooping=*/false);
			SkelMeshComp->SetPosition(SeekTime, /*bFireNotifies=*/false);
		}

		PreviewScene->AddComponent(SkelMeshComp, SkelMeshComp->GetRelativeTransform());

		// Tick the world once so the pose evaluation lands before capture.
		UWorld* World = PreviewScene->GetWorld();
		if (World)
		{
			World->Tick(ELevelTick::LEVELTICK_PauseTick, 0.0f);
			SkelMeshComp->TickComponent(0.0f, ELevelTick::LEVELTICK_All, nullptr);
			World->SendAllEndOfFrameUpdates();
		}

		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
			GetTransientPackage(), NAME_None, RF_Transient);
		RT->InitAutoFormat(ResX, ResY);
		RT->ClearColor = FLinearColor(0.18f, 0.18f, 0.18f);
		RT->UpdateResourceImmediate(true);

		USceneCaptureComponent2D* CaptureComp = NewObject<USceneCaptureComponent2D>(
			GetTransientPackage(), NAME_None, RF_Transient);
		CaptureComp->bTickInEditor = false;
		CaptureComp->SetComponentTickEnabled(false);
		CaptureComp->SetVisibility(true);
		CaptureComp->bCaptureEveryFrame = false;
		CaptureComp->bCaptureOnMovement = false;
		CaptureComp->TextureTarget = RT;
		CaptureComp->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;
		CaptureComp->ProjectionType = ECameraProjectionMode::Perspective;
		CaptureComp->FOVAngle = FOV;
		CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;

		CaptureComp->RegisterComponentWithWorld(World);
		CaptureComp->SetWorldLocationAndRotation(CameraLocation, CameraRotation);

		bSuccess = RenderAndSaveCapture(CaptureComp, RT, ResX, ResY, OutputPath);

		// Cleanup.
		CaptureComp->TextureTarget = nullptr;
		CaptureComp->UnregisterComponent();
		PreviewScene->RemoveComponent(SkelMeshComp);
	}
	else if (AssetType.Equals(TEXT("widget"), ESearchCase::IgnoreCase))
	{
		// Headless / nullrhi / commandlet contexts have no rendering path — bail
		// with the same -32603 convention claudedesign::capture_widget uses so
		// agents can pattern-match the error code.
		if (!FApp::CanEverRender())
		{
			return FMonolithActionResult::Error(
				TEXT("Cannot render widget: this app has no rendering path (server / nullrhi / commandlet)"),
				-32603);
		}

		// Load Widget Blueprint. UMG widget assets live in UWidgetBlueprint with
		// the runtime UClass on GeneratedClass.
		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
		if (!WBP)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Failed to load Widget Blueprint: %s"), *AssetPath),
				-32602);
		}
		if (!WBP->GeneratedClass)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Widget Blueprint '%s' has no GeneratedClass (needs compile?)"), *AssetPath),
				-32603);
		}

		UClass* GenClass = WBP->GeneratedClass.Get();
		if (!GenClass || !GenClass->IsChildOf(UUserWidget::StaticClass()))
		{
			return FMonolithActionResult::Error(
				TEXT("Widget Blueprint GeneratedClass is not a UUserWidget subclass"),
				-32603);
		}

		UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!EditorWorld)
		{
			return FMonolithActionResult::Error(
				TEXT("No editor world available to create widget in"), -32603);
		}

		UUserWidget* Instance = CreateWidget<UUserWidget>(EditorWorld, GenClass);
		if (!Instance)
		{
			return FMonolithActionResult::Error(
				TEXT("CreateWidget<UUserWidget> returned null"), -32603);
		}

		// Resolved-question #2: ship optional `scale` param defaulting to 1.0, clamp >= 0.01.
		double ScaleD = 1.0;
		Params->TryGetNumberField(TEXT("scale"), ScaleD);
		const float Scale = FMath::Max(0.01f, (float)ScaleD);

		const uint32 PhysicalW = FMath::Max(1u, (uint32)(ResX * Scale));
		const uint32 PhysicalH = FMath::Max(1u, (uint32)(ResY * Scale));

		const bool bUseGammaCorrection = true;
		const bool bIsLinearSpace = !bUseGammaCorrection;
		const EPixelFormat RequestedFormat = FSlateApplication::Get().GetRenderer()->GetSlateRecommendedColorFormat();

		UTextureRenderTarget2D* WidgetRT = NewObject<UTextureRenderTarget2D>();
		WidgetRT->ClearColor = FLinearColor::Transparent;
		WidgetRT->Filter = TF_Bilinear;
		WidgetRT->SRGB = bIsLinearSpace;
		WidgetRT->TargetGamma = 1;
		WidgetRT->InitCustomFormat(PhysicalW, PhysicalH, RequestedFormat, bIsLinearSpace);
		WidgetRT->UpdateResourceImmediate(/*bClearRenderTarget=*/true);

		// FWidgetRenderer derives from FDeferredCleanupInterface — must be deleted
		// via BeginCleanup, not raw `delete`. Mirrors sibling claudedesign pattern.
		FWidgetRenderer* WRenderer = new FWidgetRenderer(bUseGammaCorrection, /*bInClearTarget=*/true);
		WRenderer->SetIsPrepassNeeded(true);

		const TSharedRef<SWidget> SlateWidget = Instance->TakeWidget();

		// First draw triggers material handle creation + shader compilation. Without
		// the warmup + FinishAllCompilation, material-backed widget batches are
		// silently skipped (SlateRHIRenderingPolicy.cpp:1109).
		WRenderer->DrawWidget(WidgetRT, SlateWidget, Scale, FVector2D(ResX, ResY), 0.0f);
		FlushRenderingCommands();
		if (GShaderCompilingManager)
		{
			GShaderCompilingManager->FinishAllCompilation();
		}

		WRenderer->DrawWidget(WidgetRT, SlateWidget, Scale, FVector2D(ResX, ResY), 0.0f);
		FlushRenderingCommands();

		// Export — match the sibling pattern. ExportRenderTarget2DAsPNG is soft-
		// deprecated but functional in 5.7; mirror existing-pattern choice.
		const FString OutDir = FPaths::GetPath(OutputPath);
		if (!OutDir.IsEmpty())
		{
			IFileManager::Get().MakeDirectory(*OutDir, /*Tree=*/true);
		}

		bool bExportOk = false;
		{
			TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*OutputPath));
			if (Writer)
			{
				bExportOk = FImageUtils::ExportRenderTarget2DAsPNG(WidgetRT, *Writer);
				Writer->Close();
			}
		}

		BeginCleanup(WRenderer);

		bSuccess = bExportOk;

		// Physical (scale-adjusted) resolution dominates for the widget branch;
		// stash it back in ResX/ResY so the success payload below reflects what
		// was actually written.
		ResX = (int32)PhysicalW;
		ResY = (int32)PhysicalH;
	}
	else
	{
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("Unsupported asset_type: %s (supported: niagara, material, static_mesh, skeletal_mesh, widget)"),
				*AssetType));
	}

	double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	if (!bSuccess)
	{
		return FMonolithActionResult::Error(TEXT("Capture failed — check log for details"));
	}

	// Return result
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("output_file"), OutputPath);
	TSharedPtr<FJsonObject> ResObj = MakeShared<FJsonObject>();
	ResObj->SetNumberField(TEXT("width"), ResX);
	ResObj->SetNumberField(TEXT("height"), ResY);
	Result->SetObjectField(TEXT("resolution"), ResObj);
	Result->SetNumberField(TEXT("seek_time"), SeekTime);
	Result->SetNumberField(TEXT("capture_time_ms"), ElapsedMs);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleCaptureSequenceFrames(
	const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString AssetType = Params->GetStringField(TEXT("asset_type"));

	if (AssetPath.IsEmpty() || AssetType.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path and asset_type are required"));
	}

	if (!Params->HasField(TEXT("timestamps")))
	{
		return FMonolithActionResult::Error(TEXT("timestamps array is required"));
	}

	// Parse timestamps
	TArray<float> Timestamps;
	const TArray<TSharedPtr<FJsonValue>>& TimestampArray = Params->GetArrayField(TEXT("timestamps"));
	for (const auto& Val : TimestampArray)
	{
		Timestamps.Add((float)Val->AsNumber());
	}
	Timestamps.Sort();

	if (Timestamps.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("timestamps array is empty"));
	}

	// Parse optional params (same as capture_scene_preview)
	int32 ResX = 512, ResY = 512;
	if (Params->HasField(TEXT("resolution")))
	{
		const TArray<TSharedPtr<FJsonValue>>& ResArray = Params->GetArrayField(TEXT("resolution"));
		if (ResArray.Num() >= 2)
		{
			ResX = (int32)ResArray[0]->AsNumber();
			ResY = (int32)ResArray[1]->AsNumber();
		}
	}

	FVector CameraLocation(200.0f, 0.0f, 100.0f);
	FRotator CameraRotation(0.0f, 180.0f, 0.0f);
	float FOV = 60.0f;
	// Same camera parsing as HandleCaptureScenePreview (with string fallback)
	if (Params->HasField(TEXT("camera")))
	{
		const TSharedPtr<FJsonObject>* CameraObj = nullptr;
		TSharedPtr<FJsonObject> ParsedCamera;
		if (!Params->TryGetObjectField(TEXT("camera"), CameraObj))
		{
			FString CameraStr = Params->GetStringField(TEXT("camera"));
			if (!CameraStr.IsEmpty())
			{
				ParsedCamera = FMonolithJsonUtils::Parse(CameraStr);
				CameraObj = &ParsedCamera;
			}
		}
		if (CameraObj && (*CameraObj).IsValid())
		{
			if ((*CameraObj)->HasField(TEXT("location")))
			{
				const TArray<TSharedPtr<FJsonValue>>& Loc = (*CameraObj)->GetArrayField(TEXT("location"));
				if (Loc.Num() >= 3) CameraLocation = FVector(Loc[0]->AsNumber(), Loc[1]->AsNumber(), Loc[2]->AsNumber());
			}
			if ((*CameraObj)->HasField(TEXT("rotation")))
			{
				const TArray<TSharedPtr<FJsonValue>>& Rot = (*CameraObj)->GetArrayField(TEXT("rotation"));
				if (Rot.Num() >= 3) CameraRotation = FRotator(Rot[0]->AsNumber(), Rot[1]->AsNumber(), Rot[2]->AsNumber());
			}
			if ((*CameraObj)->HasField(TEXT("fov")))
			{
				FOV = (float)(*CameraObj)->GetNumberField(TEXT("fov"));
			}
		}
	}

	FString OutputDir;
	if (Params->HasField(TEXT("output_dir")))
	{
		OutputDir = Params->GetStringField(TEXT("output_dir"));
		if (FPaths::IsRelative(OutputDir))
		{
			OutputDir = FPaths::ProjectDir() / OutputDir;
		}
	}
	else
	{
		FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		FString SafeName = FPaths::GetBaseFilename(AssetPath);
		OutputDir = FPaths::ProjectDir() / TEXT("Saved/Screenshots/Monolith") /
			FString::Printf(TEXT("%s_%s"), *Timestamp, *SafeName);
	}

	FString FilenamePrefix = TEXT("frame");
	if (Params->HasField(TEXT("filename_prefix")))
	{
		FilenamePrefix = Params->GetStringField(TEXT("filename_prefix"));
	}

	// Currently only supports Niagara for multi-frame
	if (!AssetType.Equals(TEXT("niagara"), ESearchCase::IgnoreCase))
	{
		return FMonolithActionResult::Error(TEXT("capture_sequence_frames currently only supports asset_type: niagara"));
	}

	// Already on game thread (UE HTTP server dispatches via FTicker)
	check(IsInGameThread());

	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *AssetPath);
	if (!System)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to load: %s"), *AssetPath));
	}

	bool bPersistent = Params->HasField(TEXT("persistent")) && Params->GetBoolField(TEXT("persistent"));

	double StartTime = FPlatformTime::Seconds();
	TArray<TSharedPtr<FJsonValue>> FrameResults;

	if (bPersistent)
	{
		// PERSISTENT MODE: Create component ONCE, advance through time, capture at intervals.
		// Preserves ribbons, particle accumulation, and inter-frame state.
		FPreviewScene::ConstructionValues CVs;
		CVs.bDefaultLighting = false;
		CVs.LightBrightness = 0.0f;
		CVs.SkyBrightness = 0.0f;
		TSharedPtr<FAdvancedPreviewScene> PreviewScene =
			MakeShareable(new FAdvancedPreviewScene(CVs));
		PreviewScene->SetFloorVisibility(false);
		PreviewScene->SetEnvironmentVisibility(false);

		UNiagaraComponent* NiagaraComp = NewObject<UNiagaraComponent>(
			GetTransientPackage(), NAME_None, RF_Transient);
		NiagaraComp->CastShadow = false;
		NiagaraComp->bCastDynamicShadow = false;
		NiagaraComp->SetAllowScalability(false);
		NiagaraComp->SetAsset(System);
		NiagaraComp->SetForceSolo(true);
		NiagaraComp->SetAgeUpdateMode(ENiagaraAgeUpdateMode::DesiredAge);
		NiagaraComp->SetCanRenderWhileSeeking(true);
		NiagaraComp->SetMaxSimTime(0.0f);
		NiagaraComp->Activate(true);

		PreviewScene->AddComponent(NiagaraComp, NiagaraComp->GetRelativeTransform());

		UWorld* World = NiagaraComp->GetWorld();
		const float TickDelta = 1.0f / 30.0f;
		float CurrentTime = 0.0f;

		// Sort timestamps to ensure we advance monotonically
		TArray<float> SortedTimestamps = Timestamps;
		SortedTimestamps.Sort();

		for (int32 i = 0; i < SortedTimestamps.Num(); i++)
		{
			float TargetTime = SortedTimestamps[i];

			// Advance from current time to target time
			NiagaraComp->SetSeekDelta(TickDelta);
			NiagaraComp->SeekToDesiredAge(TargetTime);

			if (World)
			{
				World->TimeSeconds = TargetTime;
				World->DeltaTimeSeconds = TickDelta;
				World->Tick(ELevelTick::LEVELTICK_PauseTick, TickDelta);
			}

			NiagaraComp->TickComponent(TickDelta, ELevelTick::LEVELTICK_All, nullptr);

			// GPU flush for particle buffers
			if (World)
			{
				World->SendAllEndOfFrameUpdates();
				if (FNiagaraWorldManager* WorldManager = FNiagaraWorldManager::Get(World))
				{
					WorldManager->FlushComputeAndDeferredQueues(true);
				}
			}
			FlushRenderingCommands();

			// Warm-up extra tick so GPU buffers are populated
			if (World)
			{
				World->Tick(ELevelTick::LEVELTICK_PauseTick, TickDelta);
				NiagaraComp->TickComponent(TickDelta, ELevelTick::LEVELTICK_All, nullptr);
				World->SendAllEndOfFrameUpdates();
				if (FNiagaraWorldManager* WorldManager = FNiagaraWorldManager::Get(World))
				{
					WorldManager->FlushComputeAndDeferredQueues(true);
				}
				FlushRenderingCommands();
			}

			// Set up capture component and render
			UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
			RT->InitAutoFormat(ResX, ResY);
			RT->ClearColor = FLinearColor::Black;
			RT->UpdateResourceImmediate(true);

			USceneCaptureComponent2D* CaptureComp = NewObject<USceneCaptureComponent2D>(
				GetTransientPackage(), NAME_None, RF_Transient);
			CaptureComp->TextureTarget = RT;
			CaptureComp->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;
			CaptureComp->bCaptureEveryFrame = false;
			CaptureComp->bCaptureOnMovement = false;
			CaptureComp->bAlwaysPersistRenderingState = true;
			CaptureComp->FOVAngle = FOV;
			CaptureComp->SetRelativeLocation(CameraLocation);
			CaptureComp->SetRelativeRotation(CameraRotation);

			PreviewScene->AddComponent(CaptureComp, FTransform::Identity);

			FString FramePath = OutputDir / FString::Printf(TEXT("%s_%03d_t%.2f.png"),
				*FilenamePrefix, i, TargetTime);

			bool bOk = RenderAndSaveCapture(CaptureComp, RT, ResX, ResY, FramePath);

			PreviewScene->RemoveComponent(CaptureComp);

			TSharedPtr<FJsonObject> FrameObj = MakeShared<FJsonObject>();
			FrameObj->SetNumberField(TEXT("timestamp"), TargetTime);
			FrameObj->SetStringField(TEXT("file"), FramePath);
			FrameObj->SetBoolField(TEXT("success"), bOk);
			FrameResults.Add(MakeShared<FJsonValueObject>(FrameObj));

			CurrentTime = TargetTime;
		}

		// Cleanup
		PreviewScene->RemoveComponent(NiagaraComp);
		NiagaraComp->DeactivateImmediate();
	}
	else
	{
		// PER-FRAME MODE: Use CaptureNiagaraFrame per frame — the proven working path
		// (DesiredAge + warm-up ticks + GPU flush). Reliable but recreates component each frame.
		for (int32 i = 0; i < Timestamps.Num(); i++)
		{
			float T = Timestamps[i];
			FString FramePath = OutputDir / FString::Printf(TEXT("%s_%03d_t%.2f.png"),
				*FilenamePrefix, i, T);

			bool bOk = CaptureNiagaraFrame(System, T, CameraLocation, CameraRotation,
				FOV, ResX, ResY, FramePath);

			TSharedPtr<FJsonObject> FrameObj = MakeShared<FJsonObject>();
			FrameObj->SetNumberField(TEXT("timestamp"), T);
			FrameObj->SetStringField(TEXT("file"), FramePath);
			FrameObj->SetBoolField(TEXT("success"), bOk);
			FrameResults.Add(MakeShared<FJsonValueObject>(FrameObj));
		}
	}

	double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("persistent_mode"), bPersistent);
	Result->SetArrayField(TEXT("frames"), FrameResults);
	Result->SetNumberField(TEXT("total_capture_time_ms"), ElapsedMs);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleImportTexture(
	const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath = Params->GetStringField(TEXT("source_path"));
	FString Destination = Params->GetStringField(TEXT("destination"));

	if (SourcePath.IsEmpty() || Destination.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("source_path and destination are required"));
	}

	// Verify source file exists
	if (!FPaths::FileExists(SourcePath))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Source file not found: %s"), *SourcePath));
	}

	// Import using AssetTools
	UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
	ImportData->Filenames.Add(SourcePath);
	ImportData->DestinationPath = FPackageName::GetLongPackagePath(Destination);
	ImportData->bReplaceExisting = true;

	FAssetToolsModule& AssetToolsModule =
		FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	TArray<UObject*> ImportedAssets = AssetToolsModule.Get().ImportAssetsAutomated(ImportData);

	if (ImportedAssets.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Import failed — no assets imported"));
	}

	UTexture2D* Texture = Cast<UTexture2D>(ImportedAssets[0]);
	if (!Texture)
	{
		return FMonolithActionResult::Error(TEXT("Imported asset is not a Texture2D"));
	}

	// Apply optional settings
	if (Params->HasField(TEXT("settings")))
	{
		const TSharedPtr<FJsonObject>* SettingsObj;
		// Handle string-serialized params (Claude Code quirk)
		TSharedPtr<FJsonObject> ParsedSettings;
		if (Params->TryGetObjectField(TEXT("settings"), SettingsObj))
		{
			ParsedSettings = *SettingsObj;
		}
		else
		{
			FString SettingsStr = Params->GetStringField(TEXT("settings"));
			if (!SettingsStr.IsEmpty())
			{
				ParsedSettings = FMonolithJsonUtils::Parse(SettingsStr);
			}
		}

		if (ParsedSettings.IsValid())
		{
			// Compression
			if (ParsedSettings->HasField(TEXT("compression")))
			{
				FString Comp = ParsedSettings->GetStringField(TEXT("compression"));
				if (Comp == TEXT("TC_Normalmap")) Texture->CompressionSettings = TC_Normalmap;
				else if (Comp == TEXT("TC_Masks")) Texture->CompressionSettings = TC_Masks;
				else if (Comp == TEXT("TC_HDR")) Texture->CompressionSettings = TC_HDR;
				else if (Comp == TEXT("TC_VectorDisplacementmap")) Texture->CompressionSettings = TC_VectorDisplacementmap;
				else Texture->CompressionSettings = TC_Default;
			}

			// sRGB
			if (ParsedSettings->HasField(TEXT("srgb")))
			{
				Texture->SRGB = ParsedSettings->GetBoolField(TEXT("srgb"));
			}

			// Tiling
			if (ParsedSettings->HasField(TEXT("tiling")))
			{
				if (ParsedSettings->GetBoolField(TEXT("tiling")))
				{
					Texture->AddressX = TA_Wrap;
					Texture->AddressY = TA_Wrap;
				}
			}

			// Max size
			if (ParsedSettings->HasField(TEXT("max_size")))
			{
				int32 MaxSize = (int32)ParsedSettings->GetNumberField(TEXT("max_size"));
				if (MaxSize > 0)
				{
					Texture->MaxTextureSize = MaxSize;
				}
			}

			// LOD group
			if (ParsedSettings->HasField(TEXT("lod_group")))
			{
				FString LODGroup = ParsedSettings->GetStringField(TEXT("lod_group"));
				if (LODGroup == TEXT("TEXTUREGROUP_WorldNormalMap")) Texture->LODGroup = TEXTUREGROUP_WorldNormalMap;
				else if (LODGroup == TEXT("TEXTUREGROUP_Effects")) Texture->LODGroup = TEXTUREGROUP_Effects;
				else if (LODGroup == TEXT("TEXTUREGROUP_EffectsNotFiltered")) Texture->LODGroup = TEXTUREGROUP_EffectsNotFiltered;
				// Default: TEXTUREGROUP_World (already default)
			}
		}
	}

	Texture->UpdateResource();
	Texture->PostEditChange();
	Texture->MarkPackageDirty();

	// Save the package
	UPackage* Package = Texture->GetOutermost();
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	UPackage::SavePackage(Package, Texture, *PackageFilename, SaveArgs);

	// Return result
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_path"), Destination);
	Result->SetNumberField(TEXT("size_x"), Texture->GetSizeX());
	Result->SetNumberField(TEXT("size_y"), Texture->GetSizeY());
	Result->SetStringField(TEXT("format"), GPixelFormats[Texture->GetPixelFormat()].Name);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleStitchFlipbook(
	const TSharedPtr<FJsonObject>& Params)
{
	// --- Extract required params ---
	FString DestPath = Params->GetStringField(TEXT("dest_path"));
	if (DestPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("dest_path is required"));
	}

	// Parse frame_paths array
	const TArray<TSharedPtr<FJsonValue>>* FramePathsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("frame_paths"), FramePathsArray) || !FramePathsArray || FramePathsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("frame_paths array is required and must not be empty"));
	}

	TArray<FString> FramePaths;
	for (const auto& Val : *FramePathsArray)
	{
		FString Path;
		if (Val->TryGetString(Path) && !Path.IsEmpty())
		{
			FramePaths.Add(Path);
		}
	}

	if (FramePaths.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No valid file paths in frame_paths"));
	}

	// Parse grid [cols, rows]
	const TArray<TSharedPtr<FJsonValue>>* GridArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("grid"), GridArray) || !GridArray || GridArray->Num() != 2)
	{
		return FMonolithActionResult::Error(TEXT("grid must be an array of [columns, rows]"));
	}
	int32 GridCols = static_cast<int32>((*GridArray)[0]->AsNumber());
	int32 GridRows = static_cast<int32>((*GridArray)[1]->AsNumber());

	if (GridCols <= 0 || GridRows <= 0)
	{
		return FMonolithActionResult::Error(TEXT("grid columns and rows must be positive"));
	}

	int32 ExpectedFrames = GridCols * GridRows;
	if (FramePaths.Num() != ExpectedFrames)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("frame_paths has %d entries but grid %dx%d expects %d"),
			FramePaths.Num(), GridCols, GridRows, ExpectedFrames));
	}

	// Optional params
	bool bSRGB = !Params->HasField(TEXT("srgb")) || Params->GetBoolField(TEXT("srgb"));
	bool bNoMipmaps = !Params->HasField(TEXT("no_mipmaps")) || Params->GetBoolField(TEXT("no_mipmaps"));
	bool bDeleteSources = !Params->HasField(TEXT("delete_sources")) || Params->GetBoolField(TEXT("delete_sources"));

	FString LODGroupStr = TEXT("TEXTUREGROUP_Effects");
	if (Params->HasField(TEXT("lod_group")))
	{
		LODGroupStr = Params->GetStringField(TEXT("lod_group"));
	}

	// --- Load all frame images ---
	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

	int32 FrameWidth = 0;
	int32 FrameHeight = 0;
	TArray<TArray<FColor>> FramePixels;
	FramePixels.SetNum(FramePaths.Num());

	for (int32 i = 0; i < FramePaths.Num(); i++)
	{
		const FString& FilePath = FramePaths[i];

		if (!FPaths::FileExists(FilePath))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Frame file not found: %s"), *FilePath));
		}

		TArray<uint8> FileData;
		if (!FFileHelper::LoadFileToArray(FileData, *FilePath))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to read frame file: %s"), *FilePath));
		}

		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!ImageWrapper.IsValid() || !ImageWrapper->SetCompressed(FileData.GetData(), FileData.Num()))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to decode PNG: %s"), *FilePath));
		}

		int32 W = ImageWrapper->GetWidth();
		int32 H = ImageWrapper->GetHeight();

		// Validate all frames are same size
		if (i == 0)
		{
			FrameWidth = W;
			FrameHeight = H;
		}
		else if (W != FrameWidth || H != FrameHeight)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Frame %d (%s) is %dx%d but frame 0 is %dx%d — all frames must be the same size"),
				i, *FilePath, W, H, FrameWidth, FrameHeight));
		}

		TArray<uint8> RawData;
		if (!ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, RawData))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to decompress frame %d: %s"), i, *FilePath));
		}

		// Convert raw bytes to FColor array
		FramePixels[i].SetNum(W * H);
		FMemory::Memcpy(FramePixels[i].GetData(), RawData.GetData(), W * H * sizeof(FColor));
	}

	// --- Compose atlas ---
	int32 AtlasWidth = FrameWidth * GridCols;
	int32 AtlasHeight = FrameHeight * GridRows;
	TArray<FColor> AtlasPixels;
	AtlasPixels.SetNumZeroed(AtlasWidth * AtlasHeight);

	for (int32 FrameIdx = 0; FrameIdx < FramePaths.Num(); FrameIdx++)
	{
		int32 Col = FrameIdx % GridCols;
		int32 Row = FrameIdx / GridCols;
		int32 OffsetX = Col * FrameWidth;
		int32 OffsetY = Row * FrameHeight;

		const TArray<FColor>& Src = FramePixels[FrameIdx];
		for (int32 Y = 0; Y < FrameHeight; Y++)
		{
			for (int32 X = 0; X < FrameWidth; X++)
			{
				int32 SrcIdx = Y * FrameWidth + X;
				int32 DstIdx = (OffsetY + Y) * AtlasWidth + (OffsetX + X);
				AtlasPixels[DstIdx] = Src[SrcIdx];
			}
		}
	}

	// --- Create UTexture2D ---
	FString PackagePath = FPackageName::GetLongPackagePath(DestPath);
	FString AssetName = FPackageName::GetLongPackageAssetName(DestPath);

	// Ensure unique package name
	FString PackageName = PackagePath / AssetName;
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to create package: %s"), *PackageName));
	}
	Package->FullyLoad();

	UTexture2D* Texture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!Texture)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create UTexture2D"));
	}

	// Configure platform data
	FTexturePlatformData* PlatformData = new FTexturePlatformData();
	PlatformData->SizeX = AtlasWidth;
	PlatformData->SizeY = AtlasHeight;
	PlatformData->PixelFormat = PF_B8G8R8A8;
	PlatformData->SetNumSlices(1);

	FTexture2DMipMap* Mip = new FTexture2DMipMap();
	Mip->SizeX = AtlasWidth;
	Mip->SizeY = AtlasHeight;
	PlatformData->Mips.Add(Mip);

	// Copy pixel data into mip
	Mip->BulkData.Lock(LOCK_READ_WRITE);
	void* MipData = Mip->BulkData.Realloc(AtlasWidth * AtlasHeight * sizeof(FColor));
	FMemory::Memcpy(MipData, AtlasPixels.GetData(), AtlasWidth * AtlasHeight * sizeof(FColor));
	Mip->BulkData.Unlock();

	Texture->SetPlatformData(PlatformData);

	// Apply texture settings
	Texture->Source.Init(AtlasWidth, AtlasHeight, 1, 1, TSF_BGRA8, nullptr);
	{
		uint8* SourceData = Texture->Source.LockMip(0);
		FMemory::Memcpy(SourceData, AtlasPixels.GetData(), AtlasWidth * AtlasHeight * sizeof(FColor));
		Texture->Source.UnlockMip(0);
	}

	Texture->SRGB = bSRGB;
	Texture->CompressionSettings = TC_Default;
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;

	if (bNoMipmaps)
	{
		Texture->MipGenSettings = TMGS_NoMipmaps;
	}

	// LOD group
	if (LODGroupStr == TEXT("TEXTUREGROUP_Effects"))
	{
		Texture->LODGroup = TEXTUREGROUP_Effects;
	}
	else if (LODGroupStr == TEXT("TEXTUREGROUP_EffectsNotFiltered"))
	{
		Texture->LODGroup = TEXTUREGROUP_EffectsNotFiltered;
	}
	else if (LODGroupStr == TEXT("TEXTUREGROUP_World"))
	{
		Texture->LODGroup = TEXTUREGROUP_World;
	}

	Texture->UpdateResource();
	Texture->PostEditChange();
	Texture->MarkPackageDirty();

	// Save
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	bool bSaved = UPackage::SavePackage(Package, Texture, *PackageFilename, SaveArgs);

	if (!bSaved)
	{
		return FMonolithActionResult::Error(TEXT("Failed to save flipbook texture package"));
	}

	// --- Delete source files if requested ---
	int32 DeletedCount = 0;
	if (bDeleteSources)
	{
		for (const FString& FilePath : FramePaths)
		{
			if (IFileManager::Get().Delete(*FilePath))
			{
				DeletedCount++;
			}
		}
	}

	// --- Return result ---
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("texture_path"), DestPath);

	TArray<TSharedPtr<FJsonValue>> ResArray;
	ResArray.Add(MakeShared<FJsonValueNumber>(AtlasWidth));
	ResArray.Add(MakeShared<FJsonValueNumber>(AtlasHeight));
	Result->SetArrayField(TEXT("resolution"), ResArray);

	Result->SetNumberField(TEXT("frame_count"), FramePaths.Num());
	Result->SetNumberField(TEXT("frame_width"), FrameWidth);
	Result->SetNumberField(TEXT("frame_height"), FrameHeight);

	TArray<TSharedPtr<FJsonValue>> GridResult;
	GridResult.Add(MakeShared<FJsonValueNumber>(GridCols));
	GridResult.Add(MakeShared<FJsonValueNumber>(GridRows));
	Result->SetArrayField(TEXT("grid"), GridResult);

	if (bDeleteSources)
	{
		Result->SetNumberField(TEXT("sources_deleted"), DeletedCount);
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleGetViewportInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	// Get the active level editor viewport
	FLevelEditorViewportClient* ViewportClient = nullptr;
	if (GEditor && GEditor->GetLevelViewportClients().Num() > 0)
	{
		ViewportClient = GEditor->GetLevelViewportClients()[0];
	}

	if (!ViewportClient)
	{
		return FMonolithActionResult::Error(TEXT("No active viewport found"));
	}

	FVector CamLocation = ViewportClient->GetViewLocation();
	FRotator CamRotation = ViewportClient->GetViewRotation();
	float FOV = ViewportClient->ViewFOV;

	FIntPoint ViewportSize = ViewportClient->Viewport->GetSizeXY();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("active_viewport"), 0);

	TSharedPtr<FJsonObject> ResObj = MakeShared<FJsonObject>();
	ResObj->SetNumberField(TEXT("width"), ViewportSize.X);
	ResObj->SetNumberField(TEXT("height"), ViewportSize.Y);
	Result->SetObjectField(TEXT("resolution"), ResObj);

	TArray<TSharedPtr<FJsonValue>> LocArr;
	LocArr.Add(MakeShared<FJsonValueNumber>(CamLocation.X));
	LocArr.Add(MakeShared<FJsonValueNumber>(CamLocation.Y));
	LocArr.Add(MakeShared<FJsonValueNumber>(CamLocation.Z));
	Result->SetArrayField(TEXT("camera_location"), LocArr);

	TArray<TSharedPtr<FJsonValue>> RotArr;
	RotArr.Add(MakeShared<FJsonValueNumber>(CamRotation.Pitch));
	RotArr.Add(MakeShared<FJsonValueNumber>(CamRotation.Yaw));
	RotArr.Add(MakeShared<FJsonValueNumber>(CamRotation.Roll));
	Result->SetArrayField(TEXT("camera_rotation"), RotArr);

	Result->SetNumberField(TEXT("fov"), FOV);
	Result->SetBoolField(TEXT("realtime"), ViewportClient->IsRealtime());

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleDeleteAssets(
	const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* AssetPathsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("asset_paths"), AssetPathsArray) || !AssetPathsArray || AssetPathsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("asset_paths array is required and must not be empty"));
	}

	TArray<FString> AssetPaths;
	for (const auto& Val : *AssetPathsArray)
	{
		FString Path;
		if (Val->TryGetString(Path) && !Path.IsEmpty())
		{
			AssetPaths.Add(Path);
		}
	}

	if (AssetPaths.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No valid paths in asset_paths"));
	}

	// Optional safety: restrict deletion to allowed prefixes
	TArray<FString> AllowedPrefixes;
	const TArray<TSharedPtr<FJsonValue>>* PrefixArray = nullptr;
	if (Params->TryGetArrayField(TEXT("allowed_prefixes"), PrefixArray) && PrefixArray)
	{
		for (const auto& PVal : *PrefixArray)
		{
			FString Prefix;
			if (PVal->TryGetString(Prefix) && !Prefix.IsEmpty())
			{
				AllowedPrefixes.Add(Prefix);
			}
		}
	}

	if (AllowedPrefixes.Num() > 0)
	{
		for (const FString& Path : AssetPaths)
		{
			bool bAllowed = false;
			for (const FString& Prefix : AllowedPrefixes)
			{
				if (Path.StartsWith(Prefix))
				{
					bAllowed = true;
					break;
				}
			}
			if (!bAllowed)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Refusing to delete '%s' — not under any allowed prefix. Allowed: %s"),
					*Path, *FString::Join(AllowedPrefixes, TEXT(", "))));
			}
		}
	}

	// Optional force flag: when true, route through ForceDeleteObjects (nulls
	// referencers). Default false preserves the conservative soft-delete path.
	bool bForce = false;
	Params->TryGetBoolField(TEXT("force"), bForce);

	// Load and delete each asset
	TArray<UObject*> ObjectsToDelete;
	TArray<FString> NotFound;

	for (const FString& Path : AssetPaths)
	{
		UObject* Asset = UEditorAssetLibrary::LoadAsset(Path);
		if (Asset)
		{
			ObjectsToDelete.Add(Asset);
		}
		else
		{
			NotFound.Add(Path);
		}
	}

	// A non-interactive MCP action must NEVER raise a modal Slate dialog: that
	// blocks the game thread and freezes the in-process MCP HTTP server. The
	// ObjectTools delete paths can raise TWO classes of modal — a "Save changes?"
	// prompt on a dirty open asset, and an Error_InUse reference-check dialog
	// (ObjectTools.cpp:3446) when a target is open in an editor. Both branches
	// (soft DeleteObjects and hard ForceDeleteObjects) can hit Error_InUse, and
	// /*bShowConfirmation=*/false does NOT gate it.
	//
	// The fix has two halves:
	//   1. Per-asset preparation (below): clear the package dirty flag so closing
	//      an open editor cannot trigger a save prompt, then force-close any open
	//      editors to drop transient editor referencers that would otherwise
	//      cause the reference check to fail.
	//   2. A tightly-scoped TGuardValue<bool>(GIsRunningUnattendedScript, true)
	//      around the delete call only. FMessageDialog::Open
	//      (MessageDialog.cpp:172) shows UI only when
	//      !FApp::IsUnattended() && !GIsRunningUnattendedScript; under the guard
	//      every ObjectTools modal auto-dismisses to its safe default and the
	//      delete proceeds non-interactively. The guard restores on scope exit.
	for (UObject* Asset : ObjectsToDelete)
	{
		if (UPackage* Pkg = Asset->GetOutermost())
		{
			Pkg->SetDirtyFlag(false);
		}
		if (GEditor)
		{
			if (UAssetEditorSubsystem* AES = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				AES->CloseAllEditorsForAsset(Asset);
			}
		}
	}

	// Capture paths BEFORE deletion: the UObject* pointers dangle once the
	// objects are deleted, so failed_to_delete must be built from these strings.
	TArray<FString> AttemptedPaths;
	AttemptedPaths.Reserve(ObjectsToDelete.Num());
	for (const UObject* Obj : ObjectsToDelete)
	{
		AttemptedPaths.Add(Obj->GetPathName());
	}

	int32 NumDeleted = 0;
	if (ObjectsToDelete.Num() > 0)
	{
		// Scope the unattended guard tightly around the synchronous delete call
		// ONLY so any modal both branches could raise auto-dismisses to its safe
		// default. The guard restores GIsRunningUnattendedScript on scope exit.
		TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
		NumDeleted = bForce
			? ObjectTools::ForceDeleteObjects(ObjectsToDelete, /*ShowConfirmation=*/false)
			: ObjectTools::DeleteObjects(ObjectsToDelete, /*bShowConfirmation=*/false);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), NumDeleted == ObjectsToDelete.Num() && NotFound.Num() == 0);
	Result->SetNumberField(TEXT("deleted"), NumDeleted);
	Result->SetNumberField(TEXT("requested"), AssetPaths.Num());
	Result->SetNumberField(TEXT("found"), ObjectsToDelete.Num());

	// Surface partial failures. The ObjectTools API returns only a count, not
	// which objects survived, so this is count-derived: when fewer objects were
	// deleted than were found, report the requested-and-found paths (pass
	// force=true to delete referenced assets the soft path refuses).
	if (NumDeleted < ObjectsToDelete.Num())
	{
		TArray<TSharedPtr<FJsonValue>> FailedArr;
		for (const FString& P : AttemptedPaths)
		{
			FailedArr.Add(MakeShared<FJsonValueString>(P));
		}
		Result->SetArrayField(TEXT("failed_to_delete"), FailedArr);
	}

	if (NotFound.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> NotFoundArr;
		for (const FString& P : NotFound)
		{
			NotFoundArr.Add(MakeShared<FJsonValueString>(P));
		}
		Result->SetArrayField(TEXT("not_found"), NotFoundArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Action: capture_system_gif
// Captures a Niagara system as a sequence of PNG frames with optional GIF encoding.
// Default mode: frames_only — returns array of PNG paths (always works, no deps).
// Optional: encoder: "ffmpeg" or "python" for GIF encoding.
// ============================================================================
FMonolithActionResult FMonolithEditorActions::HandleCaptureSystemGif(
	const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
		return FMonolithActionResult::Error(TEXT("asset_path is required"));

	double DurationSeconds = Params->HasField(TEXT("duration_seconds")) ? Params->GetNumberField(TEXT("duration_seconds")) : 2.0;
	int32 FPS = Params->HasField(TEXT("fps")) ? static_cast<int32>(Params->GetNumberField(TEXT("fps"))) : 15;
	int32 Resolution = Params->HasField(TEXT("resolution")) ? static_cast<int32>(Params->GetNumberField(TEXT("resolution"))) : 256;
	FString Encoder = Params->HasField(TEXT("encoder")) ? Params->GetStringField(TEXT("encoder")).ToLower() : TEXT("frames_only");

	if (FPS <= 0) FPS = 15;
	if (Resolution <= 0) Resolution = 256;
	if (DurationSeconds <= 0) DurationSeconds = 2.0;

	// Output directory
	FString OutputDir;
	if (Params->HasField(TEXT("output_path")))
	{
		OutputDir = Params->GetStringField(TEXT("output_path"));
		if (FPaths::IsRelative(OutputDir))
		{
			OutputDir = FPaths::ProjectDir() / OutputDir;
		}
	}
	else
	{
		FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		FString SafeName = FPaths::GetBaseFilename(AssetPath);
		OutputDir = FPaths::ProjectDir() / TEXT("Saved/Screenshots/Monolith") /
			FString::Printf(TEXT("GIF_%s_%s"), *Timestamp, *SafeName);
	}
	IFileManager::Get().MakeDirectory(*OutputDir, true);

	// Load system
	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *AssetPath);
	if (!System)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load Niagara system: %s"), *AssetPath));

	// Generate timestamps
	int32 FrameCount = FMath::Max(1, static_cast<int32>(DurationSeconds * FPS));
	TArray<float> Timestamps;
	for (int32 i = 0; i < FrameCount; i++)
	{
		Timestamps.Add(static_cast<float>(i) / static_cast<float>(FPS));
	}

	// Build params for capture_sequence_frames (persistent mode)
	TArray<TSharedPtr<FJsonValue>> TimestampValues;
	for (float T : Timestamps)
	{
		TimestampValues.Add(MakeShared<FJsonValueNumber>(T));
	}

	TSharedRef<FJsonObject> CaptureParams = MakeShared<FJsonObject>();
	CaptureParams->SetStringField(TEXT("asset_path"), AssetPath);
	CaptureParams->SetStringField(TEXT("asset_type"), TEXT("niagara"));
	CaptureParams->SetArrayField(TEXT("timestamps"), TimestampValues);
	CaptureParams->SetStringField(TEXT("output_dir"), OutputDir);
	CaptureParams->SetStringField(TEXT("filename_prefix"), TEXT("gif_frame"));
	CaptureParams->SetBoolField(TEXT("persistent"), true);

	// Set resolution
	TArray<TSharedPtr<FJsonValue>> ResArr;
	ResArr.Add(MakeShared<FJsonValueNumber>(Resolution));
	ResArr.Add(MakeShared<FJsonValueNumber>(Resolution));
	CaptureParams->SetArrayField(TEXT("resolution"), ResArr);

	// Capture frames
	FMonolithActionResult CaptureResult = HandleCaptureSequenceFrames(CaptureParams);
	if (!CaptureResult.bSuccess)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Frame capture failed: %s"), *CaptureResult.ErrorMessage));

	// Collect frame paths from the capture result
	TArray<FString> FramePaths;
	const TArray<TSharedPtr<FJsonValue>>* FramesArr = nullptr;
	if (CaptureResult.Result.IsValid() && CaptureResult.Result->TryGetArrayField(TEXT("frames"), FramesArr))
	{
		for (const auto& FV : *FramesArr)
		{
			const TSharedPtr<FJsonObject> FrameObj = FV->AsObject();
			if (FrameObj.IsValid())
			{
				FString FilePath = FrameObj->GetStringField(TEXT("file"));
				if (!FilePath.IsEmpty())
					FramePaths.Add(FilePath);
			}
		}
	}

	// Build result
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("frame_count"), FramePaths.Num());
	Result->SetNumberField(TEXT("duration"), DurationSeconds);
	Result->SetNumberField(TEXT("fps"), FPS);
	Result->SetNumberField(TEXT("resolution"), Resolution);
	Result->SetStringField(TEXT("output_dir"), OutputDir);

	// Always include frame paths
	TArray<TSharedPtr<FJsonValue>> PathArr;
	for (const FString& P : FramePaths)
		PathArr.Add(MakeShared<FJsonValueString>(P));
	Result->SetArrayField(TEXT("frame_paths"), PathArr);

	// Optional GIF encoding
	if (Encoder != TEXT("frames_only") && FramePaths.Num() > 0)
	{
		FString GifPath = OutputDir / TEXT("output.gif");

		if (Encoder == TEXT("ffmpeg"))
		{
			FString InputPattern = OutputDir / TEXT("gif_frame_%04d.png");
			FString FFmpegArgs = FString::Printf(
				TEXT("-y -framerate %d -i \"%s\" -vf \"scale=%d:-1:flags=lanczos\" -loop 0 \"%s\""),
				FPS, *InputPattern, Resolution, *GifPath);

			FString FFmpegPath = TEXT("ffmpeg");
			int32 ReturnCode = -1;
			FString StdOut, StdErr;

			// Try to run ffmpeg
			bool bLaunched = FPlatformProcess::ExecProcess(*FFmpegPath, *FFmpegArgs, &ReturnCode, &StdOut, &StdErr);

			if (bLaunched && ReturnCode == 0 && IFileManager::Get().FileExists(*GifPath))
			{
				Result->SetStringField(TEXT("gif_path"), GifPath);
				Result->SetStringField(TEXT("encoder_used"), TEXT("ffmpeg"));
			}
			else
			{
				Result->SetStringField(TEXT("encoder_error"),
					FString::Printf(TEXT("ffmpeg failed (code %d). Ensure ffmpeg is in PATH. stderr: %s"),
						ReturnCode, *StdErr.Left(500)));
			}
		}
		else if (Encoder == TEXT("python"))
		{
			// Build a quick python one-liner using imageio
			FString FrameListStr;
			for (const FString& P : FramePaths)
			{
				if (!FrameListStr.IsEmpty()) FrameListStr += TEXT(",");
				FString Escaped = P;
				Escaped.ReplaceInline(TEXT("\\"), TEXT("/"));
				FrameListStr += FString::Printf(TEXT("'%s'"), *Escaped);
			}

			FString PyScript = FString::Printf(
				TEXT("import imageio; frames=[imageio.imread(p) for p in [%s]]; imageio.mimsave('%s',frames,duration=%f,loop=0)"),
				*FrameListStr,
				*GifPath.Replace(TEXT("\\"), TEXT("/")),
				1.0 / FPS);

			FString PythonPath = TEXT("python");
			FString PythonArgs = FString::Printf(TEXT("-c \"%s\""), *PyScript);

			int32 ReturnCode = -1;
			FString StdOut, StdErr;
			bool bLaunched = FPlatformProcess::ExecProcess(*PythonPath, *PythonArgs, &ReturnCode, &StdOut, &StdErr);

			if (bLaunched && ReturnCode == 0 && IFileManager::Get().FileExists(*GifPath))
			{
				Result->SetStringField(TEXT("gif_path"), GifPath);
				Result->SetStringField(TEXT("encoder_used"), TEXT("python"));
			}
			else
			{
				Result->SetStringField(TEXT("encoder_error"),
					FString::Printf(TEXT("python imageio failed (code %d). Ensure python + imageio are installed. stderr: %s"),
						ReturnCode, *StdErr.Left(500)));
			}
		}
		else
		{
			Result->SetStringField(TEXT("encoder_error"),
				FString::Printf(TEXT("Unknown encoder '%s'. Valid: frames_only, ffmpeg, python"), *Encoder));
		}
	}

	return FMonolithActionResult::Success(Result);
}

// --- Automation tests ---
//
// `list_automation_tests` and `run_automation_tests` use the engine's automation
// framework (`FAutomationTestFramework`) to enumerate and execute tests inside the
// already-running editor process. No PIE, no commandlet, no second editor instance.
//
// `run_automation_tests` only handles tests that complete synchronously inside
// `StartTestByName + StopTest` (which is the case for SimpleAutomationTest macros).
// Latent / async tests (TickTests-driven) are skipped with a clear note so the
// caller knows they were not exercised.

namespace MonolithAutomationDetail
{
	static FString GetTestFullPath(const FAutomationTestInfo& Info)
	{
#if ENGINE_MAJOR_VERSION >= 5
		return Info.GetFullTestPath();
#else
		return Info.GetTestName();
#endif
	}

	static void CollectMatchingTests(const FString& Prefix, TArray<FAutomationTestInfo>& OutTests)
	{
		FAutomationTestFramework& Framework = FAutomationTestFramework::Get();

		// Force-load latest test list (covers tests added since the editor started).
		Framework.LoadTestModules();

		// Default RequestedTestFilter is SmokeFilter only (UE constructor default), which
		// excludes most game-module tests. Widen to all filter buckets so any registered
		// test the caller's prefix points at is eligible. Restore on scope exit.
		const EAutomationTestFlags AllFilters = static_cast<EAutomationTestFlags>(
			static_cast<uint32>(EAutomationTestFlags::SmokeFilter) |
			static_cast<uint32>(EAutomationTestFlags::EngineFilter) |
			static_cast<uint32>(EAutomationTestFlags::ProductFilter) |
			static_cast<uint32>(EAutomationTestFlags::PerfFilter) |
			static_cast<uint32>(EAutomationTestFlags::StressFilter) |
			static_cast<uint32>(EAutomationTestFlags::NegativeFilter));
		// No public getter for the previous filter, so just set ours and leave it.
		// Subsequent test runs in the same session pick up this widened filter, which
		// is harmless (other tools will set their own when they need it).
		Framework.SetRequestedTestFilter(AllFilters);

		TArray<FAutomationTestInfo> AllTests;
		Framework.GetValidTestNames(AllTests);

		for (const FAutomationTestInfo& Info : AllTests)
		{
			const FString FullPath = GetTestFullPath(Info);
			if (Prefix.IsEmpty() || FullPath.StartsWith(Prefix))
			{
				OutTests.Add(Info);
			}
		}
	}
}

// --- Scripting actions (HOFF 7) ---

namespace
{
	const TCHAR* PythonLogTypeToString(EPythonLogOutputType T)
	{
		switch (T)
		{
		case EPythonLogOutputType::Info:    return TEXT("info");
		case EPythonLogOutputType::Warning: return TEXT("warning");
		case EPythonLogOutputType::Error:   return TEXT("error");
		default:                            return TEXT("info");
		}
	}
}

FMonolithActionResult FMonolithEditorActions::HandleListAutomationTests(const TSharedPtr<FJsonObject>& Params)
{
	FString Prefix;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("prefix"), Prefix);
	}

	TArray<FAutomationTestInfo> Tests;
	MonolithAutomationDetail::CollectMatchingTests(Prefix, Tests);

	TArray<TSharedPtr<FJsonValue>> TestsJson;
	TestsJson.Reserve(Tests.Num());
	for (const FAutomationTestInfo& Info : Tests)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("full_path"), MonolithAutomationDetail::GetTestFullPath(Info));
		Obj->SetStringField(TEXT("display_name"), Info.GetDisplayName());
		Obj->SetStringField(TEXT("test_name"), Info.GetTestName());
		Obj->SetNumberField(TEXT("flags"), static_cast<double>(static_cast<uint32>(Info.GetTestFlags())));
		TestsJson.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("prefix"), Prefix);
	Result->SetNumberField(TEXT("count"), Tests.Num());
	Result->SetArrayField(TEXT("tests"), TestsJson);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleRunAutomationTests(const TSharedPtr<FJsonObject>& Params)
{
	FString Prefix;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("prefix"), Prefix) || Prefix.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Required parameter: prefix (string, e.g. 'MazeLegends.Bow')"));
	}

	int32 MaxTests = 200;
	if (Params.IsValid())
	{
		double MaxNum = MaxTests;
		if (Params->TryGetNumberField(TEXT("max_tests"), MaxNum))
		{
			MaxTests = FMath::Max(1, FMath::FloorToInt(MaxNum));
		}
	}

	TArray<FAutomationTestInfo> MatchingTests;
	MonolithAutomationDetail::CollectMatchingTests(Prefix, MatchingTests);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("prefix"), Prefix);

	if (MatchingTests.Num() == 0)
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetNumberField(TEXT("total"), 0);
		Result->SetNumberField(TEXT("passed"), 0);
		Result->SetNumberField(TEXT("failed"), 0);
		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("No tests matching prefix '%s' (call list_automation_tests for available tests)"), *Prefix));
		return FMonolithActionResult::Success(Result);
	}

	const int32 TestsToRun = FMath::Min(MaxTests, MatchingTests.Num());

	FAutomationTestFramework& Framework = FAutomationTestFramework::Get();

	TArray<TSharedPtr<FJsonValue>> ResultsJson;
	int32 Passed = 0;
	int32 Failed = 0;
	int32 Skipped = 0;

	for (int32 i = 0; i < TestsToRun; ++i)
	{
		const FAutomationTestInfo& Info = MatchingTests[i];
		const FString FullPath = MonolithAutomationDetail::GetTestFullPath(Info);
		// StartTestByName looks up by the class-name registry key (e.g. FBowDataAssetTest),
		// NOT the human-readable full path. Passing FullPath fails silently and leaves
		// GIsAutomationTesting=false, which trips an assertion when StopTest is called.
		const FString TestKey = Info.GetTestName();

		TSharedPtr<FJsonObject> TestResult = MakeShared<FJsonObject>();
		TestResult->SetStringField(TEXT("full_path"), FullPath);
		TestResult->SetStringField(TEXT("test_name"), TestKey);

		if (!Framework.ContainsTest(TestKey))
		{
			TestResult->SetStringField(TEXT("status"), TEXT("skipped"));
			TestResult->SetStringField(TEXT("reason"),
				FString::Printf(TEXT("ContainsTest('%s') returned false (registry lookup failed)"), *TestKey));
			Skipped++;
			ResultsJson.Add(MakeShared<FJsonValueObject>(TestResult));
			continue;
		}

		Framework.StartTestByName(TestKey, /*RoleIndex=*/0, FullPath);

		FAutomationTestExecutionInfo ExecInfo;
		const bool bCompleted = Framework.StopTest(ExecInfo);
		const bool bSuccess = bCompleted && (ExecInfo.GetErrorTotal() == 0);

		TestResult->SetStringField(TEXT("status"), bSuccess ? TEXT("passed") : TEXT("failed"));
		TestResult->SetNumberField(TEXT("duration_seconds"), ExecInfo.Duration);
		TestResult->SetNumberField(TEXT("error_count"), ExecInfo.GetErrorTotal());
		TestResult->SetNumberField(TEXT("warning_count"), ExecInfo.GetWarningTotal());

		// Capture error messages for visibility.
		if (ExecInfo.GetErrorTotal() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ErrorsJson;
			for (const FAutomationExecutionEntry& Entry : ExecInfo.GetEntries())
			{
				if (Entry.Event.Type == EAutomationEventType::Error)
				{
					ErrorsJson.Add(MakeShared<FJsonValueString>(Entry.Event.Message));
				}
			}
			TestResult->SetArrayField(TEXT("errors"), ErrorsJson);
		}

		if (bSuccess) Passed++; else Failed++;
		ResultsJson.Add(MakeShared<FJsonValueObject>(TestResult));
	}

	Result->SetBoolField(TEXT("success"), Failed == 0);
	Result->SetNumberField(TEXT("total"), TestsToRun);
	Result->SetNumberField(TEXT("passed"), Passed);
	Result->SetNumberField(TEXT("failed"), Failed);
	Result->SetNumberField(TEXT("skipped"), Skipped);
	Result->SetArrayField(TEXT("results"), ResultsJson);

	if (MatchingTests.Num() > TestsToRun)
	{
		Result->SetNumberField(TEXT("truncated_remaining"), MatchingTests.Num() - TestsToRun);
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleRunPython(const TSharedPtr<FJsonObject>& Params)
{
	FString Command;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: command"));
	}

	FString ModeStr = TEXT("execute_file");
	Params->TryGetStringField(TEXT("mode"), ModeStr);
	EPythonCommandExecutionMode Mode = EPythonCommandExecutionMode::ExecuteFile;
	if (ModeStr.Equals(TEXT("execute_file"), ESearchCase::IgnoreCase))
	{
		Mode = EPythonCommandExecutionMode::ExecuteFile;
	}
	else if (ModeStr.Equals(TEXT("execute_statement"), ESearchCase::IgnoreCase))
	{
		Mode = EPythonCommandExecutionMode::ExecuteStatement;
	}
	else if (ModeStr.Equals(TEXT("evaluate_statement"), ESearchCase::IgnoreCase))
	{
		Mode = EPythonCommandExecutionMode::EvaluateStatement;
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid mode '%s'. Valid: execute_file, execute_statement, evaluate_statement."), *ModeStr));
	}

	bool bUnattended = false;
	Params->TryGetBoolField(TEXT("unattended"), bUnattended);

	FString ScopeStr = TEXT("private");
	Params->TryGetStringField(TEXT("file_scope"), ScopeStr);
	EPythonFileExecutionScope FileScope = EPythonFileExecutionScope::Private;
	if (ScopeStr.Equals(TEXT("private"), ESearchCase::IgnoreCase))
	{
		FileScope = EPythonFileExecutionScope::Private;
	}
	else if (ScopeStr.Equals(TEXT("public"), ESearchCase::IgnoreCase))
	{
		FileScope = EPythonFileExecutionScope::Public;
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid file_scope '%s'. Valid: private, public."), *ScopeStr));
	}

	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
	if (!Python)
	{
		// Fallback: load PythonScriptPlugin if it has not been brought up yet.
		FModuleManager::Get().LoadModule(TEXT("PythonScriptPlugin"));
		Python = IPythonScriptPlugin::Get();
	}
	if (!Python)
	{
		return FMonolithActionResult::Error(
			TEXT("PythonScriptPlugin module is not available. Enable PythonScriptPlugin in the project's plugins list."));
	}
	if (!Python->IsPythonAvailable())
	{
		return FMonolithActionResult::Error(
			TEXT("Python is not available in this build (IPythonScriptPlugin::IsPythonAvailable() returned false)."));
	}

	FPythonCommandEx Cmd;
	Cmd.Command = Command;
	Cmd.ExecutionMode = Mode;
	Cmd.FileExecutionScope = FileScope;
	Cmd.Flags = bUnattended ? EPythonCommandFlags::Unattended : EPythonCommandFlags::None;

	const bool bOk = Python->ExecPythonCommandEx(Cmd);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetBoolField(TEXT("success"), bOk);
	Result->SetStringField(TEXT("mode"), ModeStr);
	Result->SetStringField(TEXT("result"), Cmd.CommandResult);

	TArray<TSharedPtr<FJsonValue>> OutputRows;
	OutputRows.Reserve(Cmd.LogOutput.Num());
	for (const FPythonLogOutputEntry& Entry : Cmd.LogOutput)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("type"), PythonLogTypeToString(Entry.Type));
		Row->SetStringField(TEXT("output"), Entry.Output);
		OutputRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetArrayField(TEXT("output"), OutputRows);

	if (!bOk)
	{
		// On failure CommandResult typically holds the Python exception trace.
		// Surface as message so callers don't have to special-case it.
		Result->SetStringField(TEXT("message"), Cmd.CommandResult);
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleLoadLevel(const TSharedPtr<FJsonObject>& Params)
{
	FString Path;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: path"));
	}

	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor is null — load_level requires editor context."));
	}

	ULevelEditorSubsystem* LevelEd = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
	if (!LevelEd)
	{
		return FMonolithActionResult::Error(TEXT("ULevelEditorSubsystem is unavailable."));
	}

	const bool bLoaded = LevelEd->LoadLevel(Path);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), bLoaded);
	Result->SetBoolField(TEXT("loaded"), bLoaded);
	Result->SetStringField(TEXT("path"), Path);
	Result->SetStringField(TEXT("message"),
		bLoaded
			? FString::Printf(TEXT("Loaded level '%s'."), *Path)
			: FString::Printf(TEXT("ULevelEditorSubsystem::LoadLevel returned false for '%s'. Verify the asset exists and is a UWorld."), *Path));

	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// F1: list_dirty_packages / save_packages — scoped dirty report + scoped saver
// (warband-harness plan 2026-06-04)
// ---------------------------------------------------------------------------

namespace MonolithEditorPackages
{
	// Read scope_paths into a prefix list. Returns true if any prefixes were given.
	static bool ParseScopePaths(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutPrefixes)
	{
		OutPrefixes.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Params.IsValid() && Params->TryGetArrayField(TEXT("scope_paths"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& Val : *Arr)
			{
				FString Prefix;
				if (Val.IsValid() && Val->TryGetString(Prefix) && !Prefix.IsEmpty())
				{
					OutPrefixes.Add(Prefix);
				}
			}
		}
		return OutPrefixes.Num() > 0;
	}

	static bool MatchesScope(const FString& PackageName, const TArray<FString>& Prefixes)
	{
		if (Prefixes.Num() == 0)
		{
			return true;
		}
		for (const FString& Prefix : Prefixes)
		{
			if (PackageName.StartsWith(Prefix))
			{
				return true;
			}
		}
		return false;
	}

	// A package backs disk content if it has a real /Game (or other mount) name and
	// is not the transient package. Filters out /Engine/Transient and GC-only objects.
	static bool IsDiskPackage(const UPackage* Package)
	{
		if (!Package || Package == GetTransientPackage())
		{
			return false;
		}
		if (Package->HasAnyFlags(RF_Transient) || Package->HasAnyPackageFlags(PKG_PlayInEditor))
		{
			return false;
		}
		const FString Name = Package->GetName();
		return Name.StartsWith(TEXT("/")) && !Name.StartsWith(TEXT("/Temp/")) &&
			FPackageName::IsValidLongPackageName(Name);
	}

	// Collect dirty, on-disk packages matching the scope (and include_transient flag).
	static void CollectDirtyPackages(const TArray<FString>& ScopePrefixes, bool bIncludeTransient,
		TArray<UPackage*>& OutPackages)
	{
		OutPackages.Reset();
		ForEachObjectOfClass(UPackage::StaticClass(), [&](UObject* Obj)
		{
			UPackage* Package = Cast<UPackage>(Obj);
			if (!Package || !Package->IsDirty())
			{
				return;
			}
			if (!bIncludeTransient && !IsDiskPackage(Package))
			{
				return;
			}
			if (!MatchesScope(Package->GetName(), ScopePrefixes))
			{
				return;
			}
			OutPackages.Add(Package);
		}, /*bIncludeDerivedClasses=*/false);
	}
}

FMonolithActionResult FMonolithEditorActions::HandleListDirtyPackages(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithEditorPackages;

	TArray<FString> ScopePrefixes;
	ParseScopePaths(Params, ScopePrefixes);

	bool bIncludeTransient = false;
	if (Params.IsValid()) { Params->TryGetBoolField(TEXT("include_transient"), bIncludeTransient); }

	// Map-vs-content filters default to true (report both kinds) per the F1 plan.
	bool bIncludeMaps = true;
	bool bIncludeContent = true;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("include_maps"), bIncludeMaps);
		Params->TryGetBoolField(TEXT("include_content"), bIncludeContent);
	}

	TArray<UPackage*> DirtyPackages;
	CollectDirtyPackages(ScopePrefixes, bIncludeTransient, DirtyPackages);

	TArray<TSharedPtr<FJsonValue>> Rows;
	for (UPackage* Package : DirtyPackages)
	{
		const FString PackageName = Package->GetName();
		const bool bIsMap = Package->ContainsMap();
		const bool bIsDisk = IsDiskPackage(Package);

		// Apply the map/content filters before emitting the row.
		if (bIsMap && !bIncludeMaps)
		{
			continue;
		}
		if (!bIsMap && !bIncludeContent)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("package"), PackageName);
		Row->SetBoolField(TEXT("is_map"), bIsMap);
		Row->SetBoolField(TEXT("transient"), !bIsDisk);
		if (bIsDisk)
		{
			const FString Ext = bIsMap ? FPackageName::GetMapPackageExtension()
									   : FPackageName::GetAssetPackageExtension();
			FString Filename;
			if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename, Ext))
			{
				Row->SetStringField(TEXT("disk_path"), Filename);
			}
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	Result->SetArrayField(TEXT("dirty_packages"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleSavePackages(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithEditorPackages;

	const TArray<TSharedPtr<FJsonValue>>* PkgArr = nullptr;
	if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("packages"), PkgArr) || !PkgArr || PkgArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: packages (non-empty array of long package names)"));
	}

	TArray<FString> RequestedNames;
	for (const TSharedPtr<FJsonValue>& Val : *PkgArr)
	{
		FString Name;
		if (Val.IsValid() && Val->TryGetString(Name) && !Name.IsEmpty())
		{
			RequestedNames.AddUnique(Name);
		}
	}
	if (RequestedNames.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("packages array contained no valid package names"));
	}

	bool bFailOnUnrequested = false;
	if (Params.IsValid()) { Params->TryGetBoolField(TEXT("fail_on_unrequested_dirty"), bFailOnUnrequested); }

	bool bDryRun = false;
	if (Params.IsValid()) { Params->TryGetBoolField(TEXT("dry_run"), bDryRun); }

	// Pre-scan: when requested, abort before saving anything if a dirty package
	// exists outside the request set (bounded by scope_paths if given).
	if (bFailOnUnrequested)
	{
		TArray<FString> ScopePrefixes;
		ParseScopePaths(Params, ScopePrefixes);

		TArray<UPackage*> DirtyPackages;
		CollectDirtyPackages(ScopePrefixes, /*bIncludeTransient=*/false, DirtyPackages);

		TArray<FString> Unrequested;
		for (UPackage* Package : DirtyPackages)
		{
			if (!RequestedNames.Contains(Package->GetName()))
			{
				Unrequested.Add(Package->GetName());
			}
		}
		if (Unrequested.Num() > 0)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("fail_on_unrequested_dirty: %d dirty package(s) outside the request set: %s"),
				Unrequested.Num(), *FString::Join(Unrequested, TEXT(", "))));
		}
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 SavedCount = 0;
	for (const FString& PackageName : RequestedNames)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("package"), PackageName);

		UPackage* Package = FindPackage(nullptr, *PackageName);
		if (!Package)
		{
			Package = LoadPackage(nullptr, *PackageName, LOAD_None);
		}
		if (!Package)
		{
			Row->SetBoolField(TEXT("saved"), false);
			Row->SetStringField(TEXT("error"), TEXT("package not found / could not be loaded"));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}

		const bool bIsMap = Package->ContainsMap();
		const FString Ext = bIsMap ? FPackageName::GetMapPackageExtension()
								   : FPackageName::GetAssetPackageExtension();
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, Ext);

		Row->SetBoolField(TEXT("is_map"), bIsMap);
		Row->SetStringField(TEXT("disk_path"), Filename);

		if (bDryRun)
		{
			// Report intent only — nothing is written. A package "would save" if it is
			// currently dirty; clean packages are reported as no-op.
			const bool bWouldSave = Package->IsDirty();
			Row->SetBoolField(TEXT("would_save"), bWouldSave);
			Row->SetBoolField(TEXT("dirty"), bWouldSave);
			if (bWouldSave) { ++SavedCount; }
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		const bool bSaved = UPackage::SavePackage(Package, nullptr, *Filename, SaveArgs);

		Row->SetBoolField(TEXT("saved"), bSaved);
		if (bSaved) { ++SavedCount; }
		else { Row->SetStringField(TEXT("error"), TEXT("UPackage::SavePackage returned false")); }
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), SavedCount == RequestedNames.Num());
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetNumberField(bDryRun ? TEXT("would_save") : TEXT("saved"), SavedCount);
	Result->SetNumberField(TEXT("requested"), RequestedNames.Num());
	Result->SetArrayField(TEXT("results"), Rows);
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// F2/F3 shared PIE-smoke scaffolding (warband-harness plan 2026-06-04)
// ---------------------------------------------------------------------------

namespace MonolithEditorPieSmoke
{
	// Default post-marker patterns every smoke counts (case-insensitive substring).
	static const TCHAR* DefaultPatterns[] =
	{
		TEXT("Blueprint Runtime Error"),
		TEXT("Accessed None"),
		TEXT("LogChooser"),
	};

	// Resolve the effective pattern set: defaults + any caller-supplied log_patterns.
	static TArray<FString> ResolvePatterns(const TSharedPtr<FJsonObject>& Params)
	{
		TArray<FString> Patterns;
		for (const TCHAR* P : DefaultPatterns)
		{
			Patterns.Add(P);
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Params.IsValid() && Params->TryGetArrayField(TEXT("log_patterns"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& Val : *Arr)
			{
				FString P;
				if (Val.IsValid() && Val->TryGetString(P) && !P.IsEmpty())
				{
					Patterns.AddUnique(P);
				}
			}
		}
		return Patterns;
	}

	// Count, per pattern, how many captured log lines after MarkerTimestamp contain it.
	static TSharedPtr<FJsonObject> CountPostMarker(FMonolithLogCapture* LogCapture,
		double MarkerTimestamp, const TArray<FString>& Patterns, int32& OutTotalMatches)
	{
		OutTotalMatches = 0;
		TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
		if (!LogCapture)
		{
			return Counts;
		}

		// Pull every captured entry since the marker (broadest verbosity).
		const TArray<FMonolithLogEntry> Entries = LogCapture->GetEntriesSince(
			MarkerTimestamp, /*CategoryFilter*/{}, ELogVerbosity::VeryVerbose, FMonolithLogCapture::MaxEntries);

		for (const FString& Pattern : Patterns)
		{
			int32 Count = 0;
			for (const FMonolithLogEntry& Entry : Entries)
			{
				const FString CategoryStr = Entry.Category.ToString();
				if (Entry.Message.Contains(Pattern, ESearchCase::IgnoreCase) ||
					CategoryStr.Contains(Pattern, ESearchCase::IgnoreCase))
				{
					++Count;
				}
			}
			Counts->SetNumberField(Pattern, Count);
			OutTotalMatches += Count;
		}
		return Counts;
	}

	// Run the caller's optional console + python scripts on the ready PIE world.
	static void RunScripts(const TSharedPtr<FJsonObject>& Params, UWorld* PieWorld)
	{
		const TArray<TSharedPtr<FJsonValue>>* ConsoleArr = nullptr;
		if (Params.IsValid() && Params->TryGetArrayField(TEXT("console_script"), ConsoleArr) && ConsoleArr && PieWorld)
		{
			APlayerController* PC = PieWorld->GetFirstPlayerController();
			for (const TSharedPtr<FJsonValue>& Val : *ConsoleArr)
			{
				FString Command;
				if (Val.IsValid() && Val->TryGetString(Command) && !Command.IsEmpty())
				{
					if (PC) { PC->ConsoleCommand(Command, /*bWriteToLog=*/true); }
					else if (GEngine) { GEngine->Exec(PieWorld, *Command); }
				}
			}
		}

		FString PythonScript;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("python_script"), PythonScript) && !PythonScript.IsEmpty())
		{
			if (IPythonScriptPlugin* Python = IPythonScriptPlugin::Get())
			{
				if (Python->IsPythonAvailable())
				{
					FPythonCommandEx Cmd;
					Cmd.Command = PythonScript;
					Cmd.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
					Python->ExecPythonCommandEx(Cmd);
				}
			}
		}
	}

	// Load the requested map into the editor before PIE (optional). Returns false +
	// OutError when a map path was given but failed to load.
	static bool LoadMapIfRequested(const TSharedPtr<FJsonObject>& Params, FString& OutError)
	{
		FString MapPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("map"), MapPath) || MapPath.IsEmpty())
		{
			return true; // No map requested — use the current editor level.
		}
		if (!GEditor)
		{
			OutError = TEXT("GEditor unavailable — cannot load map for PIE smoke.");
			return false;
		}
		ULevelEditorSubsystem* LevelEd = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
		if (!LevelEd || !LevelEd->LoadLevel(MapPath))
		{
			OutError = FString::Printf(TEXT("Failed to load map '%s' for PIE smoke."), *MapPath);
			return false;
		}
		return true;
	}

	// Resolve the AnimInstance variable names to sample: caller-supplied sample_vars,
	// or the default GroundSpeed / bShouldMove / DesiredYawDelta set.
	static TArray<FString> ResolveSampleVars(const TSharedPtr<FJsonObject>& Params)
	{
		TArray<FString> Vars;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Params.IsValid() && Params->TryGetArrayField(TEXT("sample_vars"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& Val : *Arr)
			{
				FString V;
				if (Val.IsValid() && Val->TryGetString(V) && !V.IsEmpty())
				{
					Vars.AddUnique(V);
				}
			}
		}
		if (Vars.Num() == 0)
		{
			Vars = { TEXT("GroundSpeed"), TEXT("bShouldMove"), TEXT("DesiredYawDelta") };
		}
		return Vars;
	}

	static const TCHAR* StatusToString(EPieSmokeStatus Status)
	{
		switch (Status)
		{
		case EPieSmokeStatus::Running:  return TEXT("running");
		case EPieSmokeStatus::Complete: return TEXT("complete");
		case EPieSmokeStatus::Stopped:  return TEXT("stopped");
		case EPieSmokeStatus::Error:    return TEXT("error");
		default:                        return TEXT("unknown");
		}
	}

	// Build a poll/stop report for a session. When bFull, emit every per-frame sample
	// (and any captured frame paths); always emit the per-var min/max/last summary and
	// the post-marker pattern counts.
	static TSharedPtr<FJsonObject> BuildSessionReport(const FPieSmokeSession& S, bool bFull,
		FMonolithLogCapture* LogCapture)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("session_id"), S.Id);
		Root->SetStringField(TEXT("status"), StatusToString(S.Status));
		Root->SetStringField(TEXT("marker"), S.Marker);
		Root->SetStringField(TEXT("map"), S.MapName);
		Root->SetNumberField(TEXT("duration"), S.DurationSeconds);
		Root->SetBoolField(TEXT("pie_active"), S.bPieActive);
		Root->SetBoolField(TEXT("pie_ready"), S.bReady);
		Root->SetNumberField(TEXT("sample_count"), S.Samples.Num());

		const double EndTime = (S.Status == EPieSmokeStatus::Running)
			? FPlatformTime::Seconds() : S.LastObservedSeconds;
		Root->SetNumberField(TEXT("elapsed_seconds"), FMath::Max(0.0, EndTime - S.StartTimeSeconds));

		if (!S.ErrorReason.IsEmpty())
		{
			Root->SetStringField(TEXT("error"), S.ErrorReason);
		}

		// Per-var summary: min / max / last across all samples.
		TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
		for (const FString& VarName : S.SampleVarNames)
		{
			bool bSeen = false;
			bool bBool = false;
			double MinV = 0.0, MaxV = 0.0, LastV = 0.0;
			bool LastBool = false;
			for (const FPieSmokeSample& Sample : S.Samples)
			{
				for (const FPieSmokeSampleVar& Var : Sample.Vars)
				{
					if (Var.Name != VarName) { continue; }
					if (Var.bIsBool)
					{
						bBool = true;
						LastBool = Var.BoolValue;
						bSeen = true;
					}
					else
					{
						const double Num = Var.NumberValue;
						if (!bSeen) { MinV = MaxV = Num; }
						else { MinV = FMath::Min(MinV, Num); MaxV = FMath::Max(MaxV, Num); }
						LastV = Num;
						bSeen = true;
					}
				}
			}
			if (!bSeen) { continue; }
			TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
			if (bBool)
			{
				VarObj->SetStringField(TEXT("type"), TEXT("bool"));
				VarObj->SetBoolField(TEXT("last"), LastBool);
			}
			else
			{
				VarObj->SetStringField(TEXT("type"), TEXT("number"));
				VarObj->SetNumberField(TEXT("min"), MinV);
				VarObj->SetNumberField(TEXT("max"), MaxV);
				VarObj->SetNumberField(TEXT("last"), LastV);
			}
			Summary->SetObjectField(VarName, VarObj);
		}
		Root->SetObjectField(TEXT("var_summary"), Summary);

		// Clip-variant fields.
		if (S.bCaptureFrames)
		{
			Root->SetStringField(TEXT("output_dir"), S.OutputDir);
			Root->SetNumberField(TEXT("frame_count"), S.CaptureFrameIndex);
			if (S.bCaptureDeferred)
			{
				Root->SetStringField(TEXT("capture_status"), TEXT("deferred"));
			}
			TArray<TSharedPtr<FJsonValue>> Frames;
			for (const FPieSmokeSample& Sample : S.Samples)
			{
				if (!Sample.FramePath.IsEmpty())
				{
					Frames.Add(MakeShared<FJsonValueString>(Sample.FramePath));
				}
			}
			Root->SetArrayField(TEXT("frame_paths"), Frames);
		}

		// Full per-frame sample array (on completion or when explicitly requested).
		if (bFull)
		{
			TArray<TSharedPtr<FJsonValue>> SampleArr;
			for (const FPieSmokeSample& Sample : S.Samples)
			{
				TSharedPtr<FJsonObject> SObj = MakeShared<FJsonObject>();
				SObj->SetNumberField(TEXT("t"), Sample.TimeSeconds);
				if (!Sample.FramePath.IsEmpty())
				{
					SObj->SetStringField(TEXT("frame_path"), Sample.FramePath);
				}
				for (const FPieSmokeSampleVar& Var : Sample.Vars)
				{
					if (Var.bIsBool) { SObj->SetBoolField(Var.Name, Var.BoolValue); }
					else { SObj->SetNumberField(Var.Name, Var.NumberValue); }
				}
				SampleArr.Add(MakeShared<FJsonValueObject>(SObj));
			}
			Root->SetArrayField(TEXT("samples"), SampleArr);
		}

		// Post-marker pattern counts from the shared log capture.
		int32 TotalMatches = 0;
		TSharedPtr<FJsonObject> Counts = CountPostMarker(LogCapture, S.StartTimeSeconds, S.LogPatterns, TotalMatches);
		Root->SetObjectField(TEXT("post_marker_counts"), Counts);
		Root->SetNumberField(TEXT("total_matches"), TotalMatches);
		Root->SetBoolField(TEXT("ok"),
			(S.Status == EPieSmokeStatus::Complete) && S.bReady && TotalMatches == 0);

		return Root;
	}
}

FMonolithActionResult FMonolithEditorActions::HandleRunPieSmoke(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithEditorPieSmoke;

	if (!GEditor || !GUnrealEd)
	{
		return FMonolithActionResult::Error(TEXT("run_pie_smoke requires editor context (GEditor/GUnrealEd)."));
	}
	if (FindActivePieWorld())
	{
		return FMonolithActionResult::Error(TEXT("A PIE session is already running — stop it before run_pie_smoke."));
	}

	FString Marker = TEXT("WARBAND_SMOKE");
	Params->TryGetStringField(TEXT("marker"), Marker);

	double Duration = 5.0;
	if (Params->HasField(TEXT("duration"))) { Duration = Params->GetNumberField(TEXT("duration")); }
	Duration = FMath::Clamp(Duration, 0.0, 120.0);

	// Compile-error policy: "refuse" (default, safe) returns an error + the offending
	// Blueprints and never starts PIE; "suppress" proceeds and silences the engine's
	// blocking compile-error prompt via the StartPieInternal unattended guard.
	FString CompileMode = TEXT("refuse");
	Params->TryGetStringField(TEXT("on_compile_errors"), CompileMode);
	const bool bSuppressModals = CompileMode.Equals(TEXT("suppress"), ESearchCase::IgnoreCase);

	// Optional map load before PIE.
	FString LoadError;
	if (!LoadMapIfRequested(Params, LoadError))
	{
		return FMonolithActionResult::Error(LoadError);
	}

	// PIE pre-flight: scan for errored Blueprints AFTER any map load (the loaded level
	// brings its own Blueprints into memory). In refuse mode, never PIE a broken world —
	// a compile-error modal would run a nested game-thread loop and strangle the MCP server.
	{
		TArray<FErroredBlueprintEntry> Errored;
		ScanErroredBlueprints(Errored);
		if (Errored.Num() > 0 && !bSuppressModals)
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetNumberField(TEXT("errored_blueprint_count"), Errored.Num());
			ErrObj->SetArrayField(TEXT("errored_blueprints"), ErroredBlueprintsToJson(Errored));
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("run_pie_smoke refused: %d Blueprint(s) have unresolved compile errors. ")
					TEXT("Starting PIE would raise a blocking modal that freezes the editor + MCP server. ")
					TEXT("Fix the Blueprints, or pass on_compile_errors=\"suppress\" to PIE anyway."),
					Errored.Num()))
				.WithErrorData(ErrObj);
		}
	}

	// Start PIE synchronously (the start request itself is safe inside the handler —
	// the re-entrancy crash was the OLD pump driving UWorld/GEditor::Tick afterwards;
	// that work now happens on the editor's real frames via the session observer).
	// bSuppressModals wraps the request in the unattended guard so a compile-error
	// prompt resolves to its default instead of blocking.
	FString StartError;
	if (!StartPieInternal(StartError, bSuppressModals))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to start PIE: %s"), *StartError));
	}

	// Emit the marker now; the observer counts only post-marker log lines. PIE may not
	// have run BeginPlay yet — the observer waits for HasBegunPlay before sampling.
	UWorld* PieWorld = FindActivePieWorld();
	UE_LOG(LogMonolith, Display, TEXT("%s begin (map=%s)"), *Marker,
		PieWorld ? *PieWorld->GetMapName() : TEXT("<current>"));

	// Optional console / python scripts run once at start (best-effort).
	if (PieWorld)
	{
		RunScripts(Params, PieWorld);
	}

	// Register the async session — the editor frame loop advances it from here.
	FPieSmokeSession Session;
	Session.StartTimeSeconds = FPlatformTime::Seconds();
	Session.DurationSeconds = Duration;
	Session.Marker = Marker;
	Session.MapName = PieWorld ? PieWorld->GetMapName() : TEXT("<current>");
	Session.LogPatterns = ResolvePatterns(Params);
	Session.SampleVarNames = ResolveSampleVars(Params);
	Params->TryGetStringField(TEXT("pawn_class"), Session.PawnClassFilter);

	const FString SessionId = FPieSmokeSessionManager::Get().CreateSession(MoveTemp(Session));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("session_id"), SessionId);
	Result->SetStringField(TEXT("status"), TEXT("running"));
	Result->SetBoolField(TEXT("started"), true);
	Result->SetStringField(TEXT("marker"), Marker);
	Result->SetNumberField(TEXT("duration"), Duration);
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// poll_pie_smoke / stop_pie_smoke — read progress / force-end an async session
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithEditorActions::HandlePollPieSmoke(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithEditorPieSmoke;

	FString SessionId;
	if (!Params->TryGetStringField(TEXT("session_id"), SessionId) || SessionId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("poll_pie_smoke requires a session_id."));
	}

	FPieSmokeSessionManager& Mgr = FPieSmokeSessionManager::Get();
	FPieSmokeSession* Session = Mgr.Find(SessionId);
	if (!Session)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown PIE-smoke session '%s'."), *SessionId));
	}

	bool bIncludeSamples = false;
	Params->TryGetBoolField(TEXT("include_samples"), bIncludeSamples);
	const bool bFull = bIncludeSamples || (Session->Status != EPieSmokeStatus::Running);

	return FMonolithActionResult::Success(
		BuildSessionReport(*Session, bFull, Mgr.GetLogCapture()));
}

FMonolithActionResult FMonolithEditorActions::HandleStopPieSmoke(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithEditorPieSmoke;

	FString SessionId;
	Params->TryGetStringField(TEXT("session_id"), SessionId); // empty => stop all

	FPieSmokeSessionManager& Mgr = FPieSmokeSessionManager::Get();
	const int32 Stopped = Mgr.Stop(SessionId);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("stopped"), Stopped);

	if (!SessionId.IsEmpty())
	{
		if (FPieSmokeSession* Session = Mgr.Find(SessionId))
		{
			Result->SetObjectField(TEXT("report"),
				BuildSessionReport(*Session, /*bFull=*/true, Mgr.GetLogCapture()));
		}
		else
		{
			Result->SetStringField(TEXT("warning"),
				FString::Printf(TEXT("Unknown session '%s' — nothing to stop."), *SessionId));
		}
	}
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// list_errored_blueprints — read-only PIE pre-flight scan
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithEditorActions::HandleListErroredBlueprints(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FErroredBlueprintEntry> Errored;
	ScanErroredBlueprints(Errored);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Errored.Num());
	Result->SetArrayField(TEXT("blueprints"), ErroredBlueprintsToJson(Errored));
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// F3: capture_pie_movement_clip — async session + per-interval frame capture +
// AnimInstance sampling (warband-harness plan 2026-06-04)
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithEditorActions::HandleCapturePieMovementClip(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithEditorPieSmoke;

	if (!GEditor || !GUnrealEd)
	{
		return FMonolithActionResult::Error(TEXT("capture_pie_movement_clip requires editor context (GEditor/GUnrealEd)."));
	}
	if (FindActivePieWorld())
	{
		return FMonolithActionResult::Error(TEXT("A PIE session is already running — stop it before capture_pie_movement_clip."));
	}

	FString Marker = TEXT("WARBAND_CLIP");
	Params->TryGetStringField(TEXT("marker"), Marker);

	double Duration = 5.0;
	if (Params->HasField(TEXT("duration"))) { Duration = Params->GetNumberField(TEXT("duration")); }
	Duration = FMath::Clamp(Duration, 0.0, 120.0);

	double Interval = 0.25;
	if (Params->HasField(TEXT("capture_interval"))) { Interval = Params->GetNumberField(TEXT("capture_interval")); }
	Interval = FMath::Clamp(Interval, 0.05, 5.0);

	FString OutputDir;
	if (Params->HasField(TEXT("output_path")))
	{
		OutputDir = Params->GetStringField(TEXT("output_path"));
		if (FPaths::IsRelative(OutputDir)) { OutputDir = FPaths::ProjectDir() / OutputDir; }
	}
	else
	{
		const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		OutputDir = FPaths::ProjectDir() / TEXT("Saved/Screenshots/Monolith/PieClip") / Stamp;
	}

	FString LoadError;
	if (!LoadMapIfRequested(Params, LoadError))
	{
		return FMonolithActionResult::Error(LoadError);
	}

	// Start PIE synchronously (safe — see HandleRunPieSmoke). Frame capture + sampling
	// run on the editor's real frames via the session observer.
	FString StartError;
	if (!StartPieInternal(StartError))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to start PIE: %s"), *StartError));
	}

	UWorld* PieWorld = FindActivePieWorld();
	UE_LOG(LogMonolith, Display, TEXT("%s begin"), *Marker);
	if (PieWorld)
	{
		RunScripts(Params, PieWorld);
	}

	FPieSmokeSession Session;
	Session.StartTimeSeconds = FPlatformTime::Seconds();
	Session.DurationSeconds = Duration;
	Session.Marker = Marker;
	Session.MapName = PieWorld ? PieWorld->GetMapName() : TEXT("<current>");
	Session.LogPatterns = ResolvePatterns(Params);
	Session.SampleVarNames = ResolveSampleVars(Params);
	Params->TryGetStringField(TEXT("pawn_class"), Session.PawnClassFilter);
	Session.bCaptureFrames = true;
	Session.CaptureInterval = Interval;
	Session.OutputDir = OutputDir;

	const FString SessionId = FPieSmokeSessionManager::Get().CreateSession(MoveTemp(Session));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("session_id"), SessionId);
	Result->SetStringField(TEXT("status"), TEXT("running"));
	Result->SetBoolField(TEXT("started"), true);
	Result->SetStringField(TEXT("marker"), Marker);
	Result->SetStringField(TEXT("output_dir"), OutputDir);
	Result->SetNumberField(TEXT("duration"), Duration);
	Result->SetNumberField(TEXT("capture_interval"), Interval);
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// F4: create_nav_harness_map — build a nav test map from a JSON spec
// (warband-harness plan 2026-06-04)
//
// Nav rebuild + validation are delegated to the registered `ai` actions via
// runtime string dispatch (FMonolithToolRegistry::ExecuteAction) so MonolithEditor
// takes NO compile-time dependency on MonolithAI / the NavigationSystem module.
// ---------------------------------------------------------------------------

namespace MonolithEditorNavHarness
{
	// Parse a [x,y,z] (or {x,y,z}) JSON value into an FVector. Returns false if absent.
	static bool ParseVec3(const TSharedPtr<FJsonObject>& Obj, const FString& Field, FVector& OutVec)
	{
		if (!Obj.IsValid() || !Obj->HasField(Field))
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Obj->TryGetArrayField(Field, Arr) && Arr && Arr->Num() >= 3)
		{
			OutVec = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
			return true;
		}
		const TSharedPtr<FJsonObject>* SubObj = nullptr;
		if (Obj->TryGetObjectField(Field, SubObj) && SubObj && (*SubObj)->Values.Num() >= 3)
		{
			OutVec = FVector((*SubObj)->GetNumberField(TEXT("x")),
							 (*SubObj)->GetNumberField(TEXT("y")),
							 (*SubObj)->GetNumberField(TEXT("z")));
			return true;
		}
		return false;
	}

	static TArray<TSharedPtr<FJsonValue>> Vec3ToJson(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Arr;
	}

	// Apply a JSON "properties" object onto a spawned actor reflectively. Supports
	// float/double, int, bool, string/name, and FSoftObjectPath (string value).
	// Unknown / unsupported properties are recorded in OutSkipped, never fatal.
	static void ApplyProperties(AActor* Actor, const TSharedPtr<FJsonObject>& PropObj, TArray<FString>& OutApplied, TArray<FString>& OutSkipped)
	{
		if (!Actor || !PropObj.IsValid())
		{
			return;
		}
		for (const auto& Pair : PropObj->Values)
		{
			const FString& PropName = Pair.Key;
			const TSharedPtr<FJsonValue>& Value = Pair.Value;
			FProperty* Prop = Actor->GetClass()->FindPropertyByName(FName(*PropName));
			if (!Prop)
			{
				OutSkipped.Add(PropName + TEXT(" (not found)"));
				continue;
			}
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Actor);

			if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
			{
				FloatProp->SetPropertyValue(ValuePtr, Value->AsNumber());
			}
			else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
			{
				DoubleProp->SetPropertyValue(ValuePtr, Value->AsNumber());
			}
			else if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
			{
				IntProp->SetPropertyValue(ValuePtr, static_cast<int32>(Value->AsNumber()));
			}
			else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
			{
				BoolProp->SetPropertyValue(ValuePtr, Value->AsBool());
			}
			else if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
			{
				StrProp->SetPropertyValue(ValuePtr, Value->AsString());
			}
			else if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
			{
				NameProp->SetPropertyValue(ValuePtr, FName(*Value->AsString()));
			}
			else if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
			{
				// FSoftObjectPath (and any string-importable struct) goes through the
				// reflection text importer — same pattern as MonolithMaterialActions.
				if (StructProp->Struct == TBaseStructure<FSoftObjectPath>::Get())
				{
					const FString PathStr = Value->AsString();
					Prop->ImportText_Direct(*PathStr, ValuePtr, Actor, PPF_None);
				}
				else
				{
					OutSkipped.Add(PropName + TEXT(" (unsupported struct ") + StructProp->Struct->GetName() + TEXT(")"));
					continue;
				}
			}
			else if (CastField<FSoftObjectProperty>(Prop) || CastField<FObjectProperty>(Prop))
			{
				// Asset reference by path string (e.g. an animation DB or mesh).
				const FString PathStr = Value->AsString();
				Prop->ImportText_Direct(*PathStr, ValuePtr, Actor, PPF_None);
			}
			else
			{
				OutSkipped.Add(PropName + TEXT(" (unsupported type ") + Prop->GetClass()->GetName() + TEXT(")"));
				continue;
			}
			OutApplied.Add(PropName);
		}
	}
}

FMonolithActionResult FMonolithEditorActions::HandleCreateNavHarnessMap(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithEditorNavHarness;

	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("create_nav_harness_map requires editor context (GEditor)."));
	}

	FString MapPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("path"), MapPath) || MapPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: path"));
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	// 1. Create the blank UWorld via the existing editor.create_empty_map action,
	//    then load it as the active editor world so spawns + nav target it.
	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("path"), MapPath);
		const FMonolithActionResult CreateRes = Registry.ExecuteAction(TEXT("editor"), TEXT("create_empty_map"), CreateParams);
		if (!CreateRes.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("create_empty_map failed: %s"), *CreateRes.ErrorMessage));
		}
	}

	ULevelEditorSubsystem* LevelEd = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
	if (!LevelEd || !LevelEd->LoadLevel(MapPath))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Created map but failed to load '%s' as the editor world."), *MapPath));
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world after loading the harness map."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> SpawnedActors;

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// 2. Floor — default 50x50m plane unless overridden.
	{
		FVector FloorLoc = FVector::ZeroVector;
		FVector FloorScale(50.0f, 50.0f, 1.0f);
		FString FloorMeshPath = TEXT("/Engine/BasicShapes/Plane.Plane");

		const TSharedPtr<FJsonObject>* FloorObj = nullptr;
		if (Params->TryGetObjectField(TEXT("floor"), FloorObj) && FloorObj)
		{
			ParseVec3(*FloorObj, TEXT("location"), FloorLoc);
			ParseVec3(*FloorObj, TEXT("scale"), FloorScale);
			FString MeshOverride;
			if ((*FloorObj)->TryGetStringField(TEXT("mesh"), MeshOverride) && !MeshOverride.IsEmpty())
			{
				FloorMeshPath = MeshOverride;
			}
		}

		AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FloorLoc, FRotator::ZeroRotator, SpawnParams);
		if (Floor)
		{
			if (UStaticMeshComponent* FloorComp = Floor->GetStaticMeshComponent())
			{
				FloorComp->SetMobility(EComponentMobility::Static);
				if (UStaticMesh* FloorMesh = LoadObject<UStaticMesh>(nullptr, *FloorMeshPath))
				{
					FloorComp->SetStaticMesh(FloorMesh);
				}
			}
			Floor->SetActorScale3D(FloorScale);
			Floor->SetActorLabel(TEXT("Harness_Floor"));
			Floor->SetFolderPath(TEXT("Harness"));
			Floor->MarkPackageDirty();

			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("kind"), TEXT("floor"));
			Row->SetStringField(TEXT("name"), Floor->GetActorNameOrLabel());
			SpawnedActors.Add(MakeShared<FJsonValueObject>(Row));
		}
	}

	// 3. Camera (optional).
	{
		const TSharedPtr<FJsonObject>* CamObj = nullptr;
		if (Params->TryGetObjectField(TEXT("camera"), CamObj) && CamObj)
		{
			FVector CamLoc(0.0f, 0.0f, 1000.0f);
			FVector CamRot(-60.0f, 0.0f, 0.0f);
			ParseVec3(*CamObj, TEXT("location"), CamLoc);
			ParseVec3(*CamObj, TEXT("rotation"), CamRot);

			ACameraActor* Cam = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), CamLoc,
				FRotator(CamRot.X, CamRot.Y, CamRot.Z), SpawnParams);
			if (Cam)
			{
				Cam->SetActorLabel(TEXT("Harness_Camera"));
				Cam->SetFolderPath(TEXT("Harness"));
				Cam->MarkPackageDirty();

				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("kind"), TEXT("camera"));
				Row->SetStringField(TEXT("name"), Cam->GetActorNameOrLabel());
				SpawnedActors.Add(MakeShared<FJsonValueObject>(Row));
			}
		}
	}

	// 4. Target points (also feed nav validation).
	TArray<TSharedPtr<FJsonValue>> NavPoints;
	{
		const TArray<TSharedPtr<FJsonValue>>* TpArr = nullptr;
		if (Params->TryGetArrayField(TEXT("target_points"), TpArr) && TpArr)
		{
			for (const TSharedPtr<FJsonValue>& Val : *TpArr)
			{
				const TSharedPtr<FJsonObject> TpObj = Val.IsValid() ? Val->AsObject() : nullptr;
				if (!TpObj.IsValid())
				{
					continue;
				}
				FString Name;
				TpObj->TryGetStringField(TEXT("name"), Name);
				FVector Loc = FVector::ZeroVector;
				ParseVec3(TpObj, TEXT("location"), Loc);

				ATargetPoint* Tp = World->SpawnActor<ATargetPoint>(ATargetPoint::StaticClass(), Loc, FRotator::ZeroRotator, SpawnParams);
				if (Tp)
				{
					if (!Name.IsEmpty()) { Tp->SetActorLabel(Name); }
					Tp->SetFolderPath(TEXT("Harness/Targets"));
					Tp->MarkPackageDirty();

					TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("kind"), TEXT("target_point"));
					Row->SetStringField(TEXT("name"), Tp->GetActorNameOrLabel());
					SpawnedActors.Add(MakeShared<FJsonValueObject>(Row));

					// Mirror into the nav-validation point list.
					TSharedPtr<FJsonObject> NavPt = MakeShared<FJsonObject>();
					NavPt->SetStringField(TEXT("name"), Name.IsEmpty() ? Tp->GetActorNameOrLabel() : Name);
					NavPt->SetArrayField(TEXT("location"), Vec3ToJson(Loc));
					NavPoints.Add(MakeShared<FJsonValueObject>(NavPt));
				}
			}
		}
	}

	// 5. Actor instances with BP class paths + reflective UPROPERTY defaults.
	TArray<TSharedPtr<FJsonValue>> ActorReports;
	{
		const TArray<TSharedPtr<FJsonValue>>* ActorsArr = nullptr;
		if (Params->TryGetArrayField(TEXT("actors"), ActorsArr) && ActorsArr)
		{
			for (const TSharedPtr<FJsonValue>& Val : *ActorsArr)
			{
				const TSharedPtr<FJsonObject> ActorObj = Val.IsValid() ? Val->AsObject() : nullptr;
				if (!ActorObj.IsValid())
				{
					continue;
				}
				FString ClassPath;
				if (!ActorObj->TryGetStringField(TEXT("class"), ClassPath) || ClassPath.IsEmpty())
				{
					continue;
				}

				UClass* ActorClass = LoadClass<AActor>(nullptr, *ClassPath);
				if (!ActorClass)
				{
					// Tolerate _C-suffix omission by trying the generated-class path.
					ActorClass = LoadObject<UClass>(nullptr, *ClassPath);
				}

				TSharedPtr<FJsonObject> ActorRow = MakeShared<FJsonObject>();
				ActorRow->SetStringField(TEXT("class"), ClassPath);
				if (!ActorClass)
				{
					ActorRow->SetBoolField(TEXT("spawned"), false);
					ActorRow->SetStringField(TEXT("error"), TEXT("could not resolve actor class"));
					ActorReports.Add(MakeShared<FJsonValueObject>(ActorRow));
					continue;
				}

				FVector Loc = FVector::ZeroVector;
				FVector Rot = FVector::ZeroVector;
				ParseVec3(ActorObj, TEXT("location"), Loc);
				ParseVec3(ActorObj, TEXT("rotation"), Rot);

				AActor* Spawned = World->SpawnActor<AActor>(ActorClass, Loc,
					FRotator(Rot.X, Rot.Y, Rot.Z), SpawnParams);
				if (!Spawned)
				{
					ActorRow->SetBoolField(TEXT("spawned"), false);
					ActorRow->SetStringField(TEXT("error"), TEXT("SpawnActor returned null"));
					ActorReports.Add(MakeShared<FJsonValueObject>(ActorRow));
					continue;
				}

				FString Folder = TEXT("Harness/Actors");
				ActorObj->TryGetStringField(TEXT("folder"), Folder);
				Spawned->SetFolderPath(FName(*Folder));

				TArray<FString> Applied, Skipped;
				const TSharedPtr<FJsonObject>* PropObj = nullptr;
				if (ActorObj->TryGetObjectField(TEXT("properties"), PropObj) && PropObj)
				{
					ApplyProperties(Spawned, *PropObj, Applied, Skipped);
				}
				Spawned->MarkPackageDirty();

				ActorRow->SetBoolField(TEXT("spawned"), true);
				ActorRow->SetStringField(TEXT("name"), Spawned->GetActorNameOrLabel());
				ActorRow->SetNumberField(TEXT("properties_applied"), Applied.Num());
				if (Skipped.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> SkippedJson;
					for (const FString& S : Skipped) { SkippedJson.Add(MakeShared<FJsonValueString>(S)); }
					ActorRow->SetArrayField(TEXT("properties_skipped"), SkippedJson);
				}
				ActorReports.Add(MakeShared<FJsonValueObject>(ActorRow));
			}
		}
	}

	// 6. Nav bounds — default sized to the floor footprint unless overridden.
	{
		FVector NavLoc = FVector::ZeroVector;
		FVector NavExtent(3000.0f, 3000.0f, 500.0f);
		const TSharedPtr<FJsonObject>* NavObj = nullptr;
		if (Params->TryGetObjectField(TEXT("nav_bounds"), NavObj) && NavObj)
		{
			ParseVec3(*NavObj, TEXT("location"), NavLoc);
			ParseVec3(*NavObj, TEXT("extent"), NavExtent);
		}

		TSharedPtr<FJsonObject> NavParams = MakeShared<FJsonObject>();
		NavParams->SetArrayField(TEXT("location"), Vec3ToJson(NavLoc));
		NavParams->SetArrayField(TEXT("extent"), Vec3ToJson(NavExtent));
		NavParams->SetStringField(TEXT("folder_path"), TEXT("Harness/Navigation"));
		const FMonolithActionResult NavRes = Registry.ExecuteAction(TEXT("ai"), TEXT("add_nav_bounds_volume"), NavParams);
		Result->SetBoolField(TEXT("nav_bounds_added"), NavRes.bSuccess);
		if (!NavRes.bSuccess)
		{
			Result->SetStringField(TEXT("nav_bounds_error"), NavRes.ErrorMessage);
		}
	}

	// 7. Rebuild navigation (delegated to ai.rebuild_navigation, which bound-waits
	//    for async tile generation).
	{
		double NavTimeout = 30.0;
		if (Params->HasField(TEXT("nav_timeout"))) { NavTimeout = Params->GetNumberField(TEXT("nav_timeout")); }

		TSharedPtr<FJsonObject> RebuildParams = MakeShared<FJsonObject>();
		RebuildParams->SetBoolField(TEXT("save_after"), false); // we save the level explicitly below
		RebuildParams->SetNumberField(TEXT("timeout_seconds"), NavTimeout);
		const FMonolithActionResult RebuildRes = Registry.ExecuteAction(TEXT("ai"), TEXT("rebuild_navigation"), RebuildParams);
		Result->SetBoolField(TEXT("nav_rebuilt"), RebuildRes.bSuccess);
		if (RebuildRes.bSuccess && RebuildRes.Result.IsValid())
		{
			Result->SetObjectField(TEXT("nav_rebuild"), RebuildRes.Result);
		}
		else if (!RebuildRes.bSuccess)
		{
			Result->SetStringField(TEXT("nav_rebuild_error"), RebuildRes.ErrorMessage);
		}
	}

	// 8. Validate nav points + requested path pairs (delegated to ai.validate_nav_points).
	if (NavPoints.Num() > 0)
	{
		TSharedPtr<FJsonObject> ValidateParams = MakeShared<FJsonObject>();
		ValidateParams->SetArrayField(TEXT("points"), NavPoints);
		const TArray<TSharedPtr<FJsonValue>>* PairsArr = nullptr;
		if (Params->TryGetArrayField(TEXT("validate_pairs"), PairsArr) && PairsArr)
		{
			ValidateParams->SetArrayField(TEXT("require_path_pairs"), *PairsArr);
		}
		const FMonolithActionResult ValidateRes = Registry.ExecuteAction(TEXT("ai"), TEXT("validate_nav_points"), ValidateParams);
		Result->SetBoolField(TEXT("nav_validated"), ValidateRes.bSuccess);
		if (ValidateRes.Result.IsValid())
		{
			Result->SetObjectField(TEXT("nav_validation"), ValidateRes.Result);
		}
		else if (!ValidateRes.bSuccess)
		{
			Result->SetStringField(TEXT("nav_validation_error"), ValidateRes.ErrorMessage);
		}
	}

	// 9. Save the level package now that actors + nav exist.
	{
		TSharedPtr<FJsonObject> SaveParams = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> PkgArr;
		PkgArr.Add(MakeShared<FJsonValueString>(MapPath));
		SaveParams->SetArrayField(TEXT("packages"), PkgArr);
		const FMonolithActionResult SaveRes = Registry.ExecuteAction(TEXT("editor"), TEXT("save_packages"), SaveParams);
		Result->SetBoolField(TEXT("saved"), SaveRes.bSuccess);
		if (!SaveRes.bSuccess)
		{
			Result->SetStringField(TEXT("save_error"), SaveRes.ErrorMessage);
		}
	}

	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("path"), MapPath);
	Result->SetArrayField(TEXT("spawned_actors"), SpawnedActors);
	Result->SetArrayField(TEXT("actor_instances"), ActorReports);
	return FMonolithActionResult::Success(Result);
}

#include "MonolithPieSmokeSession.h"
#include "MonolithEditorActions.h" // FMonolithLogCapture / FMonolithLogEntry

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
#include "EngineUtils.h"

#include "UnrealClient.h"        // FViewport (PIE frame capture)
#include "ImageUtils.h"          // FImageUtils::SaveImageAutoFormat
#include "ImageCore.h"           // FImage / ERawImageFormat / EGammaSpace
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithPieSmoke, Log, All);

namespace
{
	// Locate the active PIE world context's UWorld (or nullptr). Self-contained copy
	// of the actions-module helper so the manager carries no cross-TU dependency.
	UWorld* FindActivePieWorld()
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

	// Read the pawn's AnimInstance (via its SkeletalMeshComponent) or nullptr.
	UAnimInstance* PawnAnimInstance(APawn* Pawn)
	{
		if (!Pawn)
		{
			return nullptr;
		}
		if (USkeletalMeshComponent* SkelComp = Pawn->FindComponentByClass<USkeletalMeshComponent>())
		{
			return SkelComp->GetAnimInstance();
		}
		return nullptr;
	}

	// True if the pawn's AnimInstance exposes at least one of the requested variables.
	bool PawnHasAnyVar(APawn* Pawn, const TArray<FString>& VarNames)
	{
		if (VarNames.Num() == 0)
		{
			return false;
		}
		UAnimInstance* Anim = PawnAnimInstance(Pawn);
		if (!Anim || !Anim->GetClass())
		{
			return false;
		}
		for (const FString& VarName : VarNames)
		{
			if (Anim->GetClass()->FindPropertyByName(FName(*VarName)))
			{
				return true;
			}
		}
		return false;
	}

	// Resolve the pawn the session samples, in priority order:
	//   (a) class-name-filtered pawn, when a filter is set;
	//   (b) else, a pawn whose AnimInstance actually exposes one of the requested vars
	//       (so the default player pawn's ABP not carrying them is skipped);
	//   (c) else, the first player controller's pawn.
	APawn* ResolveTargetPawn(UWorld* PieWorld, const FString& ClassFilter,
		const TArray<FString>& VarNames)
	{
		if (!PieWorld)
		{
			return nullptr;
		}
		if (!ClassFilter.IsEmpty())
		{
			for (TActorIterator<APawn> It(PieWorld); It; ++It)
			{
				APawn* Candidate = *It;
				if (Candidate && Candidate->GetClass() &&
					Candidate->GetClass()->GetName().Contains(ClassFilter))
				{
					return Candidate;
				}
			}
			return nullptr;
		}
		if (VarNames.Num() > 0)
		{
			for (TActorIterator<APawn> It(PieWorld); It; ++It)
			{
				APawn* Candidate = *It;
				if (PawnHasAnyVar(Candidate, VarNames))
				{
					return Candidate;
				}
			}
		}
		if (APlayerController* PC = PieWorld->GetFirstPlayerController())
		{
			return PC->GetPawn();
		}
		return nullptr;
	}

	// Read one named float/double/bool AnimInstance variable reflectively. Returns false
	// if the property is absent / unsupported (different ABPs expose different vars).
	bool ReadAnimVar(UAnimInstance* Anim, const FString& VarName, FPieSmokeSampleVar& OutVar)
	{
		if (!Anim)
		{
			return false;
		}
		FProperty* Prop = Anim->GetClass()->FindPropertyByName(FName(*VarName));
		if (!Prop)
		{
			return false;
		}
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Anim);
		OutVar.Name = VarName;
		if (const FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
		{
			OutVar.bIsBool = false;
			OutVar.NumberValue = FloatProp->GetPropertyValue(ValuePtr);
			return true;
		}
		if (const FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
		{
			OutVar.bIsBool = false;
			OutVar.NumberValue = DoubleProp->GetPropertyValue(ValuePtr);
			return true;
		}
		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			OutVar.bIsBool = true;
			OutVar.BoolValue = BoolProp->GetPropertyValue(ValuePtr);
			return true;
		}
		return false;
	}

	// Read the active PIE viewport into a PNG. Returns false (no crash) when the
	// viewport / pixels are unavailable.
	bool CapturePieFrame(const FString& OutputPath)
	{
		FViewport* Viewport = GEditor ? GEditor->GetPIEViewport() : nullptr;
		if (!Viewport)
		{
			return false;
		}
		const FIntPoint Size = Viewport->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0)
		{
			return false;
		}

		TArray<FColor> Pixels;
		if (!Viewport->ReadPixels(Pixels) || Pixels.Num() < Size.X * Size.Y)
		{
			return false;
		}
		for (FColor& Px : Pixels)
		{
			Px.A = 255; // viewport reads can carry scene-depth alpha noise
		}

		const FString Dir = FPaths::GetPath(OutputPath);
		IFileManager::Get().MakeDirectory(*Dir, true);

		FImage Image;
		Image.Init(Size.X, Size.Y, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		FMemory::Memcpy(Image.RawData.GetData(), Pixels.GetData(), Size.X * Size.Y * sizeof(FColor));
		return FImageUtils::SaveImageAutoFormat(*OutputPath, Image);
	}
}

FPieSmokeSessionManager& FPieSmokeSessionManager::Get()
{
	static FPieSmokeSessionManager Instance;
	return Instance;
}

FString FPieSmokeSessionManager::CreateSession(FPieSmokeSession&& Session)
{
	if (Session.Id.IsEmpty())
	{
		Session.Id = FString::Printf(TEXT("pie_smoke_%u_%s"),
			NextSessionSerial++,
			*FDateTime::Now().ToString(TEXT("%H%M%S")));
	}
	const FString Id = Session.Id;
	Sessions.Add(Id, MoveTemp(Session));
	EnsureObserver();
	return Id;
}

FPieSmokeSession* FPieSmokeSessionManager::Find(const FString& SessionId)
{
	return Sessions.Find(SessionId);
}

int32 FPieSmokeSessionManager::Stop(const FString& SessionId)
{
	int32 Stopped = 0;
	bool bAnyRunning = false;

	for (TPair<FString, FPieSmokeSession>& Pair : Sessions)
	{
		FPieSmokeSession& S = Pair.Value;
		if (SessionId.IsEmpty() || Pair.Key == SessionId)
		{
			if (S.Status == EPieSmokeStatus::Running)
			{
				S.Status = EPieSmokeStatus::Stopped;
				S.LastObservedSeconds = FPlatformTime::Seconds();
				++Stopped;
			}
		}
	}

	// End PIE if no session still wants it running. We only drive RequestEndPlayMap
	// when nothing Running remains, so concurrent sessions (rare) aren't cut short.
	for (const TPair<FString, FPieSmokeSession>& Pair : Sessions)
	{
		if (Pair.Value.Status == EPieSmokeStatus::Running)
		{
			bAnyRunning = true;
			break;
		}
	}
	if (!bAnyRunning && GEditor && FindActivePieWorld())
	{
		GEditor->RequestEndPlayMap();
	}
	return Stopped;
}

bool FPieSmokeSessionManager::HasRunningSessions() const
{
	for (const TPair<FString, FPieSmokeSession>& Pair : Sessions)
	{
		if (Pair.Value.Status == EPieSmokeStatus::Running)
		{
			return true;
		}
	}
	return false;
}

void FPieSmokeSessionManager::EnsureObserver()
{
	if (bObserverActive)
	{
		return;
	}
	bObserverActive = true;

	// Single shared frame observer. Runs as part of the editor's REAL frame (after the
	// frame's BeginFrame/dynamic-resolution bracket is already balanced), so it is NOT
	// re-entrant and may READ world/PIE/actor state freely. It MUST NOT call
	// World->Tick / GEditor->Tick / ProcessAsyncLoading — the engine advances those.
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		TEXT("MonolithPieSmokeObserver"), 0.0f,
		[](float DeltaTime) -> bool
		{
			return FPieSmokeSessionManager::Get().OnFrameTick(DeltaTime);
		});

	// Mark sessions inactive the instant PIE ends (crash / manual stop / completion) so
	// the observer never dereferences a torn-down PIE world.
	EndPieHandle = FEditorDelegates::EndPIE.AddRaw(this, &FPieSmokeSessionManager::OnPieEnded);
	PrePieEndedHandle = FEditorDelegates::PrePIEEnded.AddRaw(this, &FPieSmokeSessionManager::OnPieEnded);
}

void FPieSmokeSessionManager::TeardownObserverIfIdle()
{
	if (HasRunningSessions())
	{
		return; // leave the observer installed while any session still runs
	}
	if (!bObserverActive)
	{
		return;
	}
	bObserverActive = false;

	if (TickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	if (EndPieHandle.IsValid())
	{
		FEditorDelegates::EndPIE.Remove(EndPieHandle);
		EndPieHandle.Reset();
	}
	if (PrePieEndedHandle.IsValid())
	{
		FEditorDelegates::PrePIEEnded.Remove(PrePieEndedHandle);
		PrePieEndedHandle.Reset();
	}
}

void FPieSmokeSessionManager::OnPieEnded(const bool /*bIsSimulating*/)
{
	// PIE is going away — every session loses its world. Mark inactive so the next
	// observer tick stops sampling. A Running session whose PIE ended before its
	// duration elapsed is finalised as Complete on the next tick (bPieActive == false).
	for (TPair<FString, FPieSmokeSession>& Pair : Sessions)
	{
		Pair.Value.bPieActive = false;
	}
}

bool FPieSmokeSessionManager::OnFrameTick(float /*DeltaTime*/)
{
	UWorld* PieWorld = FindActivePieWorld();

	for (TPair<FString, FPieSmokeSession>& Pair : Sessions)
	{
		FPieSmokeSession& S = Pair.Value;
		if (S.Status != EPieSmokeStatus::Running)
		{
			continue;
		}

		const double Now = FPlatformTime::Seconds();
		S.LastObservedSeconds = Now;
		const double Elapsed = Now - S.StartTimeSeconds;

		// PIE gone (ended / crashed / manually stopped) before duration elapsed.
		if (!S.bPieActive || !PieWorld || !IsValid(PieWorld))
		{
			S.Status = EPieSmokeStatus::Complete;
			continue;
		}

		// World up but not yet begun play — wait for the next frame.
		if (!PieWorld->HasBegunPlay())
		{
			continue;
		}
		S.bReady = true;

		AdvanceSession(S);

		if (Elapsed >= S.DurationSeconds)
		{
			S.Status = EPieSmokeStatus::Complete;
		}
	}

	// Stop PIE once no session needs it (and the world is still up).
	if (!HasRunningSessions())
	{
		if (GEditor && FindActivePieWorld())
		{
			GEditor->RequestEndPlayMap();
		}
		TeardownObserverIfIdle();
		return false; // self-unregister: no work left
	}
	return true;
}

void FPieSmokeSessionManager::AdvanceSession(FPieSmokeSession& Session)
{
	UWorld* PieWorld = FindActivePieWorld();
	if (!PieWorld)
	{
		return;
	}

	// (Re)resolve the tracked pawn — pawns (and their AnimInstances) can spawn a frame
	// or two after BeginPlay, so keep retrying until a valid one is cached.
	APawn* Pawn = Session.TargetPawn.IsValid() ? Session.TargetPawn.Get() : nullptr;
	if (!Pawn)
	{
		Pawn = ResolveTargetPawn(PieWorld, Session.PawnClassFilter, Session.SampleVarNames);
		Session.TargetPawn = Pawn;
	}

	const double SampleTime = FPlatformTime::Seconds() - Session.StartTimeSeconds;

	FPieSmokeSample Sample;
	Sample.TimeSeconds = SampleTime;

	if (Pawn)
	{
		if (USkeletalMeshComponent* SkelComp = Pawn->FindComponentByClass<USkeletalMeshComponent>())
		{
			if (UAnimInstance* Anim = SkelComp->GetAnimInstance())
			{
				for (const FString& VarName : Session.SampleVarNames)
				{
					FPieSmokeSampleVar Var;
					if (ReadAnimVar(Anim, VarName, Var))
					{
						Sample.Vars.Add(MoveTemp(Var));
					}
				}
			}
		}
	}

	// Clip variant: capture a viewport frame at most once per CaptureInterval.
	if (Session.bCaptureFrames && !Session.bCaptureDeferred)
	{
		const bool bDue = (Session.LastCaptureSeconds < 0.0) ||
			(SampleTime - Session.LastCaptureSeconds >= Session.CaptureInterval);
		if (bDue)
		{
			const FString FramePath = Session.OutputDir /
				FString::Printf(TEXT("frame_%03d.png"), Session.CaptureFrameIndex);
			if (CapturePieFrame(FramePath))
			{
				Sample.FramePath = FramePath;
				Session.LastCaptureSeconds = SampleTime;
				++Session.CaptureFrameIndex;
			}
			else if (Session.CaptureFrameIndex == 0 && SampleTime > Session.CaptureInterval * 2.0)
			{
				// Viewport never produced pixels — flag clip capture deferred but keep
				// the session running so anim sampling + log counts still complete.
				Session.bCaptureDeferred = true;
				UE_LOG(LogMonolithPieSmoke, Warning,
					TEXT("capture_pie_movement_clip: PIE viewport unavailable for session %s — capture deferred."),
					*Session.Id);
			}
		}
	}

	Session.Samples.Add(MoveTemp(Sample));
}

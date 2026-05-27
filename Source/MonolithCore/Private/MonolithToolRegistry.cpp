#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "HAL/PlatformMisc.h"

// =============================================================================
//  FMonolithParamSchema — K2 alias rewriting + K3 unknown-key detection
// =============================================================================

bool FMonolithParamSchema::ApplyAliases(
	const TSharedPtr<FJsonObject>& Schema,
	const TSharedPtr<FJsonObject>& Params,
	FString& OutCollision)
{
	if (!Schema.IsValid() || !Params.IsValid())
	{
		return true;
	}

	for (const auto& Pair : Schema->Values)
	{
		const FString& Canonical = Pair.Key;

		const TSharedPtr<FJsonObject>* ParamDef = nullptr;
		if (!Pair.Value->TryGetObject(ParamDef) || !ParamDef)
		{
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>* AliasArr = nullptr;
		if (!(*ParamDef)->TryGetArrayField(TEXT("aliases"), AliasArr) || !AliasArr)
		{
			continue;
		}

		const bool bCanonicalPresent = Params->HasField(Canonical);

		for (const TSharedPtr<FJsonValue>& AliasVal : *AliasArr)
		{
			FString Alias;
			if (!AliasVal.IsValid() || !AliasVal->TryGetString(Alias))
			{
				continue;
			}

			if (!Params->HasField(Alias))
			{
				continue;
			}

			if (bCanonicalPresent)
			{
				OutCollision = FString::Printf(
					TEXT("Param collision: both canonical '%s' and alias '%s' supplied. Use only one. — supply either the canonical param OR its alias, never both."),
					*Canonical, *Alias);
				return false;
			}

			// Rewrite alias -> canonical (preserve value).
			TSharedPtr<FJsonValue> Val = Params->TryGetField(Alias);
			if (Val.IsValid())
			{
				Params->SetField(Canonical, Val);
			}
			Params->RemoveField(Alias);
			break; // Only one alias rewrite per canonical.
		}
	}

	return true;
}

TArray<FString> FMonolithParamSchema::FindUnknownKeys(
	const TSharedPtr<FJsonObject>& Schema,
	const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> Unknown;
	if (!Schema.IsValid() || !Params.IsValid())
	{
		return Unknown;
	}

	// Build the set of allowed keys: canonical names + their declared aliases.
	TSet<FString> Allowed;
	for (const auto& Pair : Schema->Values)
	{
		Allowed.Add(Pair.Key);

		const TSharedPtr<FJsonObject>* ParamDef = nullptr;
		if (!Pair.Value->TryGetObject(ParamDef) || !ParamDef)
		{
			continue;
		}
		const TArray<TSharedPtr<FJsonValue>>* AliasArr = nullptr;
		if ((*ParamDef)->TryGetArrayField(TEXT("aliases"), AliasArr) && AliasArr)
		{
			for (const TSharedPtr<FJsonValue>& AV : *AliasArr)
			{
				FString A;
				if (AV.IsValid() && AV->TryGetString(A))
				{
					Allowed.Add(A);
				}
			}
		}
	}

	// Legacy wbp_path/asset_path back-compat: allow asset_path everywhere.
	Allowed.Add(TEXT("asset_path"));

	// Survivor B (plan §3.B) — universal response-shaping params. Allow these
	// on EVERY action so the K3 STRICT_PARAMS=1 path does not hard-fail on
	// `_fields` / `_omit` / `_compact_json`. The post-filter at the bottom
	// of ExecuteAction consumes + acts on them.
	Allowed.Add(TEXT("_fields"));
	Allowed.Add(TEXT("_omit"));
	Allowed.Add(TEXT("_compact_json"));

	for (const auto& Pair : Params->Values)
	{
		if (!Allowed.Contains(Pair.Key))
		{
			Unknown.Add(Pair.Key);
		}
	}

	return Unknown;
}

bool FMonolithParamSchema::IsStrictParamsEnabled()
{
	const FString Val = FPlatformMisc::GetEnvironmentVariable(TEXT("STRICT_PARAMS"));
	return Val == TEXT("1");
}

// =============================================================================
//  FMonolithToolRegistry
// =============================================================================

FMonolithToolRegistry& FMonolithToolRegistry::Get()
{
	static FMonolithToolRegistry Instance;
	return Instance;
}

void FMonolithToolRegistry::RegisterAction(
	const FString& Namespace,
	const FString& Action,
	const FString& Description,
	const FMonolithActionHandler& Handler,
	const TSharedPtr<FJsonObject>& ParamSchema,
	const FString& Category)
{
	FScopeLock Lock(&RegistryLock);

	FString Key = MakeKey(Namespace, Action);

	if (Actions.Contains(Key))
	{
		UE_LOG(LogMonolith, Warning, TEXT("Overwriting existing action: %s"), *Key);
	}

	FRegisteredAction RegAction;
	RegAction.Info.Namespace = Namespace;
	RegAction.Info.Action = Action;
	RegAction.Info.Description = Description;
	RegAction.Info.Category = Category;
	RegAction.Info.ParamSchema = ParamSchema;
	RegAction.Handler = Handler;

	Actions.Add(Key, MoveTemp(RegAction));
	NamespaceActions.FindOrAdd(Namespace).AddUnique(Key);

	UE_LOG(LogMonolith, Verbose, TEXT("Registered action: %s — %s"), *Key, *Description);
}

void FMonolithToolRegistry::UnregisterNamespace(const FString& Namespace)
{
	FScopeLock Lock(&RegistryLock);

	if (TArray<FString>* Keys = NamespaceActions.Find(Namespace))
	{
		for (const FString& Key : *Keys)
		{
			Actions.Remove(Key);
		}
		UE_LOG(LogMonolith, Log, TEXT("Unregistered namespace: %s (%d actions)"), *Namespace, Keys->Num());
		NamespaceActions.Remove(Namespace);
	}
}

FMonolithActionResult FMonolithToolRegistry::ExecuteAction(
	const FString& Namespace,
	const FString& Action,
	const TSharedPtr<FJsonObject>& Params)
{
	FScopeLock Lock(&RegistryLock);

	FString Key = MakeKey(Namespace, Action);
	FRegisteredAction* RegAction = Actions.Find(Key);

	if (!RegAction)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Unknown action: %s.%s — call monolith_discover(\"%s\") to enumerate valid actions in this namespace."), *Namespace, *Action, *Namespace),
			FMonolithJsonUtils::ErrMethodNotFound
		);
	}

	if (!RegAction->Handler.IsBound())
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Action handler not bound: %s — this is a Monolith bug; the action is registered but its handler delegate is null. Report at github.com/tumourlove/monolith."), *Key),
			FMonolithJsonUtils::ErrInternalError
		);
	}

	const FMonolithActionInfo& ActionInfo = RegAction->Info;
	TSharedPtr<FJsonObject> EffectiveParams = Params.IsValid() ? Params : MakeShared<FJsonObject>();

	// K2 — alias rewriting BEFORE the required-param check.
	if (ActionInfo.ParamSchema.IsValid())
	{
		FString Collision;
		if (!FMonolithParamSchema::ApplyAliases(ActionInfo.ParamSchema, EffectiveParams, Collision))
		{
			return FMonolithActionResult::Error(Collision, FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	// Validate required params from schema before dispatching.
	// Skip asset_path — GetAssetPath() accepts both asset_path and system_path aliases
	// and produces a clear error message itself.
	if (ActionInfo.ParamSchema.IsValid())
	{
		TArray<FString> Missing;
		for (const auto& Pair : ActionInfo.ParamSchema->Values)
		{
			if (Pair.Key == TEXT("asset_path")) continue;

			const TSharedPtr<FJsonObject>* ParamDef = nullptr;
			if (Pair.Value->TryGetObject(ParamDef) && ParamDef)
			{
				bool bRequired = false;
				(*ParamDef)->TryGetBoolField(TEXT("required"), bRequired);
				if (bRequired && !EffectiveParams->HasField(Pair.Key))
				{
					// Legacy wbp_path / asset_path aliasing: accept asset_path as substitute for wbp_path
					// (only fires for schemas not migrated to K2 aliases).
					if (Pair.Key == TEXT("wbp_path") && EffectiveParams->HasField(TEXT("asset_path")))
						continue;
					Missing.Add(Pair.Key);
				}
			}
		}
		if (Missing.Num() > 0)
		{
			TArray<FString> Provided;
			for (const auto& P : EffectiveParams->Values) Provided.Add(P.Key);
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Missing required param(s): [%s]. Provided keys: [%s] — inspect the action's parameter schema via monolith_discover(\"<namespace>\") and supply all required fields."),
					*FString::Join(Missing, TEXT(", ")),
					*FString::Join(Provided, TEXT(", "))));
		}
	}

	// Survivor D (plan §3.D) — schema-tagged AssetPath \→/ rewrite.
	// Runs AFTER K2 alias rewrite (so the key the schema sees matches the
	// param name in EffectiveParams) and BEFORE K3 unknown-key check.
	// Only Kind == AssetPath is rewritten; all other Kinds (Other, DiskPath,
	// GameplayTag) pass through. Warnings appended to the same K3 warnings[]
	// channel by the post-handler block below.
	TArray<FString> AssetPathRewriteWarnings;
	if (ActionInfo.ParamSchema.IsValid())
	{
		for (const auto& SchemaPair : ActionInfo.ParamSchema->Values)
		{
			const TSharedPtr<FJsonObject>* ParamDefPtr = nullptr;
			if (!SchemaPair.Value->TryGetObject(ParamDefPtr) || !ParamDefPtr || !ParamDefPtr->IsValid())
			{
				continue;
			}
			FString KindStr;
			if (!(*ParamDefPtr)->TryGetStringField(TEXT("kind"), KindStr))
			{
				continue; // No kind tag → Other (default) → no rewrite.
			}
			if (MonolithParamKind::FromString(KindStr) != EMonolithParamKind::AssetPath)
			{
				continue;
			}

			const FString& ParamName = SchemaPair.Key;
			FString Value;
			if (!EffectiveParams->TryGetStringField(ParamName, Value))
			{
				continue;
			}
			if (!Value.Contains(TEXT("\\")))
			{
				continue;
			}

			FString Rewritten = Value.Replace(TEXT("\\"), TEXT("/"));
			EffectiveParams->SetStringField(ParamName, Rewritten);
			AssetPathRewriteWarnings.Add(FString::Printf(
				TEXT("Normalised backslashes in 'asset_path' param '%s' — future calls should use forward slashes."),
				*ParamName));
		}
	}

	// K3 — unknown-key detection (after required-check, before dispatch).
	TArray<FString> Unknown;
	if (ActionInfo.ParamSchema.IsValid())
	{
		Unknown = FMonolithParamSchema::FindUnknownKeys(ActionInfo.ParamSchema, EffectiveParams);

		if (Unknown.Num() > 0)
		{
			for (const FString& K : Unknown)
			{
				UE_LOG(LogMonolith, Warning,
					TEXT("Unknown param '%s' for action '%s:%s' (typo? not in schema)"),
					*K, *Namespace, *Action);
			}

			if (FMonolithParamSchema::IsStrictParamsEnabled())
			{
				return FMonolithActionResult::Error(
					FString::Printf(TEXT("STRICT_PARAMS=1: rejected action '%s:%s' due to unknown params: [%s] — unset STRICT_PARAMS or remove the unknown params from the call."),
						*Namespace, *Action, *FString::Join(Unknown, TEXT(", "))),
					FMonolithJsonUtils::ErrInvalidParams);
			}
		}
	}

	// Release lock before executing handler (handlers may take time)
	FMonolithActionHandler HandlerCopy = RegAction->Handler;
	Lock.Unlock();

	FMonolithActionResult ActionResult = HandlerCopy.Execute(EffectiveParams);

	// Collect ALL post-handler warnings into a single channel, then attach once.
	// Sources, in order:
	//   1. K3 unknown-param soft-warn (pre-existing behaviour)
	//   2. Survivor D — AssetPath \→/ rewrite warnings (plan §3.D)
	//   3. Survivor B — response-shaping warnings (plan §3.B, e.g. _fields/_omit collision)
	if (ActionResult.bSuccess && ActionResult.Result.IsValid())
	{
		TArray<FString> AllWarnings;
		AllWarnings.Append(AssetPathRewriteWarnings);
		for (const FString& K : Unknown)
		{
			AllWarnings.Add(FString::Printf(TEXT("Unknown param '%s' for action '%s:%s'"), *K, *Namespace, *Action));
		}

		// Survivor B post-filter — mutates ActionResult.Result in-place and may
		// append its own warnings (e.g., mutually-exclusive _fields + _omit).
		// Runs BEFORE attaching the warnings array so its warnings get included
		// in the final emit; runs AFTER warning collection so the filter cannot
		// strip the warnings[] key out from under us via _fields whitelist.
		// (We attach warnings to ActionResult.Result AFTER ApplyResponseShaping.)
		ApplyResponseShaping(ActionResult.Result, EffectiveParams, AllWarnings);

		if (AllWarnings.Num() > 0 && ActionResult.Result.IsValid())
		{
			TArray<TSharedPtr<FJsonValue>> Existing;
			const TArray<TSharedPtr<FJsonValue>>* Found = nullptr;
			if (ActionResult.Result->TryGetArrayField(TEXT("warnings"), Found) && Found)
			{
				Existing = *Found;
			}
			for (const FString& W : AllWarnings)
			{
				Existing.Add(MakeShared<FJsonValueString>(W));
			}
			ActionResult.Result->SetArrayField(TEXT("warnings"), Existing);
		}
	}

	return ActionResult;
}

TArray<FString> FMonolithToolRegistry::GetNamespaces() const
{
	FScopeLock Lock(&RegistryLock);
	TArray<FString> Result;
	NamespaceActions.GetKeys(Result);
	return Result;
}

TArray<FMonolithActionInfo> FMonolithToolRegistry::GetActions(const FString& Namespace) const
{
	FScopeLock Lock(&RegistryLock);
	TArray<FMonolithActionInfo> Result;

	if (const TArray<FString>* Keys = NamespaceActions.Find(Namespace))
	{
		for (const FString& Key : *Keys)
		{
			if (const FRegisteredAction* RegAction = Actions.Find(Key))
			{
				Result.Add(RegAction->Info);
			}
		}
	}
	return Result;
}

TArray<FMonolithActionInfo> FMonolithToolRegistry::GetAllActions() const
{
	FScopeLock Lock(&RegistryLock);
	TArray<FMonolithActionInfo> Result;
	for (const auto& Pair : Actions)
	{
		Result.Add(Pair.Value.Info);
	}
	return Result;
}

bool FMonolithToolRegistry::HasAction(const FString& Namespace, const FString& Action) const
{
	FScopeLock Lock(&RegistryLock);
	return Actions.Contains(MakeKey(Namespace, Action));
}

int32 FMonolithToolRegistry::GetActionCount() const
{
	FScopeLock Lock(&RegistryLock);
	return Actions.Num();
}

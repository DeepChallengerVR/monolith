#include "MonolithChooserActions.h"
#include "MonolithParamSchema.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_CHOOSER
#include "MonolithAssetUtils.h"

// Chooser runtime headers. Chooser.Build.cs adds its Internal/ dir to
// PublicIncludePaths, so the Internal table/result headers are reachable for any
// module taking the "Chooser" dependency.
#include "Chooser.h"                 // UChooserTable (Internal, on public include path)
#include "ChooserSignature.h"        // UChooserSignature (Public)
#include "ChooserPropertyAccess.h"   // FContextObjectTypeBase/Class/Struct (Public)
#include "ObjectChooser_Asset.h"     // FAssetChooser / FSoftAssetChooser (Internal, public path)

#include "StructUtils/InstancedStruct.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/SoftObjectPath.h"

// DuplicateAsset (EditorScriptingUtilities — already a MonolithAnimation dep).
#include "EditorAssetLibrary.h"
#endif // WITH_CHOOSER

// ---------------------------------------------------------------------------
// Registration (always registers; per-handler gating below)
// ---------------------------------------------------------------------------

void FMonolithChooserActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// --- inspect_chooser ---
	Registry.RegisterAction(TEXT("chooser"), TEXT("inspect_chooser"),
		TEXT("Read-only inspection of a UChooserTable: context-data parameters (class/struct requirements), result type + result class, row count, column count + types, referenced assets walked from result rows, and compile/validation status."),
		FMonolithActionHandler::CreateStatic(&HandleInspectChooser),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("UChooserTable asset path"))
			.Build());

	// --- duplicate_chooser_tree ---
	Registry.RegisterAction(TEXT("chooser"), TEXT("duplicate_chooser_tree"),
		TEXT("Duplicate one or more chooser tables into a destination folder (sources are never mutated). Optionally remap RootChooser/ParentTable/NestedChoosers and result asset references per remap_rules (map of old-path -> new-path)."),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateChooserTree),
		FParamSchemaBuilder()
			.Required(TEXT("source_assets"), TEXT("array"), TEXT("Array of source UChooserTable asset paths to duplicate"))
			.Required(TEXT("destination_folder"), TEXT("string"), TEXT("Destination content folder, e.g. /Game/Tests/Monolith/Warband"))
			.Optional(TEXT("remap_rules"), TEXT("object"), TEXT("Optional map of old-asset-path -> new-asset-path applied to RootChooser/ParentTable/NestedChoosers and result FInstancedStruct asset refs in each duplicate"))
			.Build());

	// --- set_context_object_class ---
	Registry.RegisterAction(TEXT("chooser"), TEXT("set_context_object_class"),
		TEXT("Rewrite the Class on a ContextData parameter entry (FContextObjectTypeClass) of a chooser table, e.g. to ABP_Humanoid_C. Marks the package dirty and recompiles (Compile(true))."),
		FMonolithActionHandler::CreateStatic(&HandleSetContextObjectClass),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("UChooserTable asset path"))
			.Required(TEXT("context_name_or_index"), TEXT("string"), TEXT("Index of the ContextData entry to retarget (0-based). A non-numeric value selects the first class-typed context entry."))
			.Required(TEXT("class_path"), TEXT("string"), TEXT("New context object class, e.g. /Game/.../ABP_Humanoid.ABP_Humanoid_C or a loaded class name"))
			.Build());

	// --- set_result_asset_reference ---
	Registry.RegisterAction(TEXT("chooser"), TEXT("set_result_asset_reference"),
		TEXT("Rewrite the Asset reference on a result row (FAssetChooser / FSoftAssetChooser) of a chooser table, e.g. a PoseSearch database. Marks the package dirty and recompiles (Compile(true))."),
		FMonolithActionHandler::CreateStatic(&HandleSetResultAssetReference),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("UChooserTable asset path"))
			.Required(TEXT("row_or_column"), TEXT("number"), TEXT("0-based result row index whose asset reference to rewrite"))
			.Required(TEXT("asset_path_value"), TEXT("string"), TEXT("New asset path to assign to the result row's Asset reference"))
			.Build());

	// --- validate_chooser ---
	Registry.RegisterAction(TEXT("chooser"), TEXT("validate_chooser"),
		TEXT("Compile a chooser table (Compile(true)) and validate it: optional expected context class + expected result type, plus null/stale result-row asset references. Read-only apart from the compile pass."),
		FMonolithActionHandler::CreateStatic(&HandleValidateChooser),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("UChooserTable asset path"))
			.Optional(TEXT("expected_context_class"), TEXT("string"), TEXT("Optional: class expected on at least one ContextData entry"))
			.Optional(TEXT("expected_result_type"), TEXT("string"), TEXT("Optional: expected result type — ObjectResult, ClassResult, or NoPrimaryResult"))
			.Build());
}

// ===========================================================================
// WITH_CHOOSER == 0 : clean off-gate stubs
// ===========================================================================
#if !WITH_CHOOSER

namespace
{
	FMonolithActionResult ChooserUnavailable()
	{
		return FMonolithActionResult::Error(TEXT("Chooser plugin not available"));
	}
}

FMonolithActionResult FMonolithChooserActions::HandleInspectChooser(const TSharedPtr<FJsonObject>&)        { return ChooserUnavailable(); }
FMonolithActionResult FMonolithChooserActions::HandleDuplicateChooserTree(const TSharedPtr<FJsonObject>&)  { return ChooserUnavailable(); }
FMonolithActionResult FMonolithChooserActions::HandleSetContextObjectClass(const TSharedPtr<FJsonObject>&) { return ChooserUnavailable(); }
FMonolithActionResult FMonolithChooserActions::HandleSetResultAssetReference(const TSharedPtr<FJsonObject>&){ return ChooserUnavailable(); }
FMonolithActionResult FMonolithChooserActions::HandleValidateChooser(const TSharedPtr<FJsonObject>&)       { return ChooserUnavailable(); }

#else // WITH_CHOOSER

// ===========================================================================
// Helpers
// ===========================================================================

namespace
{
	/** Human-readable name for the chooser result type enum. */
	FString ResultTypeToString(EObjectChooserResultType Type)
	{
		switch (Type)
		{
		case EObjectChooserResultType::ObjectResult:    return TEXT("ObjectResult");
		case EObjectChooserResultType::ClassResult:     return TEXT("ClassResult");
		case EObjectChooserResultType::NoPrimaryResult: return TEXT("NoPrimaryResult");
		default:                                        return TEXT("Unknown");
		}
	}

	/** Resolve a result type string (case-insensitive) to the enum. Returns false if unrecognized. */
	bool ParseResultType(const FString& Str, EObjectChooserResultType& Out)
	{
		if (Str.Equals(TEXT("ObjectResult"), ESearchCase::IgnoreCase))    { Out = EObjectChooserResultType::ObjectResult;    return true; }
		if (Str.Equals(TEXT("ClassResult"), ESearchCase::IgnoreCase))     { Out = EObjectChooserResultType::ClassResult;     return true; }
		if (Str.Equals(TEXT("NoPrimaryResult"), ESearchCase::IgnoreCase)) { Out = EObjectChooserResultType::NoPrimaryResult; return true; }
		return false;
	}

	/**
	 * Extract the referenced asset (if any) from a result-row FInstancedStruct.
	 * Handles the hard FAssetChooser and soft FSoftAssetChooser struct types.
	 * Returns the asset path, or empty string if the row is not an asset result.
	 * bOutIsNull is set true when the row IS an asset result but the reference is unset.
	 */
	FString GetRowAssetPath(const FInstancedStruct& Row, bool& bOutIsNull, FString& OutStructType)
	{
		bOutIsNull = false;
		OutStructType.Reset();

		const UScriptStruct* SS = Row.GetScriptStruct();
		if (!SS)
		{
			return FString();
		}
		OutStructType = SS->GetName();

		if (const FAssetChooser* Hard = Row.GetPtr<FAssetChooser>())
		{
			if (Hard->Asset)
			{
				return Hard->Asset->GetPathName();
			}
			bOutIsNull = true;
			return FString();
		}
		if (const FSoftAssetChooser* Soft = Row.GetPtr<FSoftAssetChooser>())
		{
			const FSoftObjectPath SoftPath = Soft->Asset.ToSoftObjectPath();
			if (SoftPath.IsValid())
			{
				return SoftPath.ToString();
			}
			bOutIsNull = true;
			return FString();
		}
		return FString();
	}

	/** Combine a destination folder + a source asset's short name into a full dest asset path. */
	FString MakeDestAssetPath(const FString& DestFolder, const FString& SourceAssetPath)
	{
		FString Folder = DestFolder;
		while (Folder.EndsWith(TEXT("/")))
		{
			Folder.LeftChopInline(1);
		}
		const FString ShortName = FMonolithAssetUtils::GetAssetName(SourceAssetPath);
		return Folder + TEXT("/") + ShortName;
	}
}

// ===========================================================================
// inspect_chooser
// ===========================================================================

FMonolithActionResult FMonolithChooserActions::HandleInspectChooser(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UChooserTable* Table = FMonolithAssetUtils::LoadAssetByPath<UChooserTable>(AssetPath);
	if (!Table)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("ChooserTable not found: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Table->GetPathName());

	// Result type + class (from parent UChooserSignature)
	Root->SetStringField(TEXT("result_type"), ResultTypeToString(Table->ResultType));
	Root->SetNumberField(TEXT("result_type_value"), static_cast<int32>(Table->ResultType));
	Root->SetStringField(TEXT("result_class"), Table->OutputObjectType ? Table->OutputObjectType->GetPathName() : TEXT(""));

	// Context data parameters. GetContextData() returns a view from the root chooser;
	// do not store the view past this scope.
	{
		TArray<TSharedPtr<FJsonValue>> ContextArr;
		const TConstArrayView<FInstancedStruct> ContextView = Table->GetContextData();
		for (int32 i = 0; i < ContextView.Num(); ++i)
		{
			const FInstancedStruct& Entry = ContextView[i];
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("index"), i);
			const UScriptStruct* SS = Entry.GetScriptStruct();
			Obj->SetStringField(TEXT("struct_type"), SS ? SS->GetName() : TEXT(""));
			if (const FContextObjectTypeClass* AsClass = Entry.GetPtr<FContextObjectTypeClass>())
			{
				Obj->SetStringField(TEXT("kind"), TEXT("class"));
				Obj->SetStringField(TEXT("class"), AsClass->Class ? AsClass->Class->GetPathName() : TEXT(""));
			}
			else if (const FContextObjectTypeStruct* AsStruct = Entry.GetPtr<FContextObjectTypeStruct>())
			{
				Obj->SetStringField(TEXT("kind"), TEXT("struct"));
				Obj->SetStringField(TEXT("struct"), AsStruct->Struct ? AsStruct->Struct->GetPathName() : TEXT(""));
			}
			else
			{
				Obj->SetStringField(TEXT("kind"), TEXT("other"));
			}
			ContextArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
		Root->SetArrayField(TEXT("context_data"), ContextArr);
	}

	// Rows + columns + referenced assets are WITH_EDITORONLY_DATA on the table.
#if WITH_EDITORONLY_DATA
	Root->SetNumberField(TEXT("row_count"), Table->ResultsStructs.Num());
	Root->SetNumberField(TEXT("column_count"), Table->ColumnsStructs.Num());

	{
		TArray<TSharedPtr<FJsonValue>> ColTypes;
		for (const FInstancedStruct& Col : Table->ColumnsStructs)
		{
			const UScriptStruct* SS = Col.GetScriptStruct();
			ColTypes.Add(MakeShared<FJsonValueString>(SS ? SS->GetName() : TEXT("<null>")));
		}
		Root->SetArrayField(TEXT("column_types"), ColTypes);
	}

	{
		TArray<TSharedPtr<FJsonValue>> RefAssets;
		for (int32 r = 0; r < Table->ResultsStructs.Num(); ++r)
		{
			bool bIsNull = false;
			FString StructType;
			const FString AssetRef = GetRowAssetPath(Table->ResultsStructs[r], bIsNull, StructType);
			if (!AssetRef.IsEmpty() || bIsNull)
			{
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetNumberField(TEXT("row"), r);
				Obj->SetStringField(TEXT("struct_type"), StructType);
				Obj->SetStringField(TEXT("asset"), AssetRef);
				Obj->SetBoolField(TEXT("is_null"), bIsNull);
				RefAssets.Add(MakeShared<FJsonValueObject>(Obj));
			}
		}
		Root->SetArrayField(TEXT("referenced_assets"), RefAssets);
	}
#else
	Root->SetNumberField(TEXT("row_count"), Table->CookedResults.Num());
	Root->SetBoolField(TEXT("editor_only_data_available"), false);
#endif

	// Compile to surface validation status (read-only side effect: regenerates cooked data).
	Table->Compile(/*bForce=*/false);
	Root->SetBoolField(TEXT("compiled"), true);

	return FMonolithActionResult::Success(Root);
}

// ===========================================================================
// duplicate_chooser_tree
// ===========================================================================

FMonolithActionResult FMonolithChooserActions::HandleDuplicateChooserTree(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* SourcesPtr = nullptr;
	if (!Params->TryGetArrayField(TEXT("source_assets"), SourcesPtr) || !SourcesPtr)
	{
		return FMonolithActionResult::Error(TEXT("Missing required array parameter: source_assets"));
	}
	const FString DestFolder = Params->GetStringField(TEXT("destination_folder"));
	if (DestFolder.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: destination_folder"));
	}

	// Build the remap table (old asset path -> new asset path).
	TMap<FString, FString> Remap;
	const TSharedPtr<FJsonObject>* RemapObj = nullptr;
	if (Params->TryGetObjectField(TEXT("remap_rules"), RemapObj) && RemapObj && RemapObj->IsValid())
	{
		for (const auto& Pair : (*RemapObj)->Values)
		{
			FString Val;
			if (Pair.Value.IsValid() && Pair.Value->TryGetString(Val))
			{
				Remap.Add(Pair.Key, Val);
			}
		}
	}

	auto ApplyRemap = [&Remap](const FString& InPath) -> FString
	{
		if (const FString* Found = Remap.Find(InPath))
		{
			return *Found;
		}
		return InPath;
	};

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 Duplicated = 0;

	for (const TSharedPtr<FJsonValue>& Val : *SourcesPtr)
	{
		FString SourcePath;
		if (!Val.IsValid() || !Val->TryGetString(SourcePath) || SourcePath.IsEmpty())
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("source"), SourcePath);

		const FString DestPath = MakeDestAssetPath(DestFolder, SourcePath);
		UObject* Dup = UEditorAssetLibrary::DuplicateAsset(SourcePath, DestPath);
		if (!Dup)
		{
			Entry->SetBoolField(TEXT("ok"), false);
			Entry->SetStringField(TEXT("error"), FString::Printf(TEXT("DuplicateAsset failed -> %s"), *DestPath));
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		Entry->SetStringField(TEXT("destination"), Dup->GetPathName());
		++Duplicated;

		UChooserTable* DupTable = Cast<UChooserTable>(Dup);
		if (DupTable && Remap.Num() > 0)
		{
			int32 RefsRemapped = 0;

#if WITH_EDITORONLY_DATA
			// Remap result-row asset references on the duplicate (never the source).
			for (FInstancedStruct& Row : DupTable->ResultsStructs)
			{
				if (FAssetChooser* Hard = Row.GetMutablePtr<FAssetChooser>())
				{
					if (Hard->Asset)
					{
						const FString OldRef = Hard->Asset->GetPathName();
						const FString NewRef = ApplyRemap(OldRef);
						if (NewRef != OldRef)
						{
							if (UObject* Loaded = FMonolithAssetUtils::LoadAssetByPath(NewRef))
							{
								Hard->Asset = Loaded;
								++RefsRemapped;
							}
						}
					}
				}
				else if (FSoftAssetChooser* Soft = Row.GetMutablePtr<FSoftAssetChooser>())
				{
					const FString OldRef = Soft->Asset.ToSoftObjectPath().ToString();
					const FString NewRef = ApplyRemap(OldRef);
					if (NewRef != OldRef)
					{
						Soft->Asset = TSoftObjectPtr<UObject>(FSoftObjectPath(NewRef));
						++RefsRemapped;
					}
				}
			}

			// Remap structural parent/nested links by reloading the remapped target.
			if (DupTable->ParentTable)
			{
				const FString NewParent = ApplyRemap(DupTable->ParentTable->GetPathName());
				if (UChooserTable* P = FMonolithAssetUtils::LoadAssetByPath<UChooserTable>(NewParent))
				{
					if (P != DupTable->ParentTable) { DupTable->ParentTable = P; ++RefsRemapped; }
				}
			}
#endif // WITH_EDITORONLY_DATA

			if (DupTable->RootChooser)
			{
				const FString NewRoot = ApplyRemap(DupTable->RootChooser->GetPathName());
				if (UChooserTable* R = FMonolithAssetUtils::LoadAssetByPath<UChooserTable>(NewRoot))
				{
					if (R != DupTable->RootChooser) { DupTable->RootChooser = R; ++RefsRemapped; }
				}
			}

			DupTable->MarkPackageDirty();
			DupTable->Compile(/*bForce=*/true);
			Entry->SetNumberField(TEXT("refs_remapped"), RefsRemapped);
		}

		Entry->SetBoolField(TEXT("ok"), true);
		Results.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("duplicated"), Duplicated);
	Root->SetArrayField(TEXT("results"), Results);
	return FMonolithActionResult::Success(Root);
}

// ===========================================================================
// set_context_object_class
// ===========================================================================

FMonolithActionResult FMonolithChooserActions::HandleSetContextObjectClass(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString IndexStr  = Params->GetStringField(TEXT("context_name_or_index"));
	const FString ClassPath = Params->GetStringField(TEXT("class_path"));

	if (ClassPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: class_path"));
	}

	UChooserTable* Table = FMonolithAssetUtils::LoadAssetByPath<UChooserTable>(AssetPath);
	if (!Table)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("ChooserTable not found: %s"), *AssetPath));
	}

	UClass* NewClass = LoadClass<UObject>(nullptr, *ClassPath);
	if (!NewClass)
	{
		// Fall back to a soft class path resolve (covers BP generated _C classes).
		const FSoftClassPath SoftClass(ClassPath);
		NewClass = SoftClass.ResolveClass();
		if (!NewClass)
		{
			NewClass = LoadObject<UClass>(nullptr, *ClassPath);
		}
	}
	if (!NewClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Could not resolve class: %s"), *ClassPath));
	}

	// ContextData lives on the context-owner (root chooser); edit it there so the
	// view returned by GetContextData() reflects the change.
	UChooserTable* Owner = Table->GetContextOwner();
	if (!Owner)
	{
		return FMonolithActionResult::Error(TEXT("ChooserTable has no context owner"));
	}

	// Resolve the target context entry: explicit numeric index, else first class entry.
	int32 TargetIndex = INDEX_NONE;
	if (IndexStr.IsNumeric())
	{
		TargetIndex = FCString::Atoi(*IndexStr);
	}

	FContextObjectTypeClass* ClassEntry = nullptr;
	if (TargetIndex != INDEX_NONE)
	{
		if (!Owner->ContextData.IsValidIndex(TargetIndex))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("context index %d out of range (have %d entries)"), TargetIndex, Owner->ContextData.Num()));
		}
		ClassEntry = Owner->ContextData[TargetIndex].GetMutablePtr<FContextObjectTypeClass>();
		if (!ClassEntry)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("ContextData[%d] is not a class parameter (FContextObjectTypeClass)"), TargetIndex));
		}
	}
	else
	{
		for (int32 i = 0; i < Owner->ContextData.Num(); ++i)
		{
			if (FContextObjectTypeClass* Candidate = Owner->ContextData[i].GetMutablePtr<FContextObjectTypeClass>())
			{
				ClassEntry = Candidate;
				TargetIndex = i;
				break;
			}
		}
		if (!ClassEntry)
		{
			return FMonolithActionResult::Error(TEXT("No class-typed context parameter (FContextObjectTypeClass) found on this chooser"));
		}
	}

	const FString OldClass = ClassEntry->Class ? ClassEntry->Class->GetPathName() : TEXT("");
	ClassEntry->Class = NewClass;

	Owner->MarkPackageDirty();
	Owner->Compile(/*bForce=*/true);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Table->GetPathName());
	Root->SetNumberField(TEXT("context_index"), TargetIndex);
	Root->SetStringField(TEXT("old_class"), OldClass);
	Root->SetStringField(TEXT("new_class"), NewClass->GetPathName());
	return FMonolithActionResult::Success(Root);
}

// ===========================================================================
// set_result_asset_reference
// ===========================================================================

FMonolithActionResult FMonolithChooserActions::HandleSetResultAssetReference(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString NewAssetPath = Params->GetStringField(TEXT("asset_path_value"));

	double RowVal = 0.0;
	if (!Params->TryGetNumberField(TEXT("row_or_column"), RowVal))
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: row_or_column"));
	}
	const int32 Row = static_cast<int32>(RowVal);

	if (NewAssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path_value"));
	}

	UChooserTable* Table = FMonolithAssetUtils::LoadAssetByPath<UChooserTable>(AssetPath);
	if (!Table)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("ChooserTable not found: %s"), *AssetPath));
	}

#if WITH_EDITORONLY_DATA
	if (!Table->ResultsStructs.IsValidIndex(Row))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("result row %d out of range (have %d rows)"), Row, Table->ResultsStructs.Num()));
	}

	FInstancedStruct& RowStruct = Table->ResultsStructs[Row];
	FString OldRef;

	if (FAssetChooser* Hard = RowStruct.GetMutablePtr<FAssetChooser>())
	{
		OldRef = Hard->Asset ? Hard->Asset->GetPathName() : TEXT("");
		UObject* Loaded = FMonolithAssetUtils::LoadAssetByPath(NewAssetPath);
		if (!Loaded)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Could not load asset for hard reference: %s"), *NewAssetPath));
		}
		Hard->Asset = Loaded;
	}
	else if (FSoftAssetChooser* Soft = RowStruct.GetMutablePtr<FSoftAssetChooser>())
	{
		OldRef = Soft->Asset.ToSoftObjectPath().ToString();
		Soft->Asset = TSoftObjectPtr<UObject>(FSoftObjectPath(NewAssetPath));
	}
	else
	{
		const UScriptStruct* SS = RowStruct.GetScriptStruct();
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("result row %d is not an asset result (type: %s)"), Row, SS ? *SS->GetName() : TEXT("<null>")));
	}

	Table->MarkPackageDirty();
	Table->Compile(/*bForce=*/true);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Table->GetPathName());
	Root->SetNumberField(TEXT("row"), Row);
	Root->SetStringField(TEXT("old_asset"), OldRef);
	Root->SetStringField(TEXT("new_asset"), NewAssetPath);
	return FMonolithActionResult::Success(Root);
#else
	return FMonolithActionResult::Error(TEXT("ResultsStructs is editor-only data; not available in this build"));
#endif
}

// ===========================================================================
// validate_chooser
// ===========================================================================

FMonolithActionResult FMonolithChooserActions::HandleValidateChooser(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString ExpectedContextClass = Params->HasField(TEXT("expected_context_class"))
		? Params->GetStringField(TEXT("expected_context_class")) : FString();
	const FString ExpectedResultType = Params->HasField(TEXT("expected_result_type"))
		? Params->GetStringField(TEXT("expected_result_type")) : FString();

	UChooserTable* Table = FMonolithAssetUtils::LoadAssetByPath<UChooserTable>(AssetPath);
	if (!Table)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("ChooserTable not found: %s"), *AssetPath));
	}

	// Force-compile to surface validation state.
	Table->Compile(/*bForce=*/true);

	TArray<TSharedPtr<FJsonValue>> Issues;
	bool bValid = true;

	// Expected result type check.
	if (!ExpectedResultType.IsEmpty())
	{
		EObjectChooserResultType Expected;
		if (!ParseResultType(ExpectedResultType, Expected))
		{
			Issues.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("unrecognized expected_result_type '%s'"), *ExpectedResultType)));
			bValid = false;
		}
		else if (Table->ResultType != Expected)
		{
			Issues.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("result type mismatch: expected %s, got %s"),
				*ExpectedResultType, *ResultTypeToString(Table->ResultType))));
			bValid = false;
		}
	}

	// Expected context class check (any class-typed context entry matching).
	if (!ExpectedContextClass.IsEmpty())
	{
		bool bFound = false;
		const TConstArrayView<FInstancedStruct> ContextView = Table->GetContextData();
		for (const FInstancedStruct& Entry : ContextView)
		{
			if (const FContextObjectTypeClass* AsClass = Entry.GetPtr<FContextObjectTypeClass>())
			{
				if (AsClass->Class)
				{
					const FString CName = AsClass->Class->GetPathName();
					if (CName == ExpectedContextClass || AsClass->Class->GetName() == ExpectedContextClass)
					{
						bFound = true;
						break;
					}
				}
			}
		}
		if (!bFound)
		{
			Issues.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("no context class parameter matches expected '%s'"), *ExpectedContextClass)));
			bValid = false;
		}
	}

	// Null/stale result-row asset references.
	int32 NullRefs = 0;
#if WITH_EDITORONLY_DATA
	for (int32 r = 0; r < Table->ResultsStructs.Num(); ++r)
	{
		bool bIsNull = false;
		FString StructType;
		const FString AssetRef = GetRowAssetPath(Table->ResultsStructs[r], bIsNull, StructType);
		if (bIsNull)
		{
			++NullRefs;
			Issues.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("result row %d (%s) has a null asset reference"), r, *StructType)));
			bValid = false;
		}
		else if (!AssetRef.IsEmpty() && !FMonolithAssetUtils::AssetExists(AssetRef))
		{
			Issues.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("result row %d references a missing asset: %s"), r, *AssetRef)));
			bValid = false;
		}
	}
#endif

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Table->GetPathName());
	Root->SetBoolField(TEXT("valid"), bValid);
	Root->SetStringField(TEXT("result_type"), ResultTypeToString(Table->ResultType));
	Root->SetStringField(TEXT("result_class"), Table->OutputObjectType ? Table->OutputObjectType->GetPathName() : TEXT(""));
	Root->SetNumberField(TEXT("null_ref_count"), NullRefs);
	Root->SetArrayField(TEXT("issues"), Issues);
	return FMonolithActionResult::Success(Root);
}

#endif // WITH_CHOOSER

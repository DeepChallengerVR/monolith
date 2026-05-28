// SPDX-License-Identifier: MIT
// Plan: Plugins/Monolith/Docs/plans/2026-05-28-reflection-intelligence.md (Phase 4a — v0.17.0).
//
// FNetworkRepIndexer — implementation. Regex sweep over `*.gen.cpp` artefacts;
// writes reflect_replicated_properties.
//
// Structural anatomy of the UHT artefact (mirrors Phase 3a's understanding):
//
//   // ********** Begin Class <Name> *...*
//   ...
//   static constexpr UE::CodeGen::FMetaDataPairParam NewProp_<X>_MetaData[] = {
//       { "Category", "Foo" },
//       { "ReplicatedUsing", "OnRep_<Func>" },
//       ...
//   };
//   ...
//   // ********** End Class <Name>
//
// Phase 4a's signal: any NewProp_<X>_MetaData block containing a
// "ReplicatedUsing" key fires a ReplicatedUsing row. We do NOT yet detect bare
// `UPROPERTY(Replicated)` (without =OnRep) because UHT does not emit a
// "Replicated" MetaData pair — it sets PropertyIsRepNotify on the type-tag
// flags instead. Phase 4b will deepen the scan to cover bare Replicated rows
// via the PropPointers[] specifier-flag scan. For Phase 4a the audit surface
// focused on `audit_onrep_coverage` and `audit_unbalanced_onreps` benefits more
// from accurate ReplicatedUsing capture than partial Replicated capture.

#include "Network/FNetworkRepIndexer.h"
#include "Network/NetworkSchema.h"
#include "MonolithReflectionIntelModule.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SQLiteDatabase.h"

namespace
{
	/** Walk up an absolute artefact path and return the `<Module>` segment that
	 *  appears between `/Inc/` and `/UHT/`. Returns FString() on failure.
	 *  Duplicated from FUHTArtefactReader.cpp on purpose — consolidation is
	 *  Phase 5+ work. */
	FString ModuleNameFromArtefactDir(const FString& AbsArtefactPath)
	{
		FString Norm = AbsArtefactPath;
		Norm.ReplaceInline(TEXT("\\"), TEXT("/"));
		const int32 IncIdx = Norm.Find(TEXT("/Inc/"));
		const int32 UhtIdx = Norm.Find(TEXT("/UHT/"));
		if (IncIdx == INDEX_NONE || UhtIdx == INDEX_NONE || UhtIdx <= IncIdx + 5)
		{
			return FString();
		}
		const int32 StartIdx = IncIdx + 5; // skip "/Inc/"
		return Norm.Mid(StartIdx, UhtIdx - StartIdx);
	}
}

FNetworkRepIndexer::FNetworkRepIndexer()
	// Phase 2 code-quality non-negotiable #4 — patterns built once per indexer instance.
	: IwyuIncludePattern(
		TEXT("//\\s*IWYU\\s+pragma:\\s*private,\\s*include\\s*\"([^\"]+)\""))
	, BeginClassPattern(
		TEXT("//\\s*\\*+\\s*Begin\\s+Class\\s+([A-Za-z_][A-Za-z0-9_]*)\\s+\\*"))
	, NewPropMetaDataHeader(
		// Match the start of a per-property MetaData block. Capture the
		// property name. UHT-emitted form:
		//   static constexpr UE::CodeGen::FMetaDataPairParam NewProp_<Name>_MetaData[]
		// Or the alternative `FMetaDataPairParam ...::NewProp_<Name>_MetaData[]` form.
		TEXT("NewProp_([A-Za-z_][A-Za-z0-9_]*)_MetaData\\s*\\["))
	, MetaDataPairPattern(
		// A `{ "Key", "Value" }` row inside a MetaDataPairParam[] body.
		TEXT("\\{\\s*\"([^\"]+)\"\\s*,\\s*\"([^\"]*)\"\\s*\\}"))
{
}

bool FNetworkRepIndexer::Run(FSQLiteDatabase& DB,
	const TArray<FString>& ArtefactRoots,
	bool bIncludeEnginePlugins,
	FString& OutStatus)
{
	ensure(IsInGameThread());

	if (!EnsureSchema(DB))
	{
		OutStatus = TEXT("FNetworkRepIndexer: schema bootstrap failed");
		UE_LOG(LogMonolithReflectionIntel, Error, TEXT("%s"), *OutStatus);
		return false;
	}

	WipeTables(DB);

	// Resolve roots. Empty = auto-resolve.
	TArray<FString> ResolvedRoots;
	const FString ProjectRoot =
		FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	if (ArtefactRoots.Num() == 0)
	{
		ResolvedRoots.Add(FPaths::ConvertRelativePathToFull(
			FPaths::ProjectIntermediateDir() / TEXT("Build")));
	}
	else
	{
		for (const FString& RawRoot : ArtefactRoots)
		{
			FString Root = RawRoot;
			if (FPaths::IsRelative(Root))
			{
				Root = FPaths::ConvertRelativePathToFull(ProjectRoot / Root);
			}
			ResolvedRoots.Add(FPaths::ConvertRelativePathToFull(Root));
		}
	}

	TArray<TPair<FString, FString>> ModuleAndArtefactPairs;
	for (const FString& Root : ResolvedRoots)
	{
		IPlatformFile& Pf = FPlatformFileManager::Get().GetPlatformFile();
		if (!Pf.DirectoryExists(*Root))
		{
			UE_LOG(LogMonolithReflectionIntel, Verbose,
				TEXT("FNetworkRepIndexer: skipping missing root '%s'"), *Root);
			continue;
		}
		CollectArtefacts(Root, bIncludeEnginePlugins, ModuleAndArtefactPairs);
	}

	if (ModuleAndArtefactPairs.Num() == 0)
	{
		OutStatus = TEXT(
			"FNetworkRepIndexer: no UHT artefacts found — has the project built "
			"with UBT yet? (Live Coding patches do not produce gen.cpp).");
		UE_LOG(LogMonolithReflectionIntel, Warning, TEXT("%s"), *OutStatus);
		return true;
	}

	int32 TotalReps = 0;
	int32 FilesScanned = 0;

	for (const TPair<FString, FString>& Pair : ModuleAndArtefactPairs)
	{
		FNetworkArtefactBatch Batch;
		ScanArtefact(/*AbsArtefactPath=*/Pair.Value,
		             /*ModuleName=*/Pair.Key,
		             /*OutBatch=*/Batch);

		if (!WriteBatch(DB, Batch))
		{
			UE_LOG(LogMonolithReflectionIntel, Warning,
				TEXT("FNetworkRepIndexer: write failed for %s"), *Pair.Value);
		}

		TotalReps += Batch.ReplicatedProperties.Num();
		++FilesScanned;
	}

	OutStatus = FString::Printf(
		TEXT("FNetworkRepIndexer: %d artefacts scanned → ReplicatedProperties=%d"),
		FilesScanned, TotalReps);
	UE_LOG(LogMonolithReflectionIntel, Log, TEXT("%s"), *OutStatus);
	return true;
}

bool FNetworkRepIndexer::EnsureSchema(FSQLiteDatabase& DB)
{
	auto Exec = [&DB](const TCHAR* Sql) -> bool
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(DB, Sql))
		{
			UE_LOG(LogMonolithReflectionIntel, Error,
				TEXT("FNetworkRepIndexer DDL prepare failed: %s"), Sql);
			return false;
		}
		return Stmt.Execute();
	};

	using namespace MonolithNetworkSchema;
	if (!Exec(GetCreateReplicatedPropertiesTableSQL())) { return false; }
	// Indices are non-fatal.
	Exec(GetCreateReplicatedPropertiesIndexOwningClassSQL());
	Exec(GetCreateReplicatedPropertiesIndexRepNotifySQL());
	return true;
}

void FNetworkRepIndexer::WipeTables(FSQLiteDatabase& DB)
{
	FSQLitePreparedStatement Stmt;
	Stmt.Create(DB, TEXT("DELETE FROM reflect_replicated_properties;"));
	Stmt.Execute();
}

void FNetworkRepIndexer::CollectArtefacts(
	const FString& RootAbs, bool bIncludeEnginePlugins,
	TArray<TPair<FString, FString>>& OutModuleAndArtefactPairs)
{
	class FCppGenVisitor : public IPlatformFile::FDirectoryVisitor
	{
	public:
		TArray<TPair<FString, FString>>& Out;
		bool bIncludeEngine;
		explicit FCppGenVisitor(TArray<TPair<FString, FString>>& InOut, bool bInIncludeEngine)
			: Out(InOut), bIncludeEngine(bInIncludeEngine) {}

		virtual bool Visit(const TCHAR* FilenameOrDirectory, bool bIsDirectory) override
		{
			if (bIsDirectory) { return true; }
			const FString Path(FilenameOrDirectory);
			FString Norm = Path;
			Norm.ReplaceInline(TEXT("\\"), TEXT("/"));
			if (!Norm.EndsWith(TEXT(".gen.cpp"), ESearchCase::IgnoreCase)) { return true; }
			if (Norm.Find(TEXT("/UHT/")) == INDEX_NONE) { return true; }
			if (!bIncludeEngine && Norm.Find(TEXT("/Engine/")) != INDEX_NONE)
			{
				return true;
			}
			const FString Module = ModuleNameFromArtefactDir(Norm);
			if (Module.IsEmpty()) { return true; }
			Out.Emplace(Module, Norm);
			return true;
		}
	};
	FCppGenVisitor Visitor(OutModuleAndArtefactPairs, bIncludeEnginePlugins);
	IFileManager::Get().IterateDirectoryRecursively(*RootAbs, Visitor);
}

void FNetworkRepIndexer::ScanArtefact(
	const FString& AbsArtefactPath,
	const FString& ModuleName,
	FNetworkArtefactBatch& OutBatch)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *AbsArtefactPath)) { return; }

	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);

	FString SourceHeaderRel; // from IWYU pragma line, file-level
	{
		FRegexMatcher M(IwyuIncludePattern, Text);
		if (M.FindNext())
		{
			SourceHeaderRel = M.GetCaptureGroup(1);
		}
	}

	// State machine. Track current Class banner + current per-property
	// MetaData block (open when we see `NewProp_<X>_MetaData[]`, close at
	// the first `};` line after the header).
	FString CurrentClass;
	FString CurrentMetaProperty;
	bool bInMetaBlock = false;
	FString PendingRepKind;
	FString PendingRepNotifyFunc;

	for (int32 LineIdx = 0; LineIdx < Lines.Num(); ++LineIdx)
	{
		const FString& Line = Lines[LineIdx];

		// ------ Begin Class banner — open a new Class segment.
		{
			FRegexMatcher M(BeginClassPattern, Line);
			if (M.FindNext())
			{
				CurrentClass = M.GetCaptureGroup(1);
				// Reset any in-progress meta-block state if a previous block
				// did not close cleanly (defensive).
				CurrentMetaProperty.Empty();
				bInMetaBlock = false;
				PendingRepKind.Empty();
				PendingRepNotifyFunc.Empty();
				continue;
			}
		}

		if (CurrentClass.IsEmpty()) { continue; }

		// ------ Open a per-property MetaData block.
		if (!bInMetaBlock)
		{
			FRegexMatcher M(NewPropMetaDataHeader, Line);
			if (M.FindNext())
			{
				CurrentMetaProperty = M.GetCaptureGroup(1);
				bInMetaBlock = true;
				PendingRepKind.Empty();
				PendingRepNotifyFunc.Empty();
				continue;
			}
			// Not in a meta block and not opening one — nothing to do.
			continue;
		}

		// ------ Inside a per-property MetaData block.
		// Close on a `};` line (end of array body).
		if (Line.Contains(TEXT("};")))
		{
			// Emit if we captured a replication signal.
			if (!PendingRepKind.IsEmpty())
			{
				FNetworkRepPropertyRow Row;
				Row.OwningClass = CurrentClass;
				Row.PropertyName = CurrentMetaProperty;
				Row.CppModule = ModuleName;
				Row.RepKind = PendingRepKind;
				Row.RepNotifyFunc = PendingRepNotifyFunc;
				Row.SourcePath = SourceHeaderRel;
				OutBatch.ReplicatedProperties.Add(MoveTemp(Row));
			}
			CurrentMetaProperty.Empty();
			bInMetaBlock = false;
			PendingRepKind.Empty();
			PendingRepNotifyFunc.Empty();
			continue;
		}

		// Scan for MetaData pair rows. Multiple pairs per line is rare but
		// possible (the artefact generator usually one-per-line). FindNext()
		// is a while-loop over the same matcher.
		FRegexMatcher PairM(MetaDataPairPattern, Line);
		while (PairM.FindNext())
		{
			const FString Key = PairM.GetCaptureGroup(1);
			const FString Val = PairM.GetCaptureGroup(2);
			if (Key.Equals(TEXT("ReplicatedUsing"), ESearchCase::IgnoreCase))
			{
				PendingRepKind = TEXT("ReplicatedUsing");
				PendingRepNotifyFunc = Val;
			}
		}
	}
}

bool FNetworkRepIndexer::WriteBatch(FSQLiteDatabase& DB, const FNetworkArtefactBatch& Batch)
{
	if (Batch.ReplicatedProperties.Num() == 0) { return true; }

	DB.Execute(TEXT("BEGIN TRANSACTION;"));
	bool bAllOk = true;

	FSQLitePreparedStatement Ins;
	if (!Ins.Create(DB, TEXT(
		"INSERT OR REPLACE INTO reflect_replicated_properties "
		"(owning_class, property_name, cpp_module, rep_kind, rep_notify_func, source_path, source_line) "
		"VALUES (?, ?, ?, ?, ?, ?, ?);")))
	{
		bAllOk = false;
	}
	else
	{
		for (const FNetworkRepPropertyRow& Row : Batch.ReplicatedProperties)
		{
			Ins.Reset();
			Ins.ClearBindings();
			Ins.SetBindingValueByIndex(1, Row.OwningClass);
			Ins.SetBindingValueByIndex(2, Row.PropertyName);
			Ins.SetBindingValueByIndex(3, Row.CppModule);
			Ins.SetBindingValueByIndex(4, Row.RepKind);
			Ins.SetBindingValueByIndex(5, Row.RepNotifyFunc);
			Ins.SetBindingValueByIndex(6, Row.SourcePath);
			Ins.SetBindingValueByIndex(7, 0); // source_line — not derivable from UHT artefact
			if (!Ins.Execute()) { bAllOk = false; }
		}
	}

	DB.Execute(TEXT("COMMIT;"));
	return bAllOk;
}

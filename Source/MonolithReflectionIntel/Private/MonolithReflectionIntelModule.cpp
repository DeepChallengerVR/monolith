// SPDX-License-Identifier: MIT
// Plan: Plugins/Monolith/Docs/plans/2026-05-28-reflection-intelligence.md (Phase 1).
//
// DEVIATION (vs plan §6 row "Modify MonolithSourceSubsystem.cpp — register
// FDecisionRecordIndexer in the indexer roster"):
// UMonolithSourceSubsystem does NOT expose an indexer-roster API in the live
// codebase; it owns a single FMonolithSourceIndexer for C++ source mining
// only. To keep dependency direction correct (MonolithReflectionIntel ->
// MonolithSource is the legal direction; the reverse would be circular),
// the indexer self-bootstraps:
//   1. Once on demand from FDecisionQueryAdapter when a table miss occurs.
//   2. On FCoreUObjectDelegates::ReloadCompleteDelegate (Live Coding / hot
//      reload) so corpus changes since editor start are picked up.
// Net effect matches the plan's intent: decision_query results stay fresh,
// and the indexer runs in the same editor lifecycle as the source subsystem.

#include "MonolithReflectionIntelModule.h"
#include "Decision/FDecisionRecordIndexer.h"
#include "Decision/FDecisionQueryAdapter.h"
#include "Risk/FGitCoChangeIndexer.h"
#include "Risk/FHotspotScorer.h"
#include "Risk/FConditionalGateIndexer.h"
#include "Risk/FRiskQueryAdapter.h"
#include "SourceAudit/FModuleDepRealityAdapter.h"
#include "CppReflect/FUHTArtefactReader.h"
#include "CppReflect/FAssetGraphJoiner.h"
#include "CppReflect/FCppReflectQueryAdapter.h"
// Phase 4a (v0.17.0) headers — network namespace, audit-action extensions,
// pipeline composers.
#include "Network/FNetworkRepIndexer.h"
#include "Network/FNetworkQueryAdapter.h"
#include "Audit/FAuditAdapter.h"
#include "Pipeline/FPipelineAdapter.h"
#include "MonolithReflectionIntelSettings.h"

#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "MonolithToolRegistry.h"
#include "SQLiteDatabase.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY(LogMonolithReflectionIntel);

#define LOCTEXT_NAMESPACE "FMonolithReflectionIntelModule"

namespace
{
	FString GetEngineSourceDbPathStatic()
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
		if (Plugin.IsValid())
		{
			return Plugin->GetBaseDir() / TEXT("Saved") / TEXT("EngineSource.db");
		}
		return FPaths::ProjectPluginsDir() / TEXT("Monolith") / TEXT("Saved") / TEXT("EngineSource.db");
	}
}

void FMonolithReflectionIntelModule::StartupModule()
{
	// Explicit re-arm of the lazy-bootstrap latches on every module load. A
	// fresh module instance always starts with all four latches cleared so
	// that Live Coding reloads re-attempt bootstrap if a prior attempt failed.
	bDecisionBootstrapAttempted = false;
	bRiskBootstrapAttempted = false;
	bCppReflectBootstrapAttempted = false;
	bNetworkBootstrapAttempted = false;

	RegisterDecisionActions();
	// Phase 2 (v0.17.0) — risk_query namespace + source_query audit action.
	RegisterRiskActions();
	RegisterSourceAuditActions();
	// Phase 3a (v0.17.0) — cppreflect_query namespace (5 actions).
	RegisterCppReflectActions();
	// Phase 4a (v0.17.0) — network_query namespace (4 actions) + 4 audit
	// actions on existing namespaces + pipeline_query namespace (2 composers).
	RegisterNetworkActions();
	RegisterAuditActions();
	RegisterPipelineActions();

	// Bind hot-reload hook so the decision corpus refreshes after Live Coding /
	// UBT rebuilds. The handler opens the DB ReadWrite only briefly (the
	// indexer wipes+rewrites and closes immediately) — collision with the
	// source subsystem's ReadWrite handle is avoided by only firing on
	// reload-complete (subsystem closes its handle during reindex anyway,
	// and outside of reindex SQLite tolerates a second briefly-open RW handle).
	ReloadCompleteHandle = FCoreUObjectDelegates::ReloadCompleteDelegate.AddRaw(
		this, &FMonolithReflectionIntelModule::OnReloadComplete);

	// NO eager bootstrap on StartupModule — the source subsystem's WAL handle
	// may already be open on EngineSource.db at this point and our writer would
	// race. All indexer bootstrap is LAZY:
	//   - Decision:    driven on first decision_query call.
	//   - Risk:        driven on first risk_query call.
	//   - CppReflect:  driven on first cppreflect_query call.
	//   - All:         driven on hot-reload via OnReloadComplete.

	UE_LOG(LogMonolithReflectionIntel, Log,
		TEXT("Monolith — ReflectionIntel module loaded (decision_query: 5 actions, "
		     "risk_query: 5 actions, source_query: +1 audit action, "
		     "cppreflect_query: 5 actions, network_query: 4 actions, "
		     "material/niagara/blueprint/project: +1 audit each, "
		     "pipeline_query: 2 composers)"));
}

void FMonolithReflectionIntelModule::ShutdownModule()
{
	if (ReloadCompleteHandle.IsValid())
	{
		FCoreUObjectDelegates::ReloadCompleteDelegate.Remove(ReloadCompleteHandle);
		ReloadCompleteHandle.Reset();
	}

	// CRITICAL: tear down the cached SQLite handle BEFORE module unload. Plain
	// TUniquePtr<T> member destruction order during module teardown is not
	// guaranteed; an explicit Reset() ensures FSQLiteDatabase::Close() runs
	// while the SQLiteCore module is still loaded, releasing the file lock
	// cleanly. Without this, editor exit / module reload would leak the
	// SQLite handle AND the underlying file lock on EngineSource.db.
	ResetCachedQueryDb();

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("decision"));
	// Phase 2 — risk_query is fully owned by this module so unregister wholesale.
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("risk"));
	// Phase 3a — cppreflect_query is fully owned by this module.
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("cppreflect"));
	// Phase 4a — network_query is fully owned by this module.
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("network"));
	// Phase 4a — pipeline_query is fully owned by this module (no other
	// module registers there in Phase 4a).
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("pipeline"));
	// NOTE: we do NOT unregister the `source` / `material` / `niagara` /
	// `blueprint` / `project` namespaces here — MonolithSource / MonolithMaterial
	// / MonolithNiagara / MonolithBlueprint / MonolithIndex own the lion's share
	// of those namespaces' actions. Our audit-action additions would also be
	// unregistered with a namespace-wide call. The risk is minimal: Phase 4a
	// ships with those modules loaded before us (the `MonolithCore` dep chain
	// enforces order) so shutdown-time leaks are bounded to one audit action
	// entry per host namespace per process exit.
}

FSQLiteDatabase* FMonolithReflectionIntelModule::GetOrOpenCachedQueryDb()
{
	const FString DbPath = GetEngineSourceDbPathStatic();

	// Fast path: handle already open at the same path and still valid.
	if (CachedQueryDb.IsValid() && CachedQueryDbPath == DbPath && CachedQueryDb->IsValid())
	{
		return CachedQueryDb.Get();
	}

	// Drop any stale handle (path-change or invalidated) before opening a new one.
	if (CachedQueryDb.IsValid())
	{
		CachedQueryDb->Close();
		CachedQueryDb.Reset();
	}

	IPlatformFile& Pf = FPlatformFileManager::Get().GetPlatformFile();
	if (!Pf.FileExists(*DbPath))
	{
		// EngineSource.db has not been bootstrapped — caller surfaces the
		// "run source.trigger_reindex" error message.
		return nullptr;
	}

	CachedQueryDb = MakeUnique<FSQLiteDatabase>();
	// ReadOnly mode — pure query side, never writes. Avoids any WAL-mode
	// silent-fail trap by NOT writing (cf. .claude/rules/scoped/cpp-code.md
	// § "Known Pitfalls": SQLite WAL + ReadOnly silent failure on Windows).
	if (!CachedQueryDb->Open(*DbPath, ESQLiteDatabaseOpenMode::ReadOnly))
	{
		CachedQueryDb.Reset();
		return nullptr;
	}
	CachedQueryDbPath = DbPath;
	return CachedQueryDb.Get();
}

void FMonolithReflectionIntelModule::ResetCachedQueryDb()
{
	if (CachedQueryDb.IsValid())
	{
		CachedQueryDb->Close();
		CachedQueryDb.Reset();
	}
	CachedQueryDbPath.Reset();
}

void FMonolithReflectionIntelModule::RegisterDecisionActions()
{
	FDecisionQueryAdapter::RegisterActions(FMonolithToolRegistry::Get());
}

void FMonolithReflectionIntelModule::RegisterRiskActions()
{
	FRiskQueryAdapter::RegisterActions(FMonolithToolRegistry::Get());
}

void FMonolithReflectionIntelModule::RegisterSourceAuditActions()
{
	FModuleDepRealityAdapter::RegisterActions(FMonolithToolRegistry::Get());
}

void FMonolithReflectionIntelModule::RegisterCppReflectActions()
{
	FCppReflectQueryAdapter::RegisterActions(FMonolithToolRegistry::Get());
}

void FMonolithReflectionIntelModule::RegisterNetworkActions()
{
	FNetworkQueryAdapter::RegisterActions(FMonolithToolRegistry::Get());
}

void FMonolithReflectionIntelModule::RegisterAuditActions()
{
	FAuditAdapter::RegisterActions(FMonolithToolRegistry::Get());
}

void FMonolithReflectionIntelModule::RegisterPipelineActions()
{
	FPipelineAdapter::RegisterActions(FMonolithToolRegistry::Get());
}

void FMonolithReflectionIntelModule::OnReloadComplete(EReloadCompleteReason /*Reason*/)
{
	// Fire-and-log; never block the reload signal. Failure to refresh the
	// decision corpus is non-fatal — query handlers still return last-known data.
	FString DecisionStatus;
	RunDecisionIndexerOnce(DecisionStatus);

	// Phase 2 — also re-run risk indexers on hot-reload. The risk indexer is
	// more expensive (spawns `git log`), but a Live Coding rebuild typically
	// reflects code changes that warrant re-scoring complexity AND co-change
	// activity tends to spike around the commits the reload reflects.
	FString RiskStatus;
	RunRiskIndexersOnce(RiskStatus);

	// Phase 3a — UHT artefacts are exactly what a Live Coding rebuild
	// regenerates, so this is the single signal that means "reflection
	// edges may be stale". Refresh after every reload.
	FString CppReflectStatus;
	RunCppReflectIndexersOnce(CppReflectStatus);

	// Phase 4a — network rep metadata lives in the same UHT artefacts; rebuild
	// after reload so audit_unbalanced_onreps stays current. Audit actions
	// themselves are pure SQL/AR queries and don't need bootstrap.
	FString NetworkStatus;
	RunNetworkIndexerOnce(NetworkStatus);
}

bool FMonolithReflectionIntelModule::RunDecisionIndexerOnce(FString& OutStatus)
{
	const UMonolithReflectionIntelSettings* Settings = UMonolithReflectionIntelSettings::Get();
	if (!Settings || !Settings->bEnableDecisionMining)
	{
		OutStatus = TEXT("RunDecisionIndexerOnce: skipped (bEnableDecisionMining=false)");
		return false;
	}

	const FString DbPath = GetEngineSourceDbPathStatic();
	IPlatformFile& Pf = FPlatformFileManager::Get().GetPlatformFile();
	if (!Pf.FileExists(*DbPath))
	{
		OutStatus = FString::Printf(
			TEXT("RunDecisionIndexerOnce: EngineSource.db not present at '%s' — bootstrap with source.trigger_reindex"),
			*DbPath);
		UE_LOG(LogMonolithReflectionIntel, Verbose, TEXT("%s"), *OutStatus);
		return false;
	}

	FSQLiteDatabase Db;
	if (!Db.Open(*DbPath, ESQLiteDatabaseOpenMode::ReadWrite))
	{
		OutStatus = FString::Printf(TEXT("RunDecisionIndexerOnce: failed to open '%s'"), *DbPath);
		UE_LOG(LogMonolithReflectionIntel, Warning, TEXT("%s"), *OutStatus);
		return false;
	}

	// Force DELETE journal mode BEFORE any CREATE TABLE / INSERT runs. Opening
	// a second handle (ours, RW) on a SQLite DB that the source subsystem may
	// have left in WAL mode is exactly the silent-failure pattern documented
	// in .claude/rules/scoped/cpp-code.md § "Known Pitfalls" (WAL + ReadOnly
	// silent failure on Windows). Mirrors the pattern at
	// MonolithSourceDatabase.cpp:124. Tolerate failure (warn + continue) per
	// project convention — other indexers also tolerate this PRAGMA result.
	if (!Db.Execute(TEXT("PRAGMA journal_mode=DELETE;")))
	{
		UE_LOG(LogMonolithReflectionIntel, Warning,
			TEXT("RunDecisionIndexerOnce: PRAGMA journal_mode=DELETE failed on '%s' (continuing)"),
			*DbPath);
	}

	TArray<FString> Roots = Settings->DecisionMarkdownRoots;
	if (Roots.Num() == 0)
	{
		Roots.Add(TEXT("Docs"));
		Roots.Add(TEXT("Plugins/Monolith/Docs"));
		Roots.Add(TEXT(".claude/rules"));
	}

	FDecisionRecordIndexer Indexer;
	const bool bOk = Indexer.Run(Db, Roots, OutStatus);
	Db.Close();
	return bOk;
}

bool FMonolithReflectionIntelModule::RunRiskIndexersOnce(FString& OutStatus)
{
	const UMonolithReflectionIntelSettings* Settings = UMonolithReflectionIntelSettings::Get();
	if (!Settings || !Settings->bEnableGitCoChangeMining)
	{
		OutStatus = TEXT("RunRiskIndexersOnce: skipped (bEnableGitCoChangeMining=false)");
		return false;
	}

	const FString DbPath = GetEngineSourceDbPathStatic();
	IPlatformFile& Pf = FPlatformFileManager::Get().GetPlatformFile();
	if (!Pf.FileExists(*DbPath))
	{
		OutStatus = FString::Printf(
			TEXT("RunRiskIndexersOnce: EngineSource.db not present at '%s' — bootstrap with source.trigger_reindex"),
			*DbPath);
		UE_LOG(LogMonolithReflectionIntel, Verbose, TEXT("%s"), *OutStatus);
		return false;
	}

	FSQLiteDatabase Db;
	if (!Db.Open(*DbPath, ESQLiteDatabaseOpenMode::ReadWrite))
	{
		OutStatus = FString::Printf(TEXT("RunRiskIndexersOnce: failed to open '%s'"), *DbPath);
		UE_LOG(LogMonolithReflectionIntel, Warning, TEXT("%s"), *OutStatus);
		return false;
	}

	// Same DELETE-journal discipline as the decision indexer — opening a
	// second RW handle on a SQLite DB the source subsystem may have left in
	// WAL mode is the documented silent-failure pattern. Cite the project rule.
	if (!Db.Execute(TEXT("PRAGMA journal_mode=DELETE;")))
	{
		UE_LOG(LogMonolithReflectionIntel, Warning,
			TEXT("RunRiskIndexersOnce: PRAGMA journal_mode=DELETE failed on '%s' (continuing)"),
			*DbPath);
	}

	// Resolve git repo roots. Phase 2 mines NESTED git repos only — the
	// project's outer working tree is tracked by Diversion, not git, and lacks
	// a `.git` directory; FGitCoChangeIndexer silently skips it. Standard
	// nested repos under Leviathan today: Monolith plugin, Resonance plugin.
	// Future-siblings (`MonolithSteamBridge`, `MonolithISX`, etc.) live under
	// `Plugins/` so we add them by default — the indexer skips any without
	// `.git/`.
	TArray<FString> GitRoots;
	GitRoots.Add(TEXT("Plugins/Monolith"));
	GitRoots.Add(TEXT("Plugins/Resonance"));
	GitRoots.Add(TEXT("Plugins/MonolithSteamBridge"));
	GitRoots.Add(TEXT("Plugins/MonolithISX"));
	GitRoots.Add(TEXT("Plugins/MonolithSubstance"));
	GitRoots.Add(TEXT("Plugins/MonolithClaudeDesignBridge"));

	const int32 MaxWindow = Settings->MaxCoChangeWindowCommits > 0
		? Settings->MaxCoChangeWindowCommits : 200;
	const int32 MaxFiles = Settings->MaxCommitFileCount > 0
		? Settings->MaxCommitFileCount : 20;

	FString GitStatus;
	FGitCoChangeIndexer GitIndexer;
	const bool bGitOk = GitIndexer.Run(Db, GitRoots, MaxWindow,
		Settings->GitMiningNoiseFilter, MaxFiles, GitStatus);

	FString HotspotStatus;
	FHotspotScorer HotspotScorer;
	const bool bHotspotOk = HotspotScorer.Run(Db, HotspotStatus);

	// Conditional gates — scan project Source/ + Plugins/<plugin>/Source/.
	// We do NOT scan the Plugins/Monolith folder root — only its Source/ —
	// because `.uplugin` / `.uproject` files are not C++.
	TArray<FString> GateRoots;
	GateRoots.Add(TEXT("Source"));
	GateRoots.Add(TEXT("Plugins"));
	FString GateStatus;
	FConditionalGateIndexer GateIndexer;
	const bool bGateOk = GateIndexer.Run(Db, GateRoots, GateStatus);

	Db.Close();

	OutStatus = FString::Printf(
		TEXT("RunRiskIndexersOnce: git=%s | hotspot=%s | gates=%s"),
		*GitStatus, *HotspotStatus, *GateStatus);
	UE_LOG(LogMonolithReflectionIntel, Log, TEXT("%s"), *OutStatus);
	return bGitOk && bHotspotOk && bGateOk;
}

bool FMonolithReflectionIntelModule::RunCppReflectIndexersOnce(FString& OutStatus)
{
	// No settings gate in Phase 3a — the UHT-artefact reader is cheap when no
	// artefacts exist on disk (graceful "0 rows" return). Phase 3b will wrap
	// the source-driven tree-sitter pass behind bIndexEnginePluginReflection.

	const FString DbPath = GetEngineSourceDbPathStatic();
	IPlatformFile& Pf = FPlatformFileManager::Get().GetPlatformFile();
	if (!Pf.FileExists(*DbPath))
	{
		OutStatus = FString::Printf(
			TEXT("RunCppReflectIndexersOnce: EngineSource.db not present at '%s' — bootstrap with source.trigger_reindex"),
			*DbPath);
		UE_LOG(LogMonolithReflectionIntel, Verbose, TEXT("%s"), *OutStatus);
		return false;
	}

	FSQLiteDatabase Db;
	if (!Db.Open(*DbPath, ESQLiteDatabaseOpenMode::ReadWrite))
	{
		OutStatus = FString::Printf(TEXT("RunCppReflectIndexersOnce: failed to open '%s'"), *DbPath);
		UE_LOG(LogMonolithReflectionIntel, Warning, TEXT("%s"), *OutStatus);
		return false;
	}

	// Same DELETE-journal discipline as Phase 1 + Phase 2 — opening a second
	// RW handle on a SQLite DB the source subsystem may have left in WAL mode
	// is the documented silent-failure pattern.
	if (!Db.Execute(TEXT("PRAGMA journal_mode=DELETE;")))
	{
		UE_LOG(LogMonolithReflectionIntel, Warning,
			TEXT("RunCppReflectIndexersOnce: PRAGMA journal_mode=DELETE failed on '%s' (continuing)"),
			*DbPath);
	}

	// Resolve UHT artefact roots from settings. Empty = auto-resolve to
	// ProjectIntermediateDir / Build inside FUHTArtefactReader::Run.
	const UMonolithReflectionIntelSettings* Settings = UMonolithReflectionIntelSettings::Get();
	TArray<FString> ArtefactRoots;
	bool bIncludeEnginePlugins = false;
	if (Settings)
	{
		bIncludeEnginePlugins = Settings->bIndexEnginePluginReflection;
		if (!Settings->UHTArtefactRoot.IsEmpty())
		{
			ArtefactRoots.Add(Settings->UHTArtefactRoot);
		}
	}

	// Run both indexers sequentially. Each opens its own internal transaction
	// so we do not need to wrap them in an outer BEGIN/COMMIT — but ordering
	// matters: FUHTArtefactReader populates reflect_uclasses before
	// FAssetGraphJoiner reads it.
	FString UhtStatus;
	FUHTArtefactReader UhtReader;
	const bool bUhtOk = UhtReader.Run(Db, ArtefactRoots, bIncludeEnginePlugins, UhtStatus);

	FString JoinerStatus;
	FAssetGraphJoiner Joiner;
	const bool bJoinerOk = Joiner.Run(Db, JoinerStatus);

	Db.Close();

	OutStatus = FString::Printf(
		TEXT("RunCppReflectIndexersOnce: uht=%s | asset_graph=%s"),
		*UhtStatus, *JoinerStatus);
	UE_LOG(LogMonolithReflectionIntel, Log, TEXT("%s"), *OutStatus);
	return bUhtOk && bJoinerOk;
}

bool FMonolithReflectionIntelModule::RunNetworkIndexerOnce(FString& OutStatus)
{
	// No settings gate in Phase 4a — the network indexer's UHT-artefact reader
	// is cheap when no artefacts exist on disk (graceful "0 rows" return). A
	// settings toggle (bEnableNetworkReplicationAudit) controls behaviour at
	// the registration layer in a future ergonomics pass; Phase 4a unconditionally
	// runs the indexer here so the action surface stays consistent across
	// build configurations.

	const FString DbPath = GetEngineSourceDbPathStatic();
	IPlatformFile& Pf = FPlatformFileManager::Get().GetPlatformFile();
	if (!Pf.FileExists(*DbPath))
	{
		OutStatus = FString::Printf(
			TEXT("RunNetworkIndexerOnce: EngineSource.db not present at '%s' — bootstrap with source.trigger_reindex"),
			*DbPath);
		UE_LOG(LogMonolithReflectionIntel, Verbose, TEXT("%s"), *OutStatus);
		return false;
	}

	FSQLiteDatabase Db;
	if (!Db.Open(*DbPath, ESQLiteDatabaseOpenMode::ReadWrite))
	{
		OutStatus = FString::Printf(TEXT("RunNetworkIndexerOnce: failed to open '%s'"), *DbPath);
		UE_LOG(LogMonolithReflectionIntel, Warning, TEXT("%s"), *OutStatus);
		return false;
	}

	// Same DELETE-journal discipline as Phase 1/2/3a — opening a second RW
	// handle on a SQLite DB the source subsystem may have left in WAL mode is
	// the documented silent-failure pattern on Windows.
	if (!Db.Execute(TEXT("PRAGMA journal_mode=DELETE;")))
	{
		UE_LOG(LogMonolithReflectionIntel, Warning,
			TEXT("RunNetworkIndexerOnce: PRAGMA journal_mode=DELETE failed on '%s' (continuing)"),
			*DbPath);
	}

	// Resolve UHT artefact roots from settings — same shape as the Phase 3a
	// cppreflect runner. Empty = auto-resolve to ProjectIntermediateDir/Build
	// inside FNetworkRepIndexer::Run.
	const UMonolithReflectionIntelSettings* Settings = UMonolithReflectionIntelSettings::Get();
	TArray<FString> ArtefactRoots;
	bool bIncludeEnginePlugins = false;
	if (Settings)
	{
		bIncludeEnginePlugins = Settings->bIndexEnginePluginReflection;
		if (!Settings->UHTArtefactRoot.IsEmpty())
		{
			ArtefactRoots.Add(Settings->UHTArtefactRoot);
		}
	}

	FNetworkRepIndexer Indexer;
	const bool bOk = Indexer.Run(Db, ArtefactRoots, bIncludeEnginePlugins, OutStatus);
	Db.Close();
	return bOk;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithReflectionIntelModule, MonolithReflectionIntel)

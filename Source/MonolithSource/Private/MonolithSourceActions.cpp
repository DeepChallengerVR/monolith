#include "MonolithSourceActions.h"
#include "MonolithSourceDatabase.h"
#include "MonolithSourceSubsystem.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"
#include "MonolithCursorCodec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Editor.h"
#include "Internationalization/Regex.h"

// ============================================================================
// Registration
// ============================================================================

void FMonolithSourceActions::RegisterAll()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	Registry.RegisterAction(TEXT("source"), TEXT("read_source"),
		TEXT("Get the implementation source code for a class, function, or struct"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleReadSource),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Symbol name (class, function, or struct)"))
			.Optional(TEXT("include_header"), TEXT("bool"), TEXT("Include the header declaration"), TEXT("false"))
			.Optional(TEXT("max_lines"), TEXT("integer"), TEXT("Max lines to return"), TEXT("500"))
			.Optional(TEXT("members_only"), TEXT("bool"), TEXT("Only show class members, not full body"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("find_references"),
		TEXT("Find all usage sites of a symbol (calls, includes, type references)"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleFindReferences),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Symbol name to find references for"))
			.Optional(TEXT("ref_kind"), TEXT("string"), TEXT("Filter by reference kind"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results"), TEXT("50"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("find_callers"),
		TEXT("Find all functions that call the given function"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleFindCallers),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Function name"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results"), TEXT("50"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("find_callees"),
		TEXT("Find all functions called by the given function"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleFindCallees),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Function name"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results"), TEXT("50"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("search_source"),
		TEXT("Full-text search across Unreal Engine source code and shaders. Supports cursor pagination — pass `cursor` from a prior response's `next_cursor` to fetch the next page."),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleSearchSource),
		FParamSchemaBuilder()
			.Required(TEXT("query"), TEXT("string"), TEXT("Search query"))
			.Optional(TEXT("scope"), TEXT("string"), TEXT("Search scope (all, engine, shaders)"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results"), TEXT("50"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("Search mode (fts, regex, exact)"))
			.Optional(TEXT("module"), TEXT("string"), TEXT("Filter to a specific module"))
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Filter by file path pattern"))
			.Optional(TEXT("symbol_kind"), TEXT("string"), TEXT("Filter by symbol kind (class, function, enum, etc.)"))
			// Survivor E (plan §3.E): opaque base64+JSON cursor from a prior
			// response's `next_cursor`. Omit on the first call.
			.Optional(TEXT("cursor"), TEXT("string"), TEXT("Opaque pagination cursor from a prior next_cursor (Survivor E)"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("get_class_hierarchy"),
		TEXT("Show the inheritance tree for a class"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleGetClassHierarchy),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Class name"))
			.Optional(TEXT("direction"), TEXT("string"), TEXT("Direction: up (parents) or down (children)"), TEXT("both"))
			.Optional(TEXT("depth"), TEXT("integer"), TEXT("Max hierarchy depth"), TEXT("5"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("get_module_info"),
		TEXT("Get module statistics: file count, symbol counts by kind, and key classes"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleGetModuleInfo),
		FParamSchemaBuilder()
			.Required(TEXT("module_name"), TEXT("string"), TEXT("Module name"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("get_symbol_context"),
		TEXT("Get a symbol definition with surrounding context lines"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleGetSymbolContext),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Symbol name"))
			.Optional(TEXT("context_lines"), TEXT("integer"), TEXT("Lines of context around the definition"), TEXT("10"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("read_file"),
		TEXT("Read source lines from a file by path"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleReadFile),
		FParamSchemaBuilder()
			.RequiredDiskPath(TEXT("file_path"), TEXT("Source file path"))
			.Optional(TEXT("start_line"), TEXT("integer"), TEXT("First line to read"), TEXT("1"))
			.Optional(TEXT("end_line"), TEXT("integer"), TEXT("Last line to read"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("trigger_reindex"),
		TEXT("Trigger C++ indexer to rebuild the engine source DB (full clean build: engine + shaders + project)"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleTriggerReindex),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("source"), TEXT("trigger_project_reindex"),
		TEXT("Trigger incremental project-only C++ source indexing (loads existing engine symbols, indexes project Source/ and Plugins/)"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleTriggerProjectReindex),
		MakeShared<FJsonObject>());

	// Survivor A (plan §3.A) — annotate the `source_query` namespace dispatcher
	// as read-only + idempotent. The `trigger_reindex` / `trigger_project_reindex`
	// actions are conservatively non-destructive (they kick a background sweep
	// that yields identical results when re-run); every other source action is
	// pure read. Annotating at the DISPATCHER level (not per-action) per plan
	// §3.A — the dispatcher tool is what `tools/list` advertises.
	FMonolithDispatcherAnnotations SourceAnnotations;
	SourceAnnotations.bReadOnlyHint = true;
	SourceAnnotations.bDestructiveHint = false;
	SourceAnnotations.bIdempotentHint = true;
	SourceAnnotations.Title = TEXT("Source-index query");
	Registry.SetDispatcherAnnotations(TEXT("source"), SourceAnnotations);
}

// ============================================================================
// Helpers
// ============================================================================

FMonolithSourceDatabase* FMonolithSourceActions::GetDB()
{
	if (!GEditor) return nullptr;
	UMonolithSourceSubsystem* Subsystem = Cast<UMonolithSourceSubsystem>(GEditor->GetEditorSubsystemBase(UMonolithSourceSubsystem::StaticClass()));
	if (!Subsystem) return nullptr;
	return Subsystem->GetDatabase();
}

FString FMonolithSourceActions::ShortPath(const FString& FullPath)
{
	// Shorten to Engine/... relative path
	FString EngineDir = FPaths::EngineDir();
	FString ParentDir = FPaths::GetPath(EngineDir); // Parent of Engine/
	if (!ParentDir.IsEmpty() && FullPath.StartsWith(ParentDir))
	{
		FString Relative = FullPath.Mid(ParentDir.Len());
		Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (Relative.StartsWith(TEXT("/")))
		{
			Relative = Relative.Mid(1);
		}
		return Relative;
	}
	return FullPath;
}

FString FMonolithSourceActions::ReadFileLines(const FString& FilePath, int32 StartLine, int32 EndLine)
{
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *FilePath))
	{
		return FString::Printf(TEXT("[File not found: %s]"), *FilePath);
	}

	StartLine = FMath::Max(1, StartLine);
	EndLine = FMath::Min(Lines.Num(), EndLine);

	FString Result;
	for (int32 i = StartLine; i <= EndLine; ++i)
	{
		Result += FString::Printf(TEXT("%5d | %s\n"), i, *Lines[i - 1]);
	}
	return Result;
}

bool FMonolithSourceActions::IsForwardDeclaration(const FString& FilePath, int32 LineStart, int32 LineEnd)
{
	if (LineEnd - LineStart > 1)
	{
		return false;
	}

	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *FilePath))
	{
		return false;
	}

	if (LineStart <= Lines.Num())
	{
		const FString& Line = Lines[LineStart - 1];
		FRegexPattern Pattern(TEXT("^\\s*(class|struct|enum)\\s+\\w[\\w:]*\\s*;"));
		FRegexMatcher Matcher(Pattern, Line);
		return Matcher.FindNext();
	}
	return false;
}

FString FMonolithSourceActions::ExtractMembers(const FString& FilePath, int32 StartLine, int32 EndLine)
{
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *FilePath))
	{
		return FString::Printf(TEXT("[Error reading %s]"), *FilePath);
	}

	StartLine = FMath::Max(1, StartLine);
	EndLine = FMath::Min(Lines.Num(), EndLine);

	FString Result;
	int32 BraceDepth = 0;
	bool bInBlockComment = false;
	int32 SignatureLineIdx = -1; // Track pending function signature for Allman-style bodies

	for (int32 i = StartLine - 1; i < EndLine; ++i)
	{
		const FString& Line = Lines[i];
		FString Stripped = Line.TrimStartAndEnd();

		// --- Count braces, respecting comments and string/char literals ---
		int32 PrevDepth = BraceDepth;
		for (int32 c = 0; c < Stripped.Len(); ++c)
		{
			TCHAR Ch = Stripped[c];
			TCHAR Next = (c + 1 < Stripped.Len()) ? Stripped[c + 1] : 0;

			if (bInBlockComment)
			{
				if (Ch == TEXT('*') && Next == TEXT('/'))
				{
					bInBlockComment = false;
					c++; // skip '/'
				}
				continue;
			}

			// Line comment — skip rest of line
			if (Ch == TEXT('/') && Next == TEXT('/')) break;

			// Block comment start
			if (Ch == TEXT('/') && Next == TEXT('*'))
			{
				bInBlockComment = true;
				c++; // skip '*'
				continue;
			}

			// String literal — skip to closing quote
			if (Ch == TEXT('"'))
			{
				for (++c; c < Stripped.Len(); ++c)
				{
					if (Stripped[c] == TEXT('\\')) { c++; }
					else if (Stripped[c] == TEXT('"')) break;
				}
				continue;
			}

			// Char literal — skip to closing quote
			if (Ch == TEXT('\''))
			{
				for (++c; c < Stripped.Len(); ++c)
				{
					if (Stripped[c] == TEXT('\\')) { c++; }
					else if (Stripped[c] == TEXT('\'')) break;
				}
				continue;
			}

			if (Ch == TEXT('{')) BraceDepth++;
			else if (Ch == TEXT('}')) BraceDepth--;
		}

		// --- Depth >= 2 at start OR transitioning 1→2+: inside function body ---
		if (PrevDepth >= 2)
		{
			// Still inside a function body — skip
			continue;
		}

		if (PrevDepth <= 1 && BraceDepth >= 2)
		{
			// Transitioning into a function body on this line
			if (SignatureLineIdx >= 0)
			{
				// Allman style: signature was on a previous line, emit with annotation
				Result += FString::Printf(TEXT("%5d | %s  // [body omitted]\n"), SignatureLineIdx + 1, *Lines[SignatureLineIdx]);
				SignatureLineIdx = -1;
			}
			else if (Stripped != TEXT("{"))
			{
				// K&R style: brace on the same line as the signature
				FString SigPart = Stripped;
				int32 BraceIdx;
				if (SigPart.FindChar(TEXT('{'), BraceIdx))
				{
					SigPart = SigPart.Left(BraceIdx).TrimEnd();
				}
				if (!SigPart.IsEmpty())
				{
					Result += FString::Printf(TEXT("%5d | %s  // [body omitted]\n"), i + 1, *SigPart);
				}
			}
			continue;
		}

		// --- Depth 0-1: class-level declarations ---

		// Keep class-level braces (class opening/closing)
		if (Stripped == TEXT("{") || Stripped == TEXT("}"))
		{
			if (SignatureLineIdx >= 0)
			{
				Result += FString::Printf(TEXT("%5d | %s\n"), SignatureLineIdx + 1, *Lines[SignatureLineIdx]);
				SignatureLineIdx = -1;
			}
			Result += FString::Printf(TEXT("%5d | %s\n"), i + 1, *Line);
			continue;
		}

		bool bKeep = Stripped.StartsWith(TEXT("public:")) || Stripped.StartsWith(TEXT("protected:")) || Stripped.StartsWith(TEXT("private:"))
			|| Stripped.StartsWith(TEXT("GENERATED")) || Stripped.StartsWith(TEXT("UFUNCTION")) || Stripped.StartsWith(TEXT("UPROPERTY"))
			|| Stripped.StartsWith(TEXT("UENUM")) || Stripped.StartsWith(TEXT("USTRUCT"))
			|| Stripped.StartsWith(TEXT("//")) || Stripped.StartsWith(TEXT("/**")) || Stripped.StartsWith(TEXT("*")) || Stripped.StartsWith(TEXT("*/"))
			|| Stripped.IsEmpty()
			|| Stripped.Contains(TEXT(";"));

		if (bKeep)
		{
			if (SignatureLineIdx >= 0)
			{
				Result += FString::Printf(TEXT("%5d | %s\n"), SignatureLineIdx + 1, *Lines[SignatureLineIdx]);
				SignatureLineIdx = -1;
			}
			Result += FString::Printf(TEXT("%5d | %s\n"), i + 1, *Line);
		}
		else
		{
			// Unrecognized line at class level — could be a function signature (Allman style)
			// Remember it; if next line opens a body (depth→2), emit with [body omitted]
			if (SignatureLineIdx >= 0)
			{
				// Previous pending signature wasn't followed by a body — emit it normally
				Result += FString::Printf(TEXT("%5d | %s\n"), SignatureLineIdx + 1, *Lines[SignatureLineIdx]);
			}
			SignatureLineIdx = i;
		}
	}

	// Flush any remaining pending signature
	if (SignatureLineIdx >= 0)
	{
		Result += FString::Printf(TEXT("%5d | %s\n"), SignatureLineIdx + 1, *Lines[SignatureLineIdx]);
	}

	return Result;
}

FString FMonolithSourceActions::MakeTextResult(const FString& Text)
{
	// Return text as a JSON result with a "text" field
	// (But the registry expects FMonolithActionResult with a JSON object)
	// We'll put it in content[0].text per MCP tool result convention
	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), Text);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return Text; // Unused, but we return the text
}

// ============================================================================
// Tool 1: read_source
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleReadSource(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available. Run source.trigger_reindex first."));
	}

	FString Symbol = Params->GetStringField(TEXT("symbol"));
	bool bIncludeHeader = true;
	if (Params->HasField(TEXT("include_header")))
	{
		bIncludeHeader = Params->GetBoolField(TEXT("include_header"));
	}
	int32 MaxLines = 0;
	if (Params->HasField(TEXT("max_lines")))
	{
		MaxLines = static_cast<int32>(Params->GetNumberField(TEXT("max_lines")));
	}
	bool bMembersOnly = false;
	if (Params->HasField(TEXT("members_only")))
	{
		bMembersOnly = Params->GetBoolField(TEXT("members_only"));
	}

	// Look up by exact name first, then FTS fallback
	TArray<FMonolithSourceSymbol> Symbols = DB->GetSymbolsByName(Symbol);
	if (Symbols.Num() == 0)
	{
		Symbols = DB->SearchSymbolsFTS(Symbol, 5);
	}
	if (Symbols.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No symbol found matching '%s'."), *Symbol));
	}

	// Filter out forward declarations when a real definition exists
	bool bHasDefinition = false;
	for (const auto& Sym : Symbols)
	{
		if (Sym.LineEnd - Sym.LineStart > 1) { bHasDefinition = true; break; }
	}

	if (bHasDefinition)
	{
		TArray<FMonolithSourceSymbol> Filtered;
		for (const auto& Sym : Symbols)
		{
			FString FilePath = DB->GetFilePath(Sym.FileId);
			if (!IsForwardDeclaration(FilePath, Sym.LineStart, Sym.LineEnd))
			{
				Filtered.Add(Sym);
			}
		}
		if (Filtered.Num() > 0) Symbols = Filtered;
	}

	TArray<FString> Parts;
	TSet<FString> SeenFiles;

	for (const auto& Sym : Symbols)
	{
		FString Key = FString::Printf(TEXT("%lld_%d_%d"), Sym.FileId, Sym.LineStart, Sym.LineEnd);
		if (SeenFiles.Contains(Key)) continue;
		SeenFiles.Add(Key);

		FString FilePath = DB->GetFilePath(Sym.FileId);

		if (!bIncludeHeader && FilePath.EndsWith(TEXT(".h")))
		{
			continue;
		}

		FString Header = FString::Printf(TEXT("--- %s (lines %d-%d) ---"), *ShortPath(FilePath), Sym.LineStart, Sym.LineEnd);
		FString Doc;
		if (!Sym.Docstring.IsEmpty())
		{
			Doc = FString::Printf(TEXT("// %s\n"), *Sym.Docstring);
		}

		FString Source;
		if (bMembersOnly && (Sym.Kind == TEXT("class") || Sym.Kind == TEXT("struct")))
		{
			Source = ExtractMembers(FilePath, Sym.LineStart, Sym.LineEnd);
		}
		else
		{
			Source = ReadFileLines(FilePath, Sym.LineStart, Sym.LineEnd);
		}
		Parts.Add(Header + TEXT("\n") + Doc + Source);
	}

	FString ResultText = Parts.Num() > 0
		? FString::Join(Parts, TEXT("\n"))
		: FString::Printf(TEXT("Found symbol '%s' but could not read source files."), *Symbol);

	if (MaxLines > 0)
	{
		TArray<FString> ResultLines;
		ResultText.ParseIntoArrayLines(ResultLines);
		if (ResultLines.Num() > MaxLines)
		{
			int32 Remaining = ResultLines.Num() - MaxLines;
			ResultLines.SetNum(MaxLines);
			ResultText = FString::Join(ResultLines, TEXT("\n"));
			ResultText += FString::Printf(TEXT("\n[...truncated, %d more lines]"), Remaining);
		}
	}

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), ResultText);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Tool 2: find_references
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleFindReferences(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available."));
	}

	FString Symbol = Params->GetStringField(TEXT("symbol"));
	FString RefKind = Params->HasField(TEXT("ref_kind")) ? Params->GetStringField(TEXT("ref_kind")) : TEXT("");
	int32 Limit = Params->HasField(TEXT("limit")) ? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : 50;

	TArray<FMonolithSourceSymbol> Symbols = DB->GetSymbolsByName(Symbol);
	if (Symbols.Num() == 0) Symbols = DB->SearchSymbolsFTS(Symbol, 5);
	if (Symbols.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No symbol found matching '%s'."), *Symbol));
	}

	TArray<FString> Lines;
	for (const auto& Sym : Symbols)
	{
		TArray<FMonolithSourceReference> Refs = DB->GetReferencesTo(Sym.Id, RefKind, Limit);
		for (const auto& Ref : Refs)
		{
			Lines.Add(FString::Printf(TEXT("[%s] %s:%d (from %s)"),
				*Ref.RefKind, *ShortPath(Ref.Path), Ref.Line, *Ref.FromName));
		}
	}

	FString ResultText = Lines.Num() > 0
		? FString::Join(Lines, TEXT("\n"))
		: FString::Printf(TEXT("No references found for '%s'."), *Symbol);

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), ResultText);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Tool 3: find_callers
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleFindCallers(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available."));
	}

	FString Function = Params->GetStringField(TEXT("symbol"));
	int32 Limit = Params->HasField(TEXT("limit")) ? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : 50;

	TArray<FMonolithSourceSymbol> Symbols = DB->GetSymbolsByName(Function, TEXT("function"));
	if (Symbols.Num() == 0)
	{
		TArray<FMonolithSourceSymbol> AllSyms = DB->SearchSymbolsFTS(Function, 5);
		for (const auto& S : AllSyms)
		{
			if (S.Kind == TEXT("function")) Symbols.Add(S);
		}
	}
	if (Symbols.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No function found matching '%s'."), *Function));
	}

	TArray<FString> Lines;
	for (const auto& Sym : Symbols)
	{
		TArray<FMonolithSourceReference> Refs = DB->GetReferencesTo(Sym.Id, TEXT("call"), Limit);
		for (const auto& Ref : Refs)
		{
			Lines.Add(FString::Printf(TEXT("%s \u2014 %s:%d"), *Ref.FromName, *ShortPath(Ref.Path), Ref.Line));
		}
	}

	FString ResultText;
	if (Lines.Num() == 0)
	{
		ResultText = FString::Printf(
			TEXT("No direct C++ callers found for '%s'. This function may be called via delegates, Blueprints, input bindings, or reflection."),
			*Function);
	}
	else
	{
		ResultText = FString::Join(Lines, TEXT("\n"));
	}

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), ResultText);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Tool 4: find_callees
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleFindCallees(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available."));
	}

	FString Function = Params->GetStringField(TEXT("symbol"));
	int32 Limit = Params->HasField(TEXT("limit")) ? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : 50;

	TArray<FMonolithSourceSymbol> Symbols = DB->GetSymbolsByName(Function, TEXT("function"));
	if (Symbols.Num() == 0)
	{
		TArray<FMonolithSourceSymbol> AllSyms = DB->SearchSymbolsFTS(Function, 5);
		for (const auto& S : AllSyms)
		{
			if (S.Kind == TEXT("function")) Symbols.Add(S);
		}
	}
	if (Symbols.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No function found matching '%s'."), *Function));
	}

	TArray<FString> Lines;
	for (const auto& Sym : Symbols)
	{
		TArray<FMonolithSourceReference> Refs = DB->GetReferencesFrom(Sym.Id, TEXT("call"), Limit);
		for (const auto& Ref : Refs)
		{
			Lines.Add(FString::Printf(TEXT("%s \u2014 %s:%d"), *Ref.ToName, *ShortPath(Ref.Path), Ref.Line));
		}
	}

	FString ResultText = Lines.Num() > 0
		? FString::Join(Lines, TEXT("\n"))
		: FString::Printf(TEXT("No callees found for '%s'."), *Function);

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), ResultText);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Tool 5: search_source
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleSearchSource(const TSharedPtr<FJsonObject>& Params)
{
	// Survivor E (plan §3.E) — cursor pagination via rerun-slice.
	//
	// FTS5 rank instability rules out keyset cursors (see plan §8). Instead
	// we rerun the full top-N query at `N = (PageIndex + 1) * Limit`, then
	// slice [PageIndex * Limit, (PageIndex + 1) * Limit). Hard cap of 1000
	// rows total — once the slice end exceeds 1000, no more pages.
	//
	// v1 design note: we use ONE symbol page + ONE source page. The source
	// branch issues an interleaved query across N scopes (header/source/inline
	// OR shader/shader_header OR "all"). Per-scope page tracking would let
	// each scope walk independently, but the plan §3.E body explicitly
	// chooses the simpler single-pair scheme for v1. The interleaved
	// de-dup at the slice site continues to use the existing TSet<FString>
	// keyed on (FileId, LineNumber).

	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available."));
	}

	const FString Query = Params->GetStringField(TEXT("query"));
	const FString Scope = Params->HasField(TEXT("scope")) ? Params->GetStringField(TEXT("scope")) : TEXT("all");
	const int32 RequestedLimit = Params->HasField(TEXT("limit")) ? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : 20;
	const FString Mode = Params->HasField(TEXT("mode")) ? Params->GetStringField(TEXT("mode")) : TEXT("fts");
	const FString Module = Params->HasField(TEXT("module")) ? Params->GetStringField(TEXT("module")) : TEXT("");
	const FString PathFilter = Params->HasField(TEXT("path_filter")) ? Params->GetStringField(TEXT("path_filter")) : TEXT("");
	const FString SymbolKind = Params->HasField(TEXT("symbol_kind")) ? Params->GetStringField(TEXT("symbol_kind")) : TEXT("");
	const FString CursorIn = Params->HasField(TEXT("cursor")) ? Params->GetStringField(TEXT("cursor")) : TEXT("");

	// Hard cap (plan §3.E): never page past row 1000. Cumulative cap.
	// When caller asks for `limit > HARD_CAP_ROWS` (e.g. limit=2000), the
	// FTS query is issued with N=1000 and the returned page is implicitly
	// capped — N is clamped to HARD_CAP_ROWS below.
	constexpr int32 HARD_CAP_ROWS = 1000;

	// Minimum-1 guard. Caller may legitimately ask for limit > HARD_CAP_ROWS;
	// the page slice will fall out short. No upper clamp on `Limit` itself
	// (the row count is bounded by N clamp + slice arithmetic).
	const int32 Limit = FMath::Max(1, RequestedLimit);

	const uint32 CurrentHash = MonolithCursorCodec::ComputeQueryHash(
		Query, Scope, Mode, Module, PathFilter, SymbolKind);

	// Decode cursor (if any). Mismatch / corruption → clean INVALID_CURSOR.
	int32 SymbolPage = 0;
	int32 SourcePage = 0;
	int32 CachedTotalEstimate = -1;
	bool bHasCursor = false;

	if (!CursorIn.IsEmpty())
	{
		MonolithCursorCodec::FCursorState State;
		if (!MonolithCursorCodec::Decode(CursorIn, State))
		{
			TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
			ErrData->SetStringField(TEXT("error_code"), TEXT("INVALID_CURSOR"));
			return FMonolithActionResult::Error(
				TEXT("Cursor decode failed; restart pagination without `cursor`."),
				FMonolithJsonUtils::ErrInvalidParams
			).WithErrorData(ErrData);
		}
		if (State.QueryHash != CurrentHash)
		{
			TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
			ErrData->SetStringField(TEXT("error_code"), TEXT("INVALID_CURSOR"));
			return FMonolithActionResult::Error(
				TEXT("Cursor query mismatch; restart pagination without `cursor`."),
				FMonolithJsonUtils::ErrInvalidParams
			).WithErrorData(ErrData);
		}
		SymbolPage = State.SymbolPage;
		SourcePage = State.SourcePage;
		CachedTotalEstimate = State.CachedTotalEstimate;
		bHasCursor = true;
	}

	const bool bIsPageZero = !bHasCursor;

	// PageIndex shared by both symbol and source rerun (v1 single-pair design).
	const int32 PageIndex = bHasCursor ? FMath::Max(SymbolPage, SourcePage) : 0;

	// N = how many rows we ask the FTS query for, then slice down to the page.
	// Clamp at HARD_CAP_ROWS — once we cross the cap, the next page would be empty.
	const int32 N = FMath::Min((PageIndex + 1) * Limit, HARD_CAP_ROWS);
	const int32 SliceStart = PageIndex * Limit;
	const int32 SliceEnd = FMath::Min(SliceStart + Limit, HARD_CAP_ROWS);

	// Sentinel: if SliceStart is already at/past the cap, return an empty
	// page (terminal). This is the documented overflow path.
	const bool bPastCap = SliceStart >= HARD_CAP_ROWS;

	TArray<FString> Parts;

	// ---------- Symbol FTS rerun-slice ----------
	TArray<FMonolithSourceSymbol> SymResultsAll;
	if (!bPastCap)
	{
		SymResultsAll = DB->SearchSymbolsFTSFiltered(Query, SymbolKind, Module, PathFilter, N);
	}
	const int32 SymSliceStart = FMath::Min(SliceStart, SymResultsAll.Num());
	const int32 SymSliceEnd = FMath::Min(SliceEnd, SymResultsAll.Num());
	const int32 SymRowsThisPage = FMath::Max(0, SymSliceEnd - SymSliceStart);

	if (SymRowsThisPage > 0)
	{
		Parts.Add(TEXT("=== Symbol Matches ==="));
		for (int32 i = SymSliceStart; i < SymSliceEnd; ++i)
		{
			const FMonolithSourceSymbol& Sym = SymResultsAll[i];
			FString FilePath = DB->GetFilePath(Sym.FileId);
			Parts.Add(FString::Printf(TEXT("  [%s] %s (%s:%d)"), *Sym.Kind, *Sym.QualifiedName, *ShortPath(FilePath), Sym.LineStart));
			if (!Sym.Signature.IsEmpty())
			{
				Parts.Add(FString::Printf(TEXT("         %s"), *Sym.Signature));
			}
		}
	}

	// ---------- Source FTS rerun-slice ----------
	TArray<FString> Scopes;
	if (Scope == TEXT("cpp"))
	{
		Scopes = { TEXT("header"), TEXT("source"), TEXT("inline") };
	}
	else if (Scope == TEXT("shaders"))
	{
		Scopes = { TEXT("shader"), TEXT("shader_header") };
	}
	else
	{
		Scopes = { TEXT("all") };
	}

	// Build the full interleaved+de-duped source list at top-N, THEN slice.
	// De-dup happens before slicing so page boundaries land on unique rows.
	TArray<FMonolithSourceChunk> SourceMergedDeduped;
	if (!bPastCap)
	{
		TSet<FString> Seen;
		for (const FString& S : Scopes)
		{
			TArray<FMonolithSourceChunk> ScopeBatch = DB->SearchSourceFTSFiltered(Query, S, Module, PathFilter, N);
			for (const FMonolithSourceChunk& Match : ScopeBatch)
			{
				FString Key = FString::Printf(TEXT("%lld_%d"), Match.FileId, Match.LineNumber);
				if (Seen.Contains(Key)) continue;
				Seen.Add(Key);
				SourceMergedDeduped.Add(Match);
				if (SourceMergedDeduped.Num() >= N)
				{
					break;
				}
			}
			if (SourceMergedDeduped.Num() >= N)
			{
				break;
			}
		}
	}

	const int32 SrcSliceStart = FMath::Min(SliceStart, SourceMergedDeduped.Num());
	const int32 SrcSliceEnd = FMath::Min(SliceEnd, SourceMergedDeduped.Num());
	const int32 SrcRowsThisPage = FMath::Max(0, SrcSliceEnd - SrcSliceStart);

	if (SrcRowsThisPage > 0)
	{
		Parts.Add(TEXT("\n=== Source Line Matches ==="));
		for (int32 i = SrcSliceStart; i < SrcSliceEnd; ++i)
		{
			const FMonolithSourceChunk& Match = SourceMergedDeduped[i];
			FString FilePath = DB->GetFilePath(Match.FileId);
			FString Text = Match.Text.TrimStartAndEnd();
			if (Text.Len() > 120) Text = Text.Left(120) + TEXT("...");
			Parts.Add(FString::Printf(TEXT("  %s:%d"), *ShortPath(FilePath), Match.LineNumber));
			Parts.Add(FString::Printf(TEXT("    %s"), *Text));
		}
	}

	FString ResultText = Parts.Num() > 0
		? FString::Join(Parts, TEXT("\n"))
		: FString::Printf(TEXT("No results found for '%s'."), *Query);

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), ResultText);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);

	// ---------- Pagination envelope ----------
	const int32 TotalRowsThisPage = SymRowsThisPage + SrcRowsThisPage;

	// total_estimate is emitted on page 0 ONLY; threaded forward via the cursor.
	if (bIsPageZero)
	{
		const int32 SymCount = DB->CountSymbolsFTSFiltered(Query, SymbolKind, Module, PathFilter);
		// For source COUNT(*) we issue one count per scope and sum — this matches
		// the rerun's union behavior. De-dup may slightly inflate the estimate
		// vs the actual de-duped page count; documented as ESTIMATE, not exact.
		int32 SrcCount = 0;
		for (const FString& S : Scopes)
		{
			SrcCount += DB->CountSourceFTSFiltered(Query, S, Module, PathFilter);
		}
		CachedTotalEstimate = SymCount + SrcCount;
		ResultObj->SetNumberField(TEXT("total_estimate"), CachedTotalEstimate);
	}
	// On pages 1+: omit total_estimate (caller has it from their cursor's tc field).

	// Emit next_cursor unless:
	//  - this page returned fewer than Limit rows (terminal), OR
	//  - the next slice start would meet/exceed HARD_CAP_ROWS.
	const bool bShortPage = TotalRowsThisPage < Limit;
	const int32 NextSliceStart = SliceEnd; // == (PageIndex + 1) * Limit
	const bool bCapReached = NextSliceStart >= HARD_CAP_ROWS;

	if (!bShortPage && !bCapReached)
	{
		MonolithCursorCodec::FCursorState OutState;
		OutState.QueryHash = CurrentHash;
		OutState.SymbolPage = PageIndex + 1;
		OutState.SourcePage = PageIndex + 1;
		OutState.CachedTotalEstimate = CachedTotalEstimate;
		ResultObj->SetStringField(TEXT("next_cursor"), MonolithCursorCodec::Encode(OutState));
	}
	// else: omit `next_cursor` — terminal page.

	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Tool 6: get_class_hierarchy
// ============================================================================

void FMonolithSourceActions::WalkAncestors(FMonolithSourceDatabase* DB, int64 SymId, TArray<FString>& Lines, int32 Indent, int32 MaxDepth, FHierarchyCounter& Counter, TSet<int64>& Visited)
{
	if (Indent > MaxDepth || Visited.Contains(SymId)) return;
	Visited.Add(SymId);

	TArray<FMonolithSourceInheritance> Parents = DB->GetParents(SymId);
	for (const auto& P : Parents)
	{
		if (Counter.Shown >= Counter.Limit) { Counter.Truncated++; continue; }
		FString Prefix;
		for (int32 i = 0; i < Indent; ++i) Prefix += TEXT("  ");
		Lines.Add(FString::Printf(TEXT("%s<- %s"), *Prefix, *P.Name));
		Counter.Shown++;
		WalkAncestors(DB, P.Id, Lines, Indent + 1, MaxDepth, Counter, Visited);
	}
}

void FMonolithSourceActions::WalkDescendants(FMonolithSourceDatabase* DB, int64 SymId, TArray<FString>& Lines, int32 Indent, int32 MaxDepth, FHierarchyCounter& Counter, TSet<int64>& Visited)
{
	if (Indent > MaxDepth || Visited.Contains(SymId)) return;
	Visited.Add(SymId);

	TArray<FMonolithSourceInheritance> Children = DB->GetChildren(SymId);
	if (Indent >= MaxDepth && Children.Num() > 0) { Counter.Truncated += Children.Num(); return; }

	for (const auto& C : Children)
	{
		if (Counter.Shown >= Counter.Limit) { Counter.Truncated++; continue; }
		FString Prefix;
		for (int32 i = 0; i < Indent; ++i) Prefix += TEXT("  ");
		Lines.Add(FString::Printf(TEXT("%s-> %s"), *Prefix, *C.Name));
		Counter.Shown++;
		WalkDescendants(DB, C.Id, Lines, Indent + 1, MaxDepth, Counter, Visited);
	}
}

FMonolithActionResult FMonolithSourceActions::HandleGetClassHierarchy(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available."));
	}

	FString ClassName = Params->HasField(TEXT("symbol")) ? Params->GetStringField(TEXT("symbol")) : Params->GetStringField(TEXT("class_name"));
	FString Direction = Params->HasField(TEXT("direction")) ? Params->GetStringField(TEXT("direction")) : TEXT("both");
	int32 Depth = Params->HasField(TEXT("depth")) ? static_cast<int32>(Params->GetNumberField(TEXT("depth"))) : 1;

	TArray<FMonolithSourceSymbol> Symbols = DB->GetSymbolsByName(ClassName, TEXT("class"));
	if (Symbols.Num() == 0) Symbols = DB->GetSymbolsByName(ClassName, TEXT("struct"));
	if (Symbols.Num() == 0)
	{
		TArray<FMonolithSourceSymbol> AllSyms = DB->SearchSymbolsFTS(ClassName, 5);
		for (const auto& S : AllSyms)
		{
			if (S.Kind == TEXT("class") || S.Kind == TEXT("struct")) Symbols.Add(S);
		}
	}
	if (Symbols.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No class or struct found matching '%s'."), *ClassName));
	}

	// Filter out forward declarations — prefer real definitions
	bool bHasDefinition = false;
	for (const auto& S : Symbols)
	{
		if (S.LineEnd - S.LineStart > 1) { bHasDefinition = true; break; }
	}
	if (bHasDefinition)
	{
		TArray<FMonolithSourceSymbol> Filtered;
		for (const auto& S : Symbols)
		{
			FString SFilePath = DB->GetFilePath(S.FileId);
			if (!IsForwardDeclaration(SFilePath, S.LineStart, S.LineEnd))
			{
				Filtered.Add(S);
			}
		}
		if (Filtered.Num() > 0) Symbols = Filtered;
	}

	const FMonolithSourceSymbol& Sym = Symbols[0];
	FString FilePath = DB->GetFilePath(Sym.FileId);
	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("%s (%s)"), *Sym.Name, *ShortPath(FilePath)));

	FHierarchyCounter Counter;

	if (Direction == TEXT("ancestors") || Direction == TEXT("both"))
	{
		Lines.Add(TEXT("\nAncestors:"));
		TSet<int64> Visited;
		WalkAncestors(DB, Sym.Id, Lines, 1, Depth, Counter, Visited);
		bool bHasAncestors = false;
		for (const FString& L : Lines) { if (L.Contains(TEXT("<-"))) { bHasAncestors = true; break; } }
		if (!bHasAncestors) Lines.Add(TEXT("  (none)"));
	}

	if (Direction == TEXT("descendants") || Direction == TEXT("both"))
	{
		Lines.Add(TEXT("\nDescendants:"));
		TSet<int64> Visited;
		WalkDescendants(DB, Sym.Id, Lines, 1, Depth, Counter, Visited);
		if (Counter.Truncated > 0)
		{
			Lines.Add(FString::Printf(TEXT("\n  ... and %d more (increase depth to see all)"), Counter.Truncated));
		}
	}

	FString ResultText = FString::Join(Lines, TEXT("\n"));

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), ResultText);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Tool 7: get_module_info
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleGetModuleInfo(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available."));
	}

	FString ModuleName = Params->GetStringField(TEXT("module_name"));

	TOptional<FMonolithSourceModuleStats> Stats = DB->GetModuleStats(ModuleName);
	if (!Stats.IsSet())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No module found matching '%s'."), *ModuleName));
	}

	const FMonolithSourceModuleStats& S = Stats.GetValue();
	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("Module: %s"), *S.Name));
	Lines.Add(FString::Printf(TEXT("Path: %s"), *ShortPath(S.Path)));
	Lines.Add(FString::Printf(TEXT("Type: %s"), *S.ModuleType));
	Lines.Add(FString::Printf(TEXT("Files: %d"), S.FileCount));
	Lines.Add(TEXT(""));
	Lines.Add(TEXT("Symbol counts by kind:"));

	TArray<FString> SortedKinds;
	S.SymbolCounts.GetKeys(SortedKinds);
	SortedKinds.Sort();
	for (const FString& Kind : SortedKinds)
	{
		Lines.Add(FString::Printf(TEXT("  %s: %d"), *Kind, S.SymbolCounts[Kind]));
	}

	// Show key classes
	TArray<FMonolithSourceSymbol> KeyClasses = DB->GetSymbolsInModule(ModuleName, TEXT("class"), 20);
	if (KeyClasses.Num() > 0)
	{
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("Key classes:"));
		for (const auto& Cls : KeyClasses)
		{
			Lines.Add(FString::Printf(TEXT("  %s (line %d)"), *Cls.Name, Cls.LineStart));
		}
	}

	FString ResultText = FString::Join(Lines, TEXT("\n"));

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), ResultText);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Tool 8: get_symbol_context
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleGetSymbolContext(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available."));
	}

	FString Symbol = Params->GetStringField(TEXT("symbol"));
	int32 ContextLines = Params->HasField(TEXT("context_lines")) ? static_cast<int32>(Params->GetNumberField(TEXT("context_lines"))) : 20;

	TArray<FMonolithSourceSymbol> Symbols = DB->GetSymbolsByName(Symbol);
	if (Symbols.Num() == 0) Symbols = DB->SearchSymbolsFTS(Symbol, 5);
	if (Symbols.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No symbol found matching '%s'."), *Symbol));
	}

	TArray<FString> Parts;
	int32 Shown = 0;
	for (const auto& Sym : Symbols)
	{
		if (Shown >= 3) break;

		FString FilePath = DB->GetFilePath(Sym.FileId);
		int32 CtxStart = FMath::Max(1, Sym.LineStart - ContextLines);
		int32 CtxEnd = Sym.LineEnd + ContextLines;

		FString Header = FString::Printf(TEXT("--- %s ---"), *Sym.QualifiedName);
		TArray<FString> InfoParts;
		if (!Sym.Docstring.IsEmpty())
		{
			InfoParts.Add(FString::Printf(TEXT("Docstring: %s"), *Sym.Docstring));
		}
		if (!Sym.Signature.IsEmpty())
		{
			InfoParts.Add(FString::Printf(TEXT("Signature: %s"), *Sym.Signature));
		}
		InfoParts.Add(FString::Printf(TEXT("File: %s (lines %d-%d)"), *ShortPath(FilePath), Sym.LineStart, Sym.LineEnd));

		FString Source = ReadFileLines(FilePath, CtxStart, CtxEnd);
		Parts.Add(Header + TEXT("\n") + FString::Join(InfoParts, TEXT("\n")) + TEXT("\n\n") + Source);
		Shown++;
	}

	FString ResultText = Parts.Num() > 0
		? FString::Join(Parts, TEXT("\n\n"))
		: FString::Printf(TEXT("Found symbol '%s' but could not read source."), *Symbol);

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), ResultText);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Tool 9: read_file
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleReadFile(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available."));
	}

	FString Path = Params->GetStringField(TEXT("file_path"));
	int32 StartLine = Params->HasField(TEXT("start_line")) ? static_cast<int32>(Params->GetNumberField(TEXT("start_line"))) : 1;
	int32 EndLine = Params->HasField(TEXT("end_line")) ? static_cast<int32>(Params->GetNumberField(TEXT("end_line"))) : 0;

	// Resolve path
	FString ResolvedPath;

	// Try as absolute first
	if (FPaths::FileExists(Path))
	{
		ResolvedPath = Path;
	}
	else
	{
		// Normalize separators to backslashes to match DB-stored paths
		FString NormalizedPath = Path;
		NormalizedPath.ReplaceInline(TEXT("/"), TEXT("\\"));

		// Try DB lookup by exact path
		TOptional<FMonolithSourceFile> F = DB->FindFileByPath(NormalizedPath);
		if (F.IsSet())
		{
			ResolvedPath = F->Path;
		}
		else
		{
			// Try suffix match (e.g. "Runtime\Engine\Classes\GameFramework\Actor.h")
			F = DB->FindFileBySuffix(NormalizedPath);
			if (F.IsSet())
			{
				ResolvedPath = F->Path;
			}
		}
	}

	if (ResolvedPath.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No file found matching '%s'."), *Path));
	}

	if (EndLine <= 0)
	{
		EndLine = StartLine + 199;
	}

	FString Header = FString::Printf(TEXT("--- %s (lines %d-%d) ---"), *ShortPath(ResolvedPath), StartLine, EndLine);
	FString Source = ReadFileLines(ResolvedPath, StartLine, EndLine);

	FString ResultText = Header + TEXT("\n") + Source;

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), ResultText);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Bonus: trigger_reindex
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleTriggerReindex(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("Editor not available."));
	}

	UMonolithSourceSubsystem* Subsystem = Cast<UMonolithSourceSubsystem>(GEditor->GetEditorSubsystemBase(UMonolithSourceSubsystem::StaticClass()));
	if (!Subsystem)
	{
		return FMonolithActionResult::Error(TEXT("MonolithSourceSubsystem not available."));
	}

	if (Subsystem->IsIndexing())
	{
		return FMonolithActionResult::Error(TEXT("Indexing already in progress."));
	}

	Subsystem->TriggerReindex();

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), TEXT("Full source indexing started (engine + project). This runs in the background — check editor log for progress."));
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// trigger_project_reindex
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleTriggerProjectReindex(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("Editor not available."));
	}

	UMonolithSourceSubsystem* Subsystem = Cast<UMonolithSourceSubsystem>(GEditor->GetEditorSubsystemBase(UMonolithSourceSubsystem::StaticClass()));
	if (!Subsystem)
	{
		return FMonolithActionResult::Error(TEXT("MonolithSourceSubsystem not available."));
	}

	if (Subsystem->IsIndexing())
	{
		return FMonolithActionResult::Error(TEXT("Indexing already in progress."));
	}

	Subsystem->TriggerProjectReindex();

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), TEXT("Project source indexing started (incremental). This runs in the background — check editor log for progress."));
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

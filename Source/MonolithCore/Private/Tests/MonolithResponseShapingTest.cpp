// SPDX-License-Identifier: MIT
// Survivor B (Response Shaping) automation tests — plan §12 "Survivor B".
// Plan: Plugins/Monolith/Docs/plans/2026-05-27-mcp-llm-ergonomics.md
//
// DEVIATION NOTE: plan §6 file-table specifies `Source/MonolithCore/Tests/...`.
// This file lives under `Source/MonolithCore/Private/Tests/...` instead so UBT's
// auto-include of `Private/` picks it up without a Build.cs change. Matches the
// existing precedent at `Private/Reflection/Tests/MonolithReflectionWalkerTest.cpp`.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformMisc.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithResponseShapingTestDetail
{
	/** Build a response object with: name, debug_info, empty_array, empty_obj, null_field, empty_string. */
	static TSharedPtr<FJsonObject> MakeKitchenSinkResponse()
	{
		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("name"), TEXT("Bob"));
		R->SetStringField(TEXT("debug_info"), TEXT("verbose-blob"));
		R->SetArrayField(TEXT("empty_array"), TArray<TSharedPtr<FJsonValue>>());
		R->SetObjectField(TEXT("empty_obj"), MakeShared<FJsonObject>());
		R->SetField(TEXT("null_field"), MakeShared<FJsonValueNull>());
		R->SetStringField(TEXT("empty_string"), TEXT(""));
		R->SetNumberField(TEXT("answer"), 42);
		return R;
	}

	static TSharedPtr<FJsonObject> MakeParams()
	{
		return MakeShared<FJsonObject>();
	}

	static void SetStringArray(TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, std::initializer_list<const TCHAR*> Items)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const TCHAR* I : Items)
		{
			Arr.Add(MakeShared<FJsonValueString>(FString(I)));
		}
		Obj->SetArrayField(Key, Arr);
	}
}

// ---------------------------------------------------------------------------
// Test 1: Top-level whitelist via _fields:["name"].
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithResponseShapingFieldsWhitelistTest,
	"Monolith.ResponseShaping.FieldsWhitelist",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingFieldsWhitelistTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithResponseShapingTestDetail;

	TSharedPtr<FJsonObject> Response = MakeKitchenSinkResponse();
	TSharedPtr<FJsonObject> Params = MakeParams();
	SetStringArray(Params, TEXT("_fields"), {TEXT("name")});

	TArray<FString> Warnings;
	ApplyResponseShaping(Response, Params, Warnings);

	TestTrue(TEXT("name retained"), Response->HasField(TEXT("name")));
	TestFalse(TEXT("debug_info dropped"), Response->HasField(TEXT("debug_info")));
	TestFalse(TEXT("answer dropped"), Response->HasField(TEXT("answer")));
	TestEqual(TEXT("no warnings emitted"), Warnings.Num(), 0);
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: Top-level blacklist via _omit:["debug_info"].
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithResponseShapingOmitBlacklistTest,
	"Monolith.ResponseShaping.OmitBlacklist",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingOmitBlacklistTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithResponseShapingTestDetail;

	TSharedPtr<FJsonObject> Response = MakeKitchenSinkResponse();
	TSharedPtr<FJsonObject> Params = MakeParams();
	SetStringArray(Params, TEXT("_omit"), {TEXT("debug_info")});

	TArray<FString> Warnings;
	ApplyResponseShaping(Response, Params, Warnings);

	TestFalse(TEXT("debug_info removed"), Response->HasField(TEXT("debug_info")));
	TestTrue(TEXT("name preserved"), Response->HasField(TEXT("name")));
	TestTrue(TEXT("answer preserved"), Response->HasField(TEXT("answer")));
	TestEqual(TEXT("no warnings emitted"), Warnings.Num(), 0);
	return true;
}

// ---------------------------------------------------------------------------
// Test 3: Mutually-exclusive — both _fields and _omit populated.
// Per plan §3.B: _fields wins, _omit ignored, warning emitted.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithResponseShapingMutuallyExclusiveTest,
	"Monolith.ResponseShaping.MutuallyExclusive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingMutuallyExclusiveTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithResponseShapingTestDetail;

	TSharedPtr<FJsonObject> Response = MakeKitchenSinkResponse();
	TSharedPtr<FJsonObject> Params = MakeParams();
	SetStringArray(Params, TEXT("_fields"), {TEXT("name")});
	SetStringArray(Params, TEXT("_omit"),   {TEXT("name")});

	TArray<FString> Warnings;
	ApplyResponseShaping(Response, Params, Warnings);

	// _fields wins: only `name` survives.
	TestTrue(TEXT("name retained because _fields wins"), Response->HasField(TEXT("name")));
	TestFalse(TEXT("answer dropped by _fields whitelist"), Response->HasField(TEXT("answer")));
	TestTrue(TEXT("exactly one warning emitted"), Warnings.Num() == 1);
	if (Warnings.Num() == 1)
	{
		TestTrue(TEXT("warning text mentions mutually exclusive"),
			Warnings[0].Contains(TEXT("mutually exclusive")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 4: Empty _fields: [] — no-op (does NOT drop everything).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithResponseShapingEmptyFieldsNoOpTest,
	"Monolith.ResponseShaping.EmptyFieldsNoOp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingEmptyFieldsNoOpTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithResponseShapingTestDetail;

	TSharedPtr<FJsonObject> Response = MakeKitchenSinkResponse();
	const int32 InitialKeys = Response->Values.Num();

	TSharedPtr<FJsonObject> Params = MakeParams();
	Params->SetArrayField(TEXT("_fields"), TArray<TSharedPtr<FJsonValue>>()); // empty array

	TArray<FString> Warnings;
	ApplyResponseShaping(Response, Params, Warnings);

	TestEqual(TEXT("all keys preserved with empty _fields"), Response->Values.Num(), InitialKeys);
	TestTrue(TEXT("name still present"), Response->HasField(TEXT("name")));
	TestTrue(TEXT("debug_info still present"), Response->HasField(TEXT("debug_info")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 5: _compact_json:true drops null + empty string + empty array + empty object.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithResponseShapingCompactJsonTest,
	"Monolith.ResponseShaping.CompactJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingCompactJsonTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithResponseShapingTestDetail;

	TSharedPtr<FJsonObject> Response = MakeKitchenSinkResponse();
	TSharedPtr<FJsonObject> Params = MakeParams();
	Params->SetBoolField(TEXT("_compact_json"), true);

	TArray<FString> Warnings;
	ApplyResponseShaping(Response, Params, Warnings);

	TestFalse(TEXT("empty_array dropped"),   Response->HasField(TEXT("empty_array")));
	TestFalse(TEXT("empty_obj dropped"),     Response->HasField(TEXT("empty_obj")));
	TestFalse(TEXT("null_field dropped"),    Response->HasField(TEXT("null_field")));
	TestFalse(TEXT("empty_string dropped"),  Response->HasField(TEXT("empty_string")));
	TestTrue (TEXT("name retained"),         Response->HasField(TEXT("name")));
	TestTrue (TEXT("debug_info retained"),   Response->HasField(TEXT("debug_info")));
	TestTrue (TEXT("answer (number) retained"), Response->HasField(TEXT("answer")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 6: K3 STRICT_PARAMS smoke — universal allowlist must include
// _fields / _omit / _compact_json so STRICT_PARAMS=1 does not hard-fail.
//
// Registers a throwaway action under namespace "monolith_test_shaping" with
// a one-string-param schema; dispatches with `_fields: ["name"]` extra; asserts
// Success (no "Unknown param" hard-fail). Cleans up via UnregisterNamespace.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithResponseShapingStrictParamsAllowlistTest,
	"Monolith.ResponseShaping.StrictParamsAllowlist",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingStrictParamsAllowlistTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithResponseShapingTestDetail;

	const FString TestNs = TEXT("monolith_test_shaping");
	const FString TestAction = TEXT("ping");

	// Set STRICT_PARAMS=1 for the duration of this test, restore after.
	const FString PriorEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("STRICT_PARAMS"));
	FPlatformMisc::SetEnvironmentVar(TEXT("STRICT_PARAMS"), TEXT("1"));
	ON_SCOPE_EXIT
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("STRICT_PARAMS"), *PriorEnv);
		FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("monolith_test_shaping"));
	};

	// Register a tiny throwaway action that just echoes back {"ok": true, "name": <input>}.
	TSharedPtr<FJsonObject> Schema = FParamSchemaBuilder()
		.Required(TEXT("name"), TEXT("string"), TEXT("Throwaway test param."))
		.Build();

	auto Handler = FMonolithActionHandler::CreateLambda(
		[](const TSharedPtr<FJsonObject>& Params) -> FMonolithActionResult
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("ok"), true);
			FString Name;
			Params->TryGetStringField(TEXT("name"), Name);
			Result->SetStringField(TEXT("name"), Name);
			Result->SetStringField(TEXT("debug_info"), TEXT("noise"));
			return FMonolithActionResult::Success(Result);
		});

	FMonolithToolRegistry::Get().RegisterAction(TestNs, TestAction, TEXT("Test."), Handler, Schema);

	// Dispatch with `_fields: ["name"]` — must NOT trip STRICT_PARAMS=1 rejection.
	TSharedPtr<FJsonObject> CallParams = MakeShared<FJsonObject>();
	CallParams->SetStringField(TEXT("name"), TEXT("Alice"));
	SetStringArray(CallParams, TEXT("_fields"), {TEXT("name")});

	FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(TestNs, TestAction, CallParams);

	TestTrue(TEXT("STRICT_PARAMS=1 did not reject the _fields universal param"), R.bSuccess);
	if (R.bSuccess && R.Result.IsValid())
	{
		// And the shaping pass did its job — only `name` should remain (plus possibly warnings).
		TestTrue(TEXT("name field present"), R.Result->HasField(TEXT("name")));
		TestFalse(TEXT("debug_info filtered by _fields"), R.Result->HasField(TEXT("debug_info")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

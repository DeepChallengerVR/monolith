#include "MonolithJsonUtils.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"

DEFINE_LOG_CATEGORY(LogMonolith);

TSharedPtr<FJsonObject> FMonolithJsonUtils::SuccessResponse(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonValue>& Result)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	if (Id.IsValid())
	{
		Response->SetField(TEXT("id"), Id);
	}
	if (Result.IsValid())
	{
		Response->SetField(TEXT("result"), Result);
	}
	else
	{
		Response->SetField(TEXT("result"), MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
	}
	return Response;
}

TSharedPtr<FJsonObject> FMonolithJsonUtils::ErrorResponse(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message, const TSharedPtr<FJsonValue>& Data)
{
	TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
	ErrorObj->SetNumberField(TEXT("code"), Code);
	ErrorObj->SetStringField(TEXT("message"), Message);
	if (Data.IsValid())
	{
		ErrorObj->SetField(TEXT("data"), Data);
	}

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	if (Id.IsValid())
	{
		Response->SetField(TEXT("id"), Id);
	}
	else
	{
		Response->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
	}
	Response->SetObjectField(TEXT("error"), ErrorObj);
	return Response;
}

TSharedPtr<FJsonObject> FMonolithJsonUtils::SuccessObject(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& ResultObj)
{
	return SuccessResponse(Id, MakeShared<FJsonValueObject>(ResultObj));
}

TSharedPtr<FJsonObject> FMonolithJsonUtils::SuccessString(const TSharedPtr<FJsonValue>& Id, const FString& Message)
{
	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("message"), Message);
	return SuccessObject(Id, ResultObj);
}

FString FMonolithJsonUtils::Serialize(const TSharedPtr<FJsonObject>& JsonObject)
{
	FString OutputString;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
	return OutputString;
}

TSharedPtr<FJsonObject> FMonolithJsonUtils::Parse(const FString& JsonString)
{
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject))
	{
		return nullptr;
	}
	return JsonObject;
}

TSharedRef<FJsonValueArray> FMonolithJsonUtils::StringArrayToJson(const TArray<FString>& Strings)
{
	TArray<TSharedPtr<FJsonValue>> JsonArray;
	for (const FString& Str : Strings)
	{
		JsonArray.Add(MakeShared<FJsonValueString>(Str));
	}
	return MakeShared<FJsonValueArray>(JsonArray);
}

// =============================================================================
//  Survivor B — Universal Response Shaping
//
//  Phase 1 of plan §3.B (Docs/plans/2026-05-27-mcp-llm-ergonomics.md).
//  TOP-LEVEL KEYS ONLY. JSONPath / nested traversal is out-of-scope (plan §2).
// =============================================================================

namespace MonolithResponseShapingDetail
{
	/** Read a string-array param into a TSet for O(1) membership. Returns false if absent or empty. */
	static bool ReadStringArrayParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, TSet<FString>& Out)
	{
		Out.Reset();
		if (!Params.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Params->TryGetArrayField(Key, Arr) || !Arr)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			FString S;
			if (V.IsValid() && V->TryGetString(S))
			{
				Out.Add(S);
			}
		}
		return Out.Num() > 0;
	}

	/** A value counts as "empty" for _compact_json if it is null, "", {}, or []. Numbers/bools/nonempty pass. */
	static bool IsEmptyForCompact(const TSharedPtr<FJsonValue>& Val)
	{
		if (!Val.IsValid() || Val->IsNull())
		{
			return true;
		}
		switch (Val->Type)
		{
		case EJson::String:
		{
			FString S;
			Val->TryGetString(S);
			return S.IsEmpty();
		}
		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (Val->TryGetArray(Arr) && Arr)
			{
				return Arr->Num() == 0;
			}
			return true;
		}
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Val->TryGetObject(Obj) && Obj && (*Obj).IsValid())
			{
				return (*Obj)->Values.Num() == 0;
			}
			return true;
		}
		default:
			return false; // numbers / bools always retained
		}
	}
}

void ApplyResponseShaping(
	TSharedPtr<FJsonObject>& Response,
	const TSharedPtr<FJsonObject>& Params,
	TArray<FString>& Warnings)
{
	if (!Response.IsValid() || !Params.IsValid())
	{
		return;
	}

	TSet<FString> FieldsSet;
	TSet<FString> OmitSet;
	const bool bHasFields = MonolithResponseShapingDetail::ReadStringArrayParam(Params, TEXT("_fields"), FieldsSet);
	const bool bHasOmit   = MonolithResponseShapingDetail::ReadStringArrayParam(Params, TEXT("_omit"),   OmitSet);

	bool bCompact = false;
	Params->TryGetBoolField(TEXT("_compact_json"), bCompact);

	// Mutually exclusive: _fields wins, _omit ignored, warn the caller.
	bool bApplyOmit = bHasOmit;
	if (bHasFields && bHasOmit)
	{
		Warnings.Add(TEXT("`_fields` and `_omit` are mutually exclusive; honoring `_fields`, ignoring `_omit`."));
		bApplyOmit = false;
	}

	// _fields whitelist — retain only matching top-level keys.
	if (bHasFields)
	{
		TArray<FString> Existing;
		Response->Values.GetKeys(Existing);
		for (const FString& K : Existing)
		{
			if (!FieldsSet.Contains(K))
			{
				Response->RemoveField(K);
			}
		}
	}
	// _omit blacklist — drop matching top-level keys (only when _fields not active).
	else if (bApplyOmit)
	{
		for (const FString& K : OmitSet)
		{
			Response->RemoveField(K);
		}
	}

	// _compact_json — drop top-level keys whose value is null/""/{}/[]. Runs AFTER fields/omit.
	if (bCompact)
	{
		TArray<FString> Existing;
		Response->Values.GetKeys(Existing);
		for (const FString& K : Existing)
		{
			const TSharedPtr<FJsonValue> Val = Response->TryGetField(K);
			if (MonolithResponseShapingDetail::IsEmptyForCompact(Val))
			{
				Response->RemoveField(K);
			}
		}
	}
}

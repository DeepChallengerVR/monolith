#pragma once
#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Motion Matching Action Pack — Sprint 5 (Pillar D): character/actor Blueprint
 * scaffolding for a UE 5.7 Motion Matching (Pose Search) setup.
 *
 * Namespace: "blueprint". Registered from FMonolithBlueprintModule::StartupModule.
 *
 * These actions wire an AnimBP + CharacterMovementComponent preset + Enhanced Input
 * onto a character Blueprint. Trajectory is AnimBP-side (Pose History node
 * bGenerateTrajectory) — there is NO CharacterTrajectoryComponent in 5.7, so
 * add_engine_component_typed deliberately does NOT special-case it.
 *
 * All handlers run on the game thread (MCP dispatch). Editor-time asset authoring
 * only — nothing replicated.
 */
class FMonolithMotionMatchingScaffoldActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// 5.1 — point a Blueprint's skeletal mesh component at an AnimBP's generated class.
	static FMonolithActionResult HandleSetAnimClass(const TSharedPtr<FJsonObject>& Params);

	// 5.2 — apply a CharacterMovementComponent locomotion preset to the BP CDO.
	static FMonolithActionResult HandleApplyMovementPreset(const TSharedPtr<FJsonObject>& Params);

	// 5.3 — resolve any UActorComponent subclass by friendly name and add it.
	static FMonolithActionResult HandleAddEngineComponentTyped(const TSharedPtr<FJsonObject>& Params);

	// 5.4 — create an IMC + per-action IA assets and bind them to AddMovementInput.
	static FMonolithActionResult HandleScaffoldLocomotionInput(const TSharedPtr<FJsonObject>& Params);

	// 5.5 — reflection-walk the AnimBP exposed-variable contract vs the BP variables. No mutation.
	static FMonolithActionResult HandleValidateAnimBpVariableContract(const TSharedPtr<FJsonObject>& Params);

	// 5.6 — COMPOSITE: create/reparent BP, set mesh, then call 5.1 + 5.2.
	static FMonolithActionResult HandleScaffoldMotionMatchingCharacter(const TSharedPtr<FJsonObject>& Params);
};

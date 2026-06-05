#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * AnimBlueprint graph-surgery actions for Monolith (animation namespace).
 *
 * Higher-risk graph repair kept separate from the stable MonolithAbpWriteActions
 * wiring surface so the reflective-K2Node and slice-removal work can be rolled
 * back as a unit.
 *
 * 5 actions:
 *   - rebuild_evaluate_chooser_node   (WITH_CHOOSER)
 *   - replace_evaluate_chooser_nodes  (WITH_CHOOSER)
 *   - duplicate_reparent_and_sanitize
 *   - find_node_slice
 *   - remove_node_slice
 *
 * The two Evaluate-Chooser actions spawn UK2Node_EvaluateChooser2 reflectively
 * (its header lives in a non-exported ChooserUncooked/Private path and cannot be
 * included). They are gated behind WITH_CHOOSER; off-gate the handlers return a
 * clean "Chooser plugin not available" error rather than failing to link.
 */
class MONOLITHANIMATION_API FMonolithAbpGraphSurgeryActions
{
public:
	/** Register all graph-surgery actions with the tool registry. Always registers; chooser handlers gate internally. */
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	// F10 — reflective Evaluate-Chooser node surgery (WITH_CHOOSER)
	static FMonolithActionResult HandleRebuildEvaluateChooserNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleReplaceEvaluateChooserNodes(const TSharedPtr<FJsonObject>& Params);

	// F11 — duplicate + reparent + dependency classification
	static FMonolithActionResult HandleDuplicateReparentAndSanitize(const TSharedPtr<FJsonObject>& Params);

	// F12 — directional node-slice compute / removal
	static FMonolithActionResult HandleFindNodeSlice(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveNodeSlice(const TSharedPtr<FJsonObject>& Params);
};

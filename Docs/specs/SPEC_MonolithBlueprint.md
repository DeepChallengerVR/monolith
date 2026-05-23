# Monolith — MonolithBlueprint Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.10 (Beta)

---

## MonolithBlueprint

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, BlueprintGraph, Json, JsonUtilities

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithBlueprintModule` | Registers 92 blueprint actions |
| `FMonolithBlueprintActions` | Static handlers. Uses `FMonolithAssetUtils::LoadAssetByPath<UBlueprint>` |
| `MonolithBlueprintInternal` | Helpers: AddGraphArray, FindGraphByName, PinTypeToString, SerializePin/Node, TraceExecFlow, FindEntryNode |

### Actions (92 — namespace: "blueprint")

**Read Actions (14)**
| Action | Params | Description |
|--------|--------|-------------|
| `list_graphs` | `asset_path` | List all graphs with name/type/node_count. Graph types: event_graph, function, macro, delegate_signature |
| `get_graph_summary` | `asset_path`, `graph_name` | Lightweight graph overview: node id/class/title + exec connections only (~10KB vs 172KB for full data) |
| `get_graph_data` | `asset_path`, `graph_name`, `node_class_filter` | Full graph with all nodes, pins (17+ type categories), connections, positions. Optional class filter |
| `get_variables` | `asset_path` | All NewVariables: name, type (with container prefix), default (from CDO), category, flags (editable, read_only, expose_on_spawn, replicated, transient) |
| `get_cdo_properties` | `asset_path`, `category_filter?`, `include_parent_defaults?`, `owner_class_filter?`, `name_pattern?`, `exclude_categories?` | Reflects all CDO properties of a Blueprint class with current default values. Optional filters compose: `category_filter` (case-insensitive substring on `Category` metadata), `include_parent_defaults` (bool, walks parent CDO chain), `owner_class_filter` (case-insensitive substring on owner class name — skips inherited `AActor`/`APawn`/`ACharacter` in one parameter, PR #57), `name_pattern` (case-insensitive substring on property name, PR #57), `exclude_categories` (string array, case-insensitive exact match against `Category` — e.g. `["Replication", "Cooking", "HLOD"]`, PR #57). All filter params default to `null`/empty (no-op). Cuts JSON payload by ~90% in typical AActor-subclass inspection flows. |
| `get_execution_flow` | `asset_path`, `entry_point` | Linearized exec trace from entry point. Handles branching (multiple exec outputs). MaxDepth=100 |
| `search_nodes` | `asset_path`, `query` | Case-insensitive search by title, class name, or function name |
| `get_components` | `asset_path` | List all components in the component hierarchy |
| `get_component_details` | `asset_path`, `component_name` | Full property reflection for a named component |
| `get_functions` | `asset_path` | List all functions with signatures, access, and purity flags |
| `get_event_dispatchers` | `asset_path` | List all event dispatchers with parameter signatures |
| `get_parent_class` | `asset_path` | Return the parent class of the Blueprint |
| `get_interfaces` | `asset_path` | List all implemented interfaces |
| `get_construction_script` | `asset_path` | Get the construction script graph |

**Variable CRUD (7)**
| Action | Params | Description |
|--------|--------|-------------|
| `add_variable` | `asset_path`, `variable_name`, `variable_type` | Add a new variable to the Blueprint |
| `remove_variable` | `asset_path`, `variable_name` | Remove a variable by name |
| `rename_variable` | `asset_path`, `old_name`, `new_name` | Rename a variable |
| `set_variable_type` | `asset_path`, `variable_name`, `variable_type` | Change a variable's type |
| `set_variable_defaults` | `asset_path`, `variable_name`, `default_value` | Set a variable's default value |
| `add_local_variable` | `asset_path`, `function_name`, `variable_name`, `variable_type` | Add a local variable inside a function graph |
| `remove_local_variable` | `asset_path`, `function_name`, `variable_name` | Remove a local variable from a function graph |

**Component CRUD (6)**
| Action | Params | Description |
|--------|--------|-------------|
| `add_component` | `asset_path`, `component_class`, `component_name` | Add a component to the Blueprint |
| `remove_component` | `asset_path`, `component_name` | Remove a component by name |
| `rename_component` | `asset_path`, `old_name`, `new_name` | Rename a component |
| `reparent_component` | `asset_path`, `component_name`, `new_parent` | Change a component's parent in the hierarchy |
| `set_component_property` | `asset_path`, `component_name`, `property_name`, `value` | Set a property on a component via reflection |
| `duplicate_component` | `asset_path`, `component_name`, `new_name` | Duplicate a component with all its settings |

**Graph Management (10)**
| Action | Params | Description |
|--------|--------|-------------|
| `add_function` | `asset_path`, `function_name` | Add a new function graph |
| `override_parent_function` | `asset_path`, `parent_function_name` | Author a Blueprint override of an overridable parent function (`BlueprintImplementableEvent` / `BlueprintNativeEvent`), including those that RETURN a value (e.g. `UCommonActivatableWidget::BP_GetDesiredFocusTarget` → `UWidget*`) — which `add_function` cannot do and the event-node form has no ReturnValue pin for. Declaring class resolved generically by name. Returns `graph_name`, `entry_node_id`, `return_pin_id`, `return_pin_name`, `override_class`, `has_return_value` |
| `remove_function` | `asset_path`, `function_name` | Remove a function graph |
| `rename_function` | `asset_path`, `old_name`, `new_name` | Rename a function graph |
| `add_macro` | `asset_path`, `macro_name` | Add a new macro graph |
| `add_event_dispatcher` | `asset_path`, `dispatcher_name` | Add a new event dispatcher |
| `set_function_params` | `asset_path`, `function_name`, `params` | Set input/output parameters on a function |
| `implement_interface` | `asset_path`, `interface_class` | Add an interface to the Blueprint |
| `remove_interface` | `asset_path`, `interface_class` | Remove an interface from the Blueprint |
| `reparent_blueprint` | `asset_path`, `new_parent_class` | Change the Blueprint's parent class |

**Node & Pin Operations (7)**
| Action | Params | Description |
|--------|--------|-------------|
| `add_node` | `asset_path`, `graph_name`, `node_class`, `position` | Add a node to a graph. Accepts common aliases (e.g. `CallFunction`, `VariableGet`, `ComponentBoundEvent`, `AddDelegate`, `RemoveDelegate`, `ClearDelegate`, `CallDelegate`) and tries `K2_` prefix fallback for function calls |
| `remove_node` | `asset_path`, `graph_name`, `node_id` | Remove a node by ID |
| `connect_pins` | `asset_path`, `graph_name`, `source_node`, `source_pin`, `target_node`, `target_pin` | Connect two pins |
| `disconnect_pins` | `asset_path`, `graph_name`, `source_node`, `source_pin`, `target_node`, `target_pin` | Disconnect two pins |
| `set_pin_default` | `asset_path`, `graph_name`, `node_id`, `pin_name`, `value` | Set a pin's default value. For PC_Class / PC_Object pins, `value` is resolved to `Pin->DefaultObject`: accepts native class names, object/class paths, or Blueprint class paths (with or without `_C` suffix). Type-checked against `PinSubCategoryObject`. |
| `set_node_position` | `asset_path`, `graph_name`, `node_id`, `x`, `y` | Set a node's position in the graph |
| `add_property_access` | `asset_path`, `graph_name`?, `member_class`, `member_name`, `is_setter`? | Author a cross-class `VariableGet`/`Set` reading/writing a UPROPERTY on an arbitrary foreign class (resolved by string via `FindFirstObject<UClass>` — never hardcoded). Calls `FMemberReference::SetExternalMember` then `AllocateDefaultPins` so the value pin resolves to the property's real type (not wildcard); resolves to the declaring class up the hierarchy. Sets a valid NodeGuid. Returns `node_id`, `value_pin_id`, `target_pin_id`. Unblocks reading a property off a passed-in object reference (e.g. iterate array-of-foreign-structs). `target_class` accepted as alias for `member_class`. |

**NodeGuid note (#15):** Every MCP node-creation path above (`add_node`, `add_event_node`, `add_timeline`, `promote_pin_to_variable`, `add_comment_node`, and `add_property_access`) calls `UEdGraphNode::CreateNewGuid()` after creation, so MCP-authored nodes carry valid GUIDs and no longer risk invalid-GUID warnings on compile/save.

**Compile & Create (6)**
| Action | Params | Description |
|--------|--------|-------------|
| `compile_blueprint` | `asset_path` | Compile the Blueprint and return errors/warnings |
| `save_dirty_assets` | `path_prefix`? (default `/Game`; empty string saves all) | Save ALL currently-dirty Blueprint and Widget Blueprint packages in one sweep — closes the data-loss window after a batch of edit actions that dirty packages but do not persist them. Returns `saved[]`, `failed[]`, `count` |
| `validate_blueprint` | `asset_path` | Validate Blueprint without full compile — checks for broken references and missing overrides |
| `create_blueprint` | `save_path`, `parent_class` | Create a new Blueprint asset |
| `duplicate_blueprint` | `asset_path`, `new_path` | Duplicate a Blueprint asset to a new path |
| `get_dependencies` | `asset_path` | List all hard and soft asset dependencies |

**Layout (1)**
| Action | Params | Description |
|--------|--------|-------------|
| `auto_layout` | `asset_path`, `graph_name`?, `formatter`? | Auto-arrange nodes in a Blueprint graph. `formatter`: `"auto"` (default) — uses Blueprint Assist if available, falls back to built-in hierarchical layout; `"blueprint_assist"` — requires BA, errors if not present; `"builtin"` — built-in layout only |

**Spawn (2)**
| Action | Params | Description |
|--------|--------|-------------|
| `spawn_blueprint_actor` | `blueprint`, `location`?, `rotation`?, `scale`?, `label`?, `folder`?, `properties`?, `tags`?, `sublevel`?, `mobility`?, `select`? | Spawn a Blueprint actor into the editor world with full transform, property reflection, tags, sublevel targeting, and mobility control. Uses `GEditor->AddActor` for proper editor integration (undo/redo). Default folder: `"Blueprints"` |
| `batch_spawn_blueprint_actors` | `blueprint`, `count`, `pattern`?, `origin`?, `spacing`?, `columns`?, `direction`?, `rotation`?, `scale`?, `label_prefix`?, `folder`?, `properties`?, `tags`?, `sublevel`?, `mobility`?, `select`? | Spawn multiple Blueprint actors in a grid or linear pattern. Partial failure semantics — continues on per-actor failure, reports successes and failures separately. Single undo transaction. Max 1000 |

**CDO Bulk Fill / Describe (Phase 1 of bulk_fill framework — 2)**
| Action | Params | Description |
|--------|--------|-------------|
| `set_cdo_properties` | `asset_path`, `properties`, `dry_run`?, `strict`? | Bulk-fill multiple CDO properties from a JSON tree in a single transaction. `properties` is a nested JSON object whose keys are UPROPERTY names and values are scalars / structs / arrays / maps / sets per the FProperty reflection schema. Routes through the central `FMonolithBulkFillRegistry` "blueprint" adapter. Same dual-path asset load as `set_cdo_property` (Blueprint CDO first, then generic UObject — DataAsset, DataTable, GameplayEffect, AbilitySet, InputAction). Engine edit cradle (transaction → Modify → PreEditChange → write → ReparentTransientInstancedSubobjects → FireFullCradle) preserved per Issue #29. `dry_run=true` walks the tree via `FMonolithReflectionWalker::InspectTree` WITHOUT mutation and returns the full `FDryRunReport`. `strict=true` promotes silent drops / clamps / unknown-fields to hard errors and cancels the transaction. |
| `describe_cdo_schema` | `asset_path` | Return the rich `FSchemaDescriptor` tree (type names, ImportText forms, enum-value lists, clamp ranges, nested struct/array/map children) for an asset's CDO. Counterpart to `get_cdo_properties` when the caller needs the schema, not the current values. Use before authoring `set_cdo_property` / `set_cdo_properties` payloads to discover the legal ImportText grammar. |

**Existing CDO actions also gain `dry_run` + `strict` optional params:**
- `set_cdo_property` — Phase 1 adds `dry_run`?, `strict`? optional params. When `dry_run=true`, validates the proposed write via the reflection walker and returns the per-field report without entering the engine edit cradle. Same `strict` semantics as the plural action.

### Bulk Fill & Describe Surface (2026-05-11)

`MonolithBlueprintBulkFillAdapter` registers under `FMonolithBulkFillRegistry` for the `blueprint` namespace, exposed via the framework-level `bulk_fill_query("apply", ...)` and `describe_query("schema", ...)` dispatchers. Phase 1 of the MCP ergonomics rollout (design spec `Docs/plans/2026-05-11-monolith-mcp-ergonomics-design.md`, implementation plan `Docs/plans/2026-05-11-monolith-mcp-ergonomics.md`).

**Surface summary.** `bulk_fill_query("apply", target_namespace="blueprint", target="<asset_path>", tree={...}, dry_run=<bool>, strict=<bool>)` walks the JSON tree against the target asset's CDO reflection schema and either commits atomically (under the standard Blueprint edit cradle preserved per Issue #29) or fails with a per-key error map. `describe_query("schema", target_namespace="blueprint", target="<asset_path>")` returns the settable CDO surface — UPROPERTY names, UE reflection types, ImportText forms, enum value lists, clamp ranges, nested struct/array/map/set children.

**fill_kind catalogue (1 — enumerated against `MonolithBlueprintBulkFillAdapter.cpp`):**

| `fill_kind` | Target shape | Walks |
|---|---|---|
| `CDOProperties` | Blueprint asset OR generic UObject (DataAsset, DataTable, GameplayEffect, AbilitySet, InputAction) | nested JSON object keyed by UPROPERTY names against the asset's CDO; dual-path asset load tries `LoadAssetByPath<UBlueprint>` then falls back to generic UObject `StaticLoadObject` |

**Adapter convenience surface.** The two `blueprint::set_cdo_properties` / `blueprint::describe_cdo_schema` actions documented above are aliases routed through this same adapter — they exist for symmetry with the read-side `get_cdo_properties` and skip the `target_namespace="blueprint"` parameter. Authoring `blueprint::set_cdo_properties` and `bulk_fill_query("apply", target_namespace="blueprint", ...)` are semantically identical; the framework call is preferred when wiring multi-namespace tooling that already routes through the central dispatchers.

**Limitations / v1.1 follow-ups.**

- Soft refs (`PC_SoftObject` / `PC_SoftClass`) and `PC_Interface` defer to the existing primitives path — `(WISHLIST v1.1)` for first-class reflection-walker support.
- Cross-package `TObjectPtr` field writes inherit the v0.14.8 PR #43 `RF_Transient` fix — adapters do not need to defend against package-flag corruption.

---

#!/usr/bin/env python3
"""
Monolith MCP stdio-to-HTTP proxy.

Sits between Claude Code (stdio) and Monolith (HTTP on localhost).
Handles initialize locally, forwards tool calls to Monolith.
Survives editor restarts — proxy process never dies.
Background health poll auto-detects when the editor comes online.

Usage (in .mcp.json):
  {"mcpServers": {"monolith": {"command": "python", "args": ["Plugins/Monolith/Scripts/monolith_proxy.py"]}}}

Requirements: Python 3.8+ (stdlib only, no pip install needed)
"""

# PEP 563: defer annotation evaluation so PEP 604 unions (`str | None`) below
# parse on Python 3.8/3.9 too (macOS ships 3.9 by default via Xcode).
from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import threading
import time
import tempfile
import urllib.error
import urllib.request
from datetime import datetime, timezone
from io import TextIOWrapper
from pathlib import Path

MONOLITH_URL = os.environ.get("MONOLITH_URL", "http://localhost:9316/mcp")
MONOLITH_HEALTH = MONOLITH_URL.replace("/mcp", "/health")
PROXY_NAME = "monolith-proxy"
PROXY_VERSION = "1.1.1"
TIMEOUT = 30.0
POLL_INTERVAL = 5.0
POLL_START_DELAY = 3.0

# Track Monolith availability for list_changed notifications
_monolith_was_up = None
_stdout_lock = threading.Lock()

# Call-log state (Phase 4 / survivor F)
#
# NOTE: Saved/Logs/MonolithCalls.jsonl is project-root-relative and excluded
# from crash zip generation by UE's crash reporter (Saved/Logs/ tail capture
# only includes editor logs, not arbitrary jsonl). If a crash collector pattern
# elsewhere DOES sweep Saved/Logs/*, the user should add MonolithCalls.jsonl to
# the exclusion list. Single-user local dev tool; no phone-home.
_call_log_enabled = False           # resolved once at startup
_call_log_handle = None             # binary append-mode file handle
_call_log_lock = threading.Lock()

CORE_QUERY_TOOLS = [
    "blueprint_query",
    "material_query",
    "animation_query",
    "niagara_query",
    "editor_query",
    "config_query",
    "project_query",
    "source_query",
    "ui_query",
    "mesh_query",
    "gas_query",
    "combograph_query",
    "ai_query",
    "logicdriver_query",
    "audio_query",
    "level_sequence_query",
    "decision_query",
    "risk_query",
    "cppreflect_query",
    "network_query",
    "pipeline_query",
    "reflect_query",
]

OFFLINE_QUERY_TO_NAMESPACE = {
    "source_query": "source",
    "project_query": "project",
    "cppreflect_query": "cppreflect",
    "network_query": "network",
    "decision_query": "decision",
    "risk_query": "risk",
}

OFFLINE_ACTIONS = {
    "source": {
        "search_source": ["query"],
        "read_source": ["symbol"],
        "find_references": ["symbol"],
        "find_callers": ["symbol"],
        "find_callees": ["symbol"],
        "get_class_hierarchy": ["symbol"],
        "get_module_info": ["module_name"],
        "get_symbol_context": ["symbol"],
        "read_file": ["file_path"],
        "get_include_path": ["symbol"],
        "get_signature": ["symbol"],
        "check_deprecations": ["symbols"],
        "verify_symbols": ["symbols"],
        "find_example_usage": ["symbol"],
        "lint_header": ["file_path"],
        "generate_class_stub": ["parent", "class_name", "module"],
    },
    "project": {
        "search": ["query"],
        "find_by_type": ["asset_class"],
        "find_references": ["asset_path"],
        "get_stats": [],
        "get_asset_details": ["asset_path"],
    },
    "cppreflect": {
        "get_uclass": ["class_name"],
        "list_uproperties": ["class_name?"],
        "list_ufunctions": ["class_name?"],
        "find_interface_impls": ["interface_name"],
        "find_class_specifier": ["specifier_name"],
        "list_class_specifiers": [],
    },
    "network": {
        "list_replicated_classes": [],
        "list_rpc_functions": [],
        "list_onrep_handlers": [],
        "audit_unbalanced_onreps": [],
    },
    "decision": {
        "list_decisions": [],
        "get_decision": ["decision_id"],
        "list_stale": ["max_age_days?"],
        "find_supersession_chain": ["decision_id"],
        "find_referent_decisions": ["decision_id"],
    },
    "risk": {
        "get_hotspot_score": ["file_path"],
        "get_cochange_pairs": ["file_path"],
        "get_file_churn": ["file_path"],
        "get_release_window_hotspots": [],
        "list_conditional_gates": [],
    },
}

OFFLINE_NAMESPACE_ORDER = ["source", "project", "cppreflect", "network", "decision", "risk"]
OFFLINE_GUIDE_SECTIONS = ["onboarding", "recipes", "decisions", "errors", "skills_map", "gotchas"]
SHAPING_KEYS = {"_fields", "_omit", "_compact_json"}

CLI_FLAG_NAMES = {
    "max_lines": "--max-lines",
    "ref_kind": "--ref-kind",
    "context_lines": "--context-lines",
}


def _log(msg: str) -> None:
    """Log to stderr (visible in Claude Code debug mode, never interferes with stdio)."""
    print(f"[monolith-proxy] {msg}", file=sys.stderr, flush=True)


# ----------------------------------------------------------------------------
# JSONL call log (Phase 4 / survivor F)
#
# One line per upstream HTTP roundtrip:
#   {"ts":"2026-05-27T18:14:56Z","namespace":"editor","action":"get_build_errors",
#    "params_hash":"<40-char-sha1-hex>","duration_ms":42.5,"ok":true,
#    "error_code":null,"result_bytes":1834}
#
# Path: <project-root>/Saved/Logs/MonolithCalls.jsonl
# Opt-out: env var MONOLITH_CALL_LOG=0
# Atomicity: open(..., "ab") + threading.Lock around write+flush is sufficient
# for single-process emission. POSIX O_APPEND would give kernel-level atomicity
# for writes < PIPE_BUF (~4KB), but the lock makes the choice moot for our
# line sizes.
# ----------------------------------------------------------------------------


def _resolve_call_log_path() -> Path:
    """Resolve <project-root>/Saved/Logs/MonolithCalls.jsonl.

    Priority:
      1. MONOLITH_PROJECT_ROOT env var (explicit override).
      2. Current working directory (proxy CWD is the project root when launched
         by Claude Code's MCP config).
    """
    root = os.environ.get("MONOLITH_PROJECT_ROOT") or os.getcwd()
    logs_dir = Path(root) / "Saved" / "Logs"
    logs_dir.mkdir(parents=True, exist_ok=True)
    return logs_dir / "MonolithCalls.jsonl"


def _init_call_log() -> None:
    """Open the call-log file handle once at startup. Default-enabled."""
    global _call_log_enabled, _call_log_handle

    _call_log_enabled = os.environ.get("MONOLITH_CALL_LOG", "1") != "0"
    if not _call_log_enabled:
        _log("Call log disabled (MONOLITH_CALL_LOG=0)")
        return

    try:
        path = _resolve_call_log_path()
        # Append-binary mode; OS handles end-of-file positioning. We flush after
        # each write so a crash leaves complete lines on disk.
        _call_log_handle = open(path, "ab")
        _log(f"Call log: {path}")
    except OSError as e:
        _log(f"Failed to open call log: {e} -- logging disabled")
        _call_log_enabled = False
        _call_log_handle = None


def _canonical_json(value) -> str:
    """sort_keys + tightest separators -- matches the cpp proxy."""
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def _extract_namespace_action(msg: dict) -> tuple[str, str]:
    """Mirror the cpp proxy's extraction logic for (namespace, action)."""
    method = msg.get("method", "") or ""
    if method != "tools/call":
        return method, ""

    params = msg.get("params") or {}
    if not isinstance(params, dict):
        return "tools/call", ""

    name = params.get("name", "") or ""
    if name.endswith("_query"):
        ns = name[: -len("_query")]
        args = params.get("arguments") or {}
        action = args.get("action", "") if isinstance(args, dict) else ""
        return ns, action or ""

    if name.startswith("monolith_"):
        return "monolith", name[len("monolith_"):]

    return name, ""


def _extract_params_for_hash(msg: dict):
    """For tools/call, hash arguments; for other methods, hash the whole params."""
    method = msg.get("method", "") or ""
    params = msg.get("params") or {}
    if not isinstance(params, dict):
        return {}

    if method == "tools/call":
        args = params.get("arguments") or {}
        return args if isinstance(args, dict) else {}
    return params


def _inspect_response(resp: str | None) -> tuple[bool, int | None, int]:
    """Returns (ok, error_code, result_bytes)."""
    if not resp:
        return False, None, 0
    try:
        parsed = json.loads(resp)
    except (json.JSONDecodeError, ValueError):
        return False, None, len(resp.encode("utf-8")) if resp else 0

    error = parsed.get("error") if isinstance(parsed, dict) else None
    if isinstance(error, dict):
        code = error.get("code")
        error_code = int(code) if isinstance(code, int) else None
        result = parsed.get("result")
        if result is not None:
            result_bytes = len(_canonical_json(result).encode("utf-8"))
        else:
            result_bytes = len(resp.encode("utf-8"))
        return False, error_code, result_bytes

    result = parsed.get("result") if isinstance(parsed, dict) else None
    if result is not None:
        result_bytes = len(_canonical_json(result).encode("utf-8"))
    else:
        result_bytes = len(resp.encode("utf-8"))
    return True, None, result_bytes


def _write_call_log_line(msg: dict, resp: str | None, duration_ms: float) -> None:
    """Append one JSONL line describing an upstream HTTP roundtrip."""
    if not _call_log_enabled or _call_log_handle is None:
        return
    try:
        ns, action = _extract_namespace_action(msg)
        params_for_hash = _extract_params_for_hash(msg)
        canonical = _canonical_json(params_for_hash)
        params_hash = hashlib.sha1(canonical.encode("utf-8")).hexdigest()

        ok, error_code, result_bytes = _inspect_response(resp)

        # Second-precision ISO-8601 UTC (matches cpp proxy)
        ts = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

        line = {
            "ts": ts,
            "namespace": ns,
            "action": action,
            "params_hash": params_hash,
            "duration_ms": round(duration_ms, 3),
            "ok": ok,
            "error_code": error_code,
            "result_bytes": result_bytes,
        }
        payload = (json.dumps(line, separators=(",", ":")) + "\n").encode("utf-8")
        with _call_log_lock:
            _call_log_handle.write(payload)
            _call_log_handle.flush()
    except Exception as e:
        # Never let logging crash the proxy.
        _log(f"Call-log write failed: {e}")


def _post_monolith(body: str, timeout: float = TIMEOUT) -> str | None:
    """POST JSON-RPC to Monolith. Returns response body or None on failure."""
    try:
        req = urllib.request.Request(
            MONOLITH_URL,
            data=body.encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.read().decode("utf-8")
    except (urllib.error.URLError, OSError, TimeoutError) as e:
        _log(f"Monolith unreachable: {e}")
        return None


def _write(stdout, msg: str) -> None:
    """Write a JSON-RPC message to stdout (thread-safe)."""
    with _stdout_lock:
        stdout.write(msg + "\n")
        stdout.flush()


def _result(id, result: dict) -> str:
    return json.dumps({"jsonrpc": "2.0", "id": id, "result": result})


def _tool_error(id, message: str) -> str:
    """Return a tool result with isError=true (graceful failure, not protocol error)."""
    return json.dumps({
        "jsonrpc": "2.0",
        "id": id,
        "result": {
            "content": [{"type": "text", "text": message}],
            "isError": True,
        },
    })


def _tool_text(id, text: str, is_error: bool = False) -> str:
    return json.dumps({
        "jsonrpc": "2.0",
        "id": id,
        "result": {
            "content": [{"type": "text", "text": text}],
            "isError": is_error,
        },
    })


def _jsonrpc_error(id, code: int, message: str) -> str:
    """Return a JSON-RPC protocol-level error."""
    return json.dumps({
        "jsonrpc": "2.0",
        "id": id,
        "error": {"code": code, "message": message},
    })


def _sanitize_cache_part(value: str) -> str:
    return "".join(c if c.isalnum() or c in "-_" else "_" for c in value)


def _tools_cache_path() -> Path:
    base = Path(os.environ.get("LOCALAPPDATA") or tempfile.gettempdir())
    cache_dir = base / "Monolith"
    cache_dir.mkdir(parents=True, exist_ok=True)

    host_port = MONOLITH_HEALTH.replace("http://", "").replace("https://", "")
    host_port = host_port.split("/", 1)[0]
    return cache_dir / f"monolith_proxy_tools_{_sanitize_cache_part(host_port)}.json"


def _query_tool_schema() -> dict:
    return {
        "type": "object",
        "properties": {
            "action": {
                "type": "string",
                "description": "The action to execute. Use monolith_discover first when the editor is available.",
            },
            "params": {
                "type": "object",
                "description": "Parameters for the selected action.",
            },
            "_fields": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit.",
            },
            "_omit": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields.",
            },
            "_compact_json": {
                "type": "boolean",
                "description": "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object.",
            },
        },
        "required": ["action"],
    }


def _empty_object_schema() -> dict:
    return {
        "type": "object",
        "properties": {
            "_fields": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit.",
            },
            "_omit": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields.",
            },
            "_compact_json": {
                "type": "boolean",
                "description": "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object.",
            },
        },
    }


def _make_tool(name: str, description: str, schema: dict) -> dict:
    return {"name": name, "description": description, "inputSchema": schema}


def _plugin_root() -> Path:
    return Path(__file__).resolve().parent.parent


def _project_root() -> Path:
    return Path(os.environ.get("MONOLITH_PROJECT_ROOT") or os.getcwd()).resolve()


def _offline_script_path() -> Path:
    return Path(__file__).resolve().with_name("monolith_offline.py")


def _offline_source_db_path() -> Path:
    return _plugin_root() / "Saved" / "EngineSource.db"


def _offline_project_db_path() -> Path:
    return _plugin_root() / "Saved" / "ProjectIndex.db"


def _format_json_payload(payload: dict) -> str:
    return json.dumps(payload, indent=2, ensure_ascii=False)


def _offline_action_schema(namespace: str, action: str) -> dict:
    specs = OFFLINE_ACTIONS.get(namespace, {}).get(action, [])
    required = [s for s in specs if not s.endswith("?")]
    properties = {
        spec.rstrip("?"): {
            "description": "Forwarded to monolith_offline.py.",
        }
        for spec in specs
    }
    properties.update({
        "_fields": {
            "type": "array",
            "items": {"type": "string"},
            "description": "Optional top-level whitelist for JSON responses.",
        },
        "_omit": {
            "type": "array",
            "items": {"type": "string"},
            "description": "Optional top-level blacklist for JSON responses.",
        },
        "_compact_json": {
            "type": "boolean",
            "description": "Drop empty top-level JSON fields when true.",
        },
    })
    return {
        "type": "object",
        "properties": properties,
        "required": required,
        "offline": True,
    }


def _offline_discover_payload(namespace: str | None = None) -> dict:
    dbs = {
        "source": str(_offline_source_db_path()),
        "project": str(_offline_project_db_path()),
    }

    if namespace:
        if namespace == "monolith":
            return {
                "success": True,
                "offline": True,
                "editor_online": False,
                "namespace": "monolith",
                "actions": [
                    {"name": "discover", "description": "Offline namespace/action inventory."},
                    {"name": "status", "description": "Proxy and offline database status."},
                    {"name": "guide", "description": "Read Docs/MONOLITH_GUIDE.md from disk."},
                ],
            }
        actions = OFFLINE_ACTIONS.get(namespace)
        if actions is None:
            return {
                "success": False,
                "offline": True,
                "editor_online": False,
                "error": (
                    f"Namespace '{namespace}' is not available while the Unreal Editor is offline. "
                    f"Offline namespaces: {', '.join(OFFLINE_NAMESPACE_ORDER)}."
                ),
            }
        return {
            "success": True,
            "offline": True,
            "editor_online": False,
            "namespace": namespace,
            "action_count": len(actions),
            "actions": [
                {
                    "name": action,
                    "params": _offline_action_schema(namespace, action),
                    "readOnlyHint": True,
                    "idempotentHint": True,
                }
                for action in actions
            ],
            "database_paths": dbs,
        }

    return {
        "success": True,
        "offline": True,
        "editor_online": False,
        "proxy": {"name": PROXY_NAME, "version": PROXY_VERSION},
        "offline_namespaces": [
            {
                "namespace": ns,
                "action_count": len(OFFLINE_ACTIONS[ns]),
                "actions": list(OFFLINE_ACTIONS[ns].keys()),
            }
            for ns in OFFLINE_NAMESPACE_ORDER
        ],
        "offline_meta_tools": ["monolith_discover", "monolith_status", "monolith_guide"],
        "database_paths": dbs,
        "guide_hint": "Call monolith_guide(section='recipes') for workflows. Write/edit actions require the Unreal Editor.",
    }


def _offline_status_payload() -> dict:
    return {
        "success": True,
        "proxy": {"name": PROXY_NAME, "version": PROXY_VERSION},
        "editor_online": _check_monolith_up(),
        "monolith_url": MONOLITH_URL,
        "offline_enabled": os.environ.get("MONOLITH_OFFLINE", "1") != "0",
        "offline_script": str(_offline_script_path()),
        "offline_namespaces": OFFLINE_NAMESPACE_ORDER,
        "database_paths": {
            "source": {
                "path": str(_offline_source_db_path()),
                "exists": _offline_source_db_path().exists(),
            },
            "project": {
                "path": str(_offline_project_db_path()),
                "exists": _offline_project_db_path().exists(),
            },
        },
    }


def _read_guide_section(section: str | None) -> tuple[bool, str]:
    guide_path = _plugin_root() / "Docs" / "MONOLITH_GUIDE.md"
    try:
        text = guide_path.read_text(encoding="utf-8")
    except OSError as e:
        return False, f"Guide markdown not found or unreadable at {guide_path}: {e}"

    if not section:
        return True, text

    if section not in OFFLINE_GUIDE_SECTIONS:
        return False, (
            f"Unknown guide section '{section}'. "
            f"Valid sections: {', '.join(OFFLINE_GUIDE_SECTIONS)}."
        )

    header = f"## {section}"
    start = text.find(header)
    if start < 0:
        return False, f"Guide section '{section}' was not found in {guide_path}."

    next_start = text.find("\n## ", start + len(header))
    if next_start < 0:
        return True, text[start:].strip()
    return True, text[start:next_start].strip()


def _extract_tool_arguments(msg: dict) -> dict:
    params = msg.get("params") or {}
    if not isinstance(params, dict):
        return {}
    args = params.get("arguments") or {}
    return args if isinstance(args, dict) else {}


def _extract_query_params(args: dict) -> tuple[str, dict, dict]:
    action = str(args.get("action") or "").strip()
    nested = args.get("params")
    if isinstance(nested, dict):
        query_params = dict(nested)
    else:
        query_params = {
            k: v
            for k, v in args.items()
            if k not in {"action", "params"} and k not in SHAPING_KEYS
        }
    shaping = {k: args.get(k) for k in SHAPING_KEYS if k in args}
    for key in SHAPING_KEYS:
        if key in query_params and key not in shaping:
            shaping[key] = query_params.pop(key)
    return action, query_params, shaping


def _flag_name(key: str) -> str:
    return CLI_FLAG_NAMES.get(key, "--" + key)


def _append_cli_value(argv: list[str], value) -> None:
    if isinstance(value, (list, tuple)):
        argv.extend(str(v) for v in value)
    else:
        argv.append(str(value))


def _build_offline_argv(namespace: str, action: str, params: dict) -> tuple[list[str] | None, str | None]:
    actions = OFFLINE_ACTIONS.get(namespace, {})
    specs = actions.get(action)
    if specs is None:
        return None, (
            f"Action '{namespace}.{action}' is not available while the Unreal Editor is offline. "
            f"Offline actions for {namespace}: {', '.join(actions.keys())}."
        )

    argv = [sys.executable or "python3", str(_offline_script_path()), namespace, action]
    remaining = dict(params)

    for raw_spec in specs:
        optional = raw_spec.endswith("?")
        name = raw_spec.rstrip("?")
        value = remaining.pop(name, None)
        if value is None and name == "module_name":
            value = remaining.pop("module", None)
            if value is None:
                value = remaining.pop("symbol", None)
        if value is None and name == "class_name":
            value = remaining.pop("class_name_pos", None)
        if value is None or value == "":
            if optional:
                continue
            return None, f"Offline action '{namespace}.{action}' requires parameter '{name}'."
        _append_cli_value(argv, value)

    for key in list(remaining.keys()):
        if key in SHAPING_KEYS:
            remaining.pop(key, None)

    for key, value in remaining.items():
        if value is None or value == "":
            continue

        if isinstance(value, bool):
            if key == "header":
                if not value:
                    argv.append("--no-header")
            elif value:
                argv.append(_flag_name(key))
            continue

        if isinstance(value, (list, tuple)):
            for item in value:
                argv.extend([_flag_name(key), str(item)])
            continue

        argv.extend([_flag_name(key), str(value)])

    return argv, None


def _apply_response_shaping(text: str, shaping: dict) -> str:
    if not shaping:
        return text
    try:
        payload = json.loads(text)
    except (TypeError, ValueError, json.JSONDecodeError):
        return text
    if not isinstance(payload, dict):
        return text

    fields = shaping.get("_fields")
    omit = shaping.get("_omit")
    if isinstance(fields, list) and isinstance(omit, list):
        payload = {
            "success": False,
            "error": "_fields and _omit are mutually exclusive.",
        }
    elif isinstance(fields, list):
        payload = {k: payload[k] for k in fields if isinstance(k, str) and k in payload}
    elif isinstance(omit, list):
        for k in omit:
            if isinstance(k, str):
                payload.pop(k, None)

    if shaping.get("_compact_json") is True:
        payload = {
            k: v for k, v in payload.items()
            if v is not None and v != "" and v != [] and v != {}
        }

    return _format_json_payload(payload)


def _try_offline_query(msg: dict, tool_name: str) -> str | None:
    if os.environ.get("MONOLITH_OFFLINE", "1") == "0":
        return None

    namespace = OFFLINE_QUERY_TO_NAMESPACE.get(tool_name)
    if not namespace:
        return None

    args = _extract_tool_arguments(msg)
    action, query_params, shaping = _extract_query_params(args)
    if not action:
        return _tool_error(msg.get("id"), f"Tool '{tool_name}' requires an 'action' string argument.")

    argv, error = _build_offline_argv(namespace, action, query_params)
    if error:
        return _tool_error(msg.get("id"), error)

    if not _offline_script_path().exists():
        return _tool_error(msg.get("id"), f"Offline script not found: {_offline_script_path()}")

    timeout = float(os.environ.get("MONOLITH_OFFLINE_TIMEOUT", "30"))
    env = os.environ.copy()
    env.setdefault("PYTHONIOENCODING", "utf-8")

    try:
        proc = subprocess.run(
            argv,
            cwd=str(_project_root()),
            env=env,
            text=True,
            encoding="utf-8",
            errors="replace",
            capture_output=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return _tool_error(
            msg.get("id"),
            f"Offline action '{namespace}.{action}' timed out after {timeout:g}s.",
        )
    except OSError as e:
        return _tool_error(msg.get("id"), f"Failed to run offline action '{namespace}.{action}': {e}")

    stdout = (proc.stdout or "").strip()
    stderr = (proc.stderr or "").strip()
    ok = proc.returncode == 0
    text = stdout if stdout else stderr
    if not text:
        text = f"Offline action '{namespace}.{action}' exited with code {proc.returncode}."
    elif stderr and not ok:
        text = f"{text}\n\nstderr:\n{stderr}" if stdout else stderr

    if ok:
        text = _apply_response_shaping(text, shaping)

    return _tool_text(msg.get("id"), text, is_error=not ok)


def _try_offline_meta_tool(msg: dict, tool_name: str) -> str | None:
    if os.environ.get("MONOLITH_OFFLINE", "1") == "0":
        return None

    args = _extract_tool_arguments(msg)
    if tool_name == "monolith_discover":
        namespace = args.get("namespace")
        if namespace is not None:
            namespace = str(namespace).strip() or None
        payload = _offline_discover_payload(namespace)
        return _tool_text(msg.get("id"), _format_json_payload(payload), not payload.get("success", False))

    if tool_name == "monolith_status":
        return _tool_text(msg.get("id"), _format_json_payload(_offline_status_payload()))

    if tool_name == "monolith_guide":
        section = args.get("section")
        ok, text = _read_guide_section(str(section).strip() if section else None)
        return _tool_text(msg.get("id"), text, is_error=not ok)

    return None


def _seed_tools() -> list[dict]:
    tools = []
    for name in CORE_QUERY_TOOLS:
        domain = name[:-6] if name.endswith("_query") else name
        offline_note = (
            " Offline-backed while the editor is closed."
            if name in OFFLINE_QUERY_TO_NAMESPACE
            else " Requires a running Unreal Editor."
        )
        tools.append(_make_tool(
            name,
            f"Query the {domain} domain.{offline_note}",
            _query_tool_schema(),
        ))

    tools.append(_make_tool(
        "monolith_discover",
        "List available tool namespaces and their actions. Pass namespace and optional category to filter.",
        {
            "type": "object",
            "properties": {
                "namespace": {"type": "string", "description": "Optional: filter to a specific namespace"},
                "category": {"type": "string", "description": "Optional: filter actions within the namespace by category"},
                "_fields": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit.",
                },
                "_omit": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields.",
                },
                "_compact_json": {
                    "type": "boolean",
                    "description": "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object.",
                },
            },
        },
    ))
    tools.append(_make_tool(
        "monolith_status",
        "Get Monolith server health when the editor is online; reports proxy/offline status when it is not.",
        _empty_object_schema(),
    ))
    tools.append(_make_tool(
        "monolith_guide",
        "Read the Monolith editorial guide. This is available from disk while the editor is offline.",
        {
            "type": "object",
            "properties": {
                "section": {
                    "type": "string",
                    "description": "Optional section key: onboarding, recipes, decisions, errors, skills_map, gotchas",
                },
                "_fields": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Accepted for schema consistency; ignored for markdown guide text.",
                },
                "_omit": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Accepted for schema consistency; ignored for markdown guide text.",
                },
                "_compact_json": {
                    "type": "boolean",
                    "description": "Accepted for schema consistency; ignored for markdown guide text.",
                },
            },
        },
    ))
    tools.append(_make_tool(
        "monolith_update",
        "Check for or install Monolith updates from GitHub Releases.",
        {
            "type": "object",
            "properties": {
                "action": {
                    "type": "string",
                    "description": "'check' to compare versions, 'install' to download and stage update",
                    "default": "check",
                },
                "_fields": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit.",
                },
                "_omit": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields.",
                },
                "_compact_json": {
                    "type": "boolean",
                    "description": "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object.",
                },
            },
        },
    ))
    tools.append(_make_tool(
        "monolith_reindex",
        "Re-index the Monolith project database. Requires the editor-side Monolith server.",
        _empty_object_schema(),
    ))
    return tools


def _write_tools_cache(resp: str) -> None:
    try:
        payload = json.loads(resp)
        tools = payload.get("result", {}).get("tools", [])
        if isinstance(tools, list) and tools:
            _tools_cache_path().write_text(json.dumps(tools), encoding="utf-8")
    except Exception as e:
        _log(f"Failed to write tools/list cache: {e}")


def _read_tools_cache() -> list[dict] | None:
    try:
        path = _tools_cache_path()
        if not path.exists():
            return None
        tools = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(tools, list) and tools:
            return tools
    except Exception as e:
        _log(f"Failed to read tools/list cache: {e}")
    return None


def _merge_seed_tools(tools: list[dict]) -> list[dict]:
    merged = list(tools)
    seen = {
        t.get("name")
        for t in merged
        if isinstance(t, dict) and isinstance(t.get("name"), str)
    }
    for seed in _seed_tools():
        name = seed.get("name")
        if name not in seen:
            merged.append(seed)
            seen.add(name)
    return merged


def _fallback_tools_list(msg: dict) -> str:
    cached = _read_tools_cache()
    if cached:
        _log("Monolith down during tools/list — returning cached tools plus offline seed tools")
        return _result(msg.get("id"), {"tools": _merge_seed_tools(cached)})

    _log("Monolith down during tools/list — returning seed tools")
    return _result(msg.get("id"), {"tools": _seed_tools()})


def _check_monolith_up() -> bool:
    """Lightweight health check via GET /health endpoint."""
    try:
        req = urllib.request.Request(MONOLITH_HEALTH, method="GET")
        with urllib.request.urlopen(req, timeout=3) as resp:
            return resp.status == 200
    except Exception:
        return False


def _send_list_changed(stdout) -> bool:
    """Send tools/list_changed notification. Returns False if stdout is broken."""
    try:
        _write(stdout, json.dumps({
            "jsonrpc": "2.0",
            "method": "notifications/tools/list_changed",
        }))
        return True
    except (BrokenPipeError, OSError):
        return False


def check_monolith_state_change(stdout) -> None:
    """Check for state transition and notify if changed."""
    global _monolith_was_up
    is_up = _check_monolith_up()

    if _monolith_was_up is not None and is_up != _monolith_was_up:
        direction = "online" if is_up else "offline"
        _log(f"Monolith went {direction} — sending tools/list_changed")
        _send_list_changed(stdout)

    _monolith_was_up = is_up


def _health_poll_thread(stdout) -> None:
    """Background thread that polls Monolith and sends list_changed on state transitions."""
    time.sleep(POLL_START_DELAY)
    _log(f"Health poll started (interval={POLL_INTERVAL}s)")

    while True:
        try:
            check_monolith_state_change(stdout)
        except (BrokenPipeError, OSError):
            _log("stdout broken, health poll exiting")
            return
        except Exception as e:
            _log(f"Health poll error: {e}")

        time.sleep(POLL_INTERVAL)


def handle_initialize(msg: dict) -> str:
    """Handle initialize locally. Proxy is always available."""
    client_version = msg.get("params", {}).get("protocolVersion", "2025-11-25")
    supported = {"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"}
    version = client_version if client_version in supported else "2025-11-25"

    return _result(msg.get("id"), {
        "protocolVersion": version,
        "capabilities": {
            "tools": {"listChanged": True},
        },
        "serverInfo": {"name": PROXY_NAME, "version": PROXY_VERSION},
        "instructions": (
            "Monolith MCP proxy for Unreal Engine. Tools are forwarded to the Unreal Editor. "
            "When the editor is closed, read-only offline tools remain available for source, "
            "project, cppreflect, network, decision, and risk queries against the on-disk SQLite indexes. "
            "Before calling a domain action, check its schema instead of guessing: "
            "monolith_discover() lists namespaces, monolith_discover('<namespace>') lists a "
            "namespace's actions, and describe_query('action_schema', ...) returns an action's "
            "exact parameter schema. monolith_guide(section='recipes') gives cross-namespace "
            "workflows, decision matrices, and gotchas. "
            "If a live-only tool returns an editor-not-running error, start the editor and retry."
        ),
    })


def handle_ping(msg: dict) -> str:
    return _result(msg.get("id"), {})


def handle_tools_list(msg: dict) -> str:
    """Forward tools/list to Monolith. Stable cached/seed list if down."""
    t0 = time.perf_counter()
    resp = _post_monolith(json.dumps(msg))
    duration_ms = (time.perf_counter() - t0) * 1000.0
    _write_call_log_line(msg, resp, duration_ms)

    if resp:
        _write_tools_cache(resp)
        return resp
    return _fallback_tools_list(msg)


def handle_tools_call(msg: dict) -> str:
    """Forward tools/call to Monolith. Graceful error if down."""
    tool_name = msg.get("params", {}).get("name", "unknown")
    t0 = time.perf_counter()
    resp = _post_monolith(json.dumps(msg))
    duration_ms = (time.perf_counter() - t0) * 1000.0
    _write_call_log_line(msg, resp, duration_ms)

    if resp:
        return resp

    offline_resp = _try_offline_meta_tool(msg, tool_name)
    if offline_resp:
        return offline_resp

    offline_resp = _try_offline_query(msg, tool_name)
    if offline_resp:
        return offline_resp

    return _tool_error(
        msg.get("id"),
        f"Monolith MCP is not available (Unreal Editor not running). "
        f"Tool '{tool_name}' is live-only. Start the editor and try again, or use one of the "
        f"offline namespaces: {', '.join(OFFLINE_NAMESPACE_ORDER)}.",
    )


def main() -> None:
    # Use binary-safe IO for Windows compatibility
    stdin = TextIOWrapper(sys.stdin.buffer, encoding="utf-8", newline="\n")
    stdout = TextIOWrapper(sys.stdout.buffer, encoding="utf-8", newline="\n")

    _log(f"Started. Forwarding to {MONOLITH_URL}")

    _init_call_log()

    # Start background health poller
    poller = threading.Thread(
        target=_health_poll_thread,
        args=(stdout,),
        daemon=True,
        name="monolith-health-poll",
    )
    poller.start()

    for line in stdin:
        line = line.strip()
        if not line:
            continue

        try:
            msg = json.loads(line)
        except json.JSONDecodeError as e:
            _log(f"Bad JSON: {e}")
            continue

        method = msg.get("method", "")
        msg_id = msg.get("id")  # None for notifications
        response = None

        if method == "initialize":
            response = handle_initialize(msg)
            _log("Initialized")

        elif method in ("notifications/initialized", "initialized"):
            # Notification — no response. Check if Monolith is up.
            check_monolith_state_change(stdout)

        elif method == "ping":
            response = handle_ping(msg)

        elif method == "tools/list":
            check_monolith_state_change(stdout)
            response = handle_tools_list(msg)

        elif method == "tools/call":
            response = handle_tools_call(msg)

        else:
            # Forward unknown methods to Monolith
            t0 = time.perf_counter()
            resp = _post_monolith(json.dumps(msg))
            duration_ms = (time.perf_counter() - t0) * 1000.0
            _write_call_log_line(msg, resp, duration_ms)

            if resp:
                response = resp
            elif msg_id is not None:
                response = _jsonrpc_error(msg_id, -32601, f"Method not found: {method}")

        if response:
            _write(stdout, response)


if __name__ == "__main__":
    main()

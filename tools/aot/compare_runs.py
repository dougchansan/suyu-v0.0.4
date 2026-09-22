#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate and compare instrumented, fixed-work AOT traces. No game files needed."""
from __future__ import annotations

import argparse
import json
import math
import re
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

IDENTITY_FIELDS = ("title_id", "host_arch", "workload_id", "content_id", "config_id", "machine_id")
COUNTERS = ("static_blocks", "svc_calls", "lookup_misses", "unhandled", "no_fallback", "jit_transitions")
FLAGS = ("complete", "counters_valid", "observed_aot", "observed_non_aot_frame",
         "all_aot_samples_strict", "all_aot_samples_guarded", "jit_compiled_out",
         "diagnostic_cutoff", "strict_workload_observed")
BACKENDS = {"non_aot", "aot_hybrid_allowed", "aot_runtime_strict", "aot_no_jit_build"}


class TraceError(ValueError):
    """Missing, incompatible, incomplete, or internally inconsistent evidence."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise TraceError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _invalid_constant(value: str) -> None:
    raise TraceError(f"non-finite JSON number: {value}")


def _integer(value: Any, name: str) -> int:
    if type(value) is not int or not 0 <= value <= (1 << 64) - 1:
        raise TraceError(f"{name} must be an unsigned 64-bit integer")
    return value


def _number(value: Any, name: str) -> float:
    if type(value) not in (int, float):
        raise TraceError(f"{name} must be numeric, not a boolean/string")
    try:
        result = float(value)
    except OverflowError as error:
        raise TraceError(f"{name} is too large") from error
    if not math.isfinite(result) or result < 0:
        raise TraceError(f"{name} must be finite and nonnegative")
    return result


def strict_gate(summary: dict[str, Any]) -> bool:
    """Recompute the gate; never trust a reported PASS flag alone."""
    return bool(summary["observed_aot"] and not summary["observed_non_aot_frame"]
                and summary["counters_valid"] and summary["all_aot_samples_strict"]
                and summary["all_aot_samples_guarded"] and not summary["diagnostic_cutoff"]
                and summary["static_blocks"] > 0 and summary["jit_transitions"] == 0
                and summary["lookup_misses"] == 0 and summary["unhandled"] == 0
                and summary["no_fallback"] == 0)


@dataclass(frozen=True)
class Run:
    metadata: dict[str, Any]
    summary: dict[str, Any]
    active_ms: tuple[float, ...]
    period_ms: tuple[float, ...]


def load_run(path: Path) -> Run:
    """Read one complete session. Concatenated runs must use separate files."""
    metadata: dict[str, Any] | None = None
    summary: dict[str, Any] | None = None
    active: list[float] = []
    periods: list[float] = []
    try:
        with path.open(encoding="utf-8") as stream:
            for number, line in enumerate(stream, 1):
                if not line.strip():
                    continue
                if len(line) > 1_048_576:
                    raise TraceError(f"line {number} exceeds the trace record limit")
                record = json.loads(line, object_pairs_hook=_unique_object,
                                    parse_constant=_invalid_constant)
                if not isinstance(record, dict):
                    raise TraceError(f"line {number} is not a JSON object")
                kind = record.get("type")
                if summary is not None:
                    raise TraceError("records after summary (possibly concatenated sessions)")
                if kind == "session":
                    if metadata is not None or active:
                        raise TraceError("multiple sessions; use a fresh trace file per run")
                    if type(record.get("schema")) is not int or record["schema"] != 1:
                        raise TraceError("unsupported trace schema")
                    for field in (*IDENTITY_FIELDS, "build_id"):
                        value = record.get(field)
                        if not isinstance(value, str) or not value.strip() or len(value) > 1024:
                            raise TraceError(f"missing/invalid identity field: {field}")
                    if not re.fullmatch(r"[0-9a-fA-F]{16}", record["title_id"]) or int(record["title_id"], 16) == 0:
                        raise TraceError("title_id must be a nonzero 16-digit hexadecimal string")
                    if record.get("timing_domain") != "system_frame_active_ms":
                        raise TraceError("unsupported timing domain")
                    metadata = record
                elif kind == "frame":
                    if metadata is None:
                        raise TraceError("frame before session header")
                    if _integer(record.get("index"), "index") != len(active) + 1:
                        raise TraceError("missing, duplicated or reordered frame")
                    active.append(_number(record.get("active_ms"), "active_ms"))
                    periods.append(_number(record.get("period_ms"), "period_ms"))
                elif kind == "summary":
                    if metadata is None:
                        raise TraceError("summary before session header")
                    for field in FLAGS:
                        if type(record.get(field)) is not bool:
                            raise TraceError(f"{field} must be a boolean")
                    for field in (*COUNTERS, "frames", "invalid_frames"):
                        _integer(record.get(field), field)
                    if not record["complete"] or record["frames"] != len(active):
                        raise TraceError("incomplete trace or incorrect frame count")
                    if record["invalid_frames"] or not record["counters_valid"]:
                        raise TraceError("invalid frame samples or discontinuous counters")
                    if record["diagnostic_cutoff"] or record["unhandled"] or record["no_fallback"]:
                        raise TraceError("diagnostic/blocked execution is not benchmark evidence")
                    if not isinstance(record.get("backend_at_shutdown"), str) or record["backend_at_shutdown"] not in BACKENDS:
                        raise TraceError("unknown backend")
                    if record["jit_compiled_out"] and record["jit_transitions"]:
                        raise TraceError("JIT transitions reported in a no-JIT build")
                    if record["strict_workload_observed"] != strict_gate(record):
                        raise TraceError("inconsistent strict-workload gate")
                    summary = record
                else:
                    raise TraceError(f"unknown record type: {kind!r}")
    except (OSError, UnicodeError, ValueError, KeyError) as error:
        raise TraceError(f"{path}: {error}") from error
    if metadata is None or summary is None or not active:
        raise TraceError(f"{path}: empty/incomplete trace (no orderly shutdown footer)")
    return Run(metadata, summary, tuple(active), tuple(periods))


def percentile(values: tuple[float, ...], fraction: float) -> float:
    if not values or not 0 <= fraction <= 1:
        raise TraceError("percentile requires samples and a fraction in [0, 1]")
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def describe(values: tuple[float, ...]) -> dict[str, float]:
    result = {"median": statistics.median(values), "p95": percentile(values, .95),
              "p99": percentile(values, .99)}
    if not all(math.isfinite(value) for value in result.values()):
        raise TraceError("summary statistic overflow; invalid measurement scale")
    return result


def compare(baseline: Run, candidate: Run, *, warmup_frames: int = 120, min_frames: int = 300,
            max_regression_percent: float = 5.0, allow_runtime_strict: bool = False) -> dict[str, Any]:
    if warmup_frames < 0 or min_frames < 1 or not math.isfinite(max_regression_percent) or max_regression_percent < 0:
        raise TraceError("invalid comparison limits")
    for key in IDENTITY_FIELDS:
        if baseline.metadata[key] != candidate.metadata[key]:
            raise TraceError(f"mismatched {key}; cross-device/title/settings comparisons are invalid")
    if len(baseline.active_ms) != len(candidate.active_ms):
        raise TraceError("frame counts differ; capture the same fixed workload, not equal walltime")
    if len(candidate.active_ms) - warmup_frames < min_frames:
        raise TraceError("insufficient measured frames after warmup")
    if not strict_gate(candidate.summary):
        raise TraceError("candidate did not observe a strict, guarded AOT-only workload")
    if not allow_runtime_strict and not candidate.summary["jit_compiled_out"]:
        raise TraceError("candidate still contains JIT; use SUYU_NO_JIT or explicitly allow runtime-strict evidence")
    reference = describe(baseline.active_ms[warmup_frames:])
    measured = describe(candidate.active_ms[warmup_frames:])
    regressions: dict[str, float] = {}
    for key, value in reference.items():
        if value <= 0:
            raise TraceError("baseline active frame time is zero; no meaningful ratio")
        regressions[key] = (measured[key] / value - 1.0) * 100.0
        if not math.isfinite(regressions[key]):
            raise TraceError("frame-time ratio overflow; invalid measurement scale")
    return {
        "schema": 1,
        "status": "pass" if all(v <= max_regression_percent + 1e-10 for v in regressions.values()) else "regression",
        "measured_frames": len(candidate.active_ms) - warmup_frames,
        "warmup_frames": warmup_frames,
        "max_regression_percent": max_regression_percent,
        "baseline_build": baseline.metadata["build_id"],
        "candidate_build": candidate.metadata["build_id"],
        "baseline_backend_at_shutdown": baseline.summary["backend_at_shutdown"],
        "candidate_jit_compiled_out": candidate.summary["jit_compiled_out"],
        "baseline_system_active_ms": reference,
        "candidate_system_active_ms": measured,
        "regression_percent": regressions,
        "baseline_system_period_ms": describe(baseline.period_ms[warmup_frames:]),
        "candidate_system_period_ms": describe(candidate.period_ms[warmup_frames:]),
        "instruction_coverage_percent": None,
        "cpu_time_coverage_percent": None,
        "gpu_time_ms": None,
        "limitations": "Instrumented system-frame timings, not CPU-only/GPU timings or game FPS. Non-AOT does not identify JIT. Metadata identities are caller-supplied, not hardware/content attestation. A passing threshold is not universal performance parity.",
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--warmup-frames", type=int, default=120)
    parser.add_argument("--min-frames", type=int, default=300)
    parser.add_argument("--max-regression-percent", type=float, default=5.0)
    parser.add_argument("--allow-runtime-strict", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        result = compare(load_run(args.baseline), load_run(args.candidate),
                         warmup_frames=args.warmup_frames, min_frames=args.min_frames,
                         max_regression_percent=args.max_regression_percent,
                         allow_runtime_strict=args.allow_runtime_strict)
        text = json.dumps(result, indent=2, allow_nan=False) + "\n"
        if args.output:
            args.output.write_text(text, encoding="utf-8")
        else:
            sys.stdout.write(text)
        return 0 if result["status"] == "pass" else 1
    except (TraceError, OSError) as error:
        print(f"Invalid AOT benchmark evidence: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

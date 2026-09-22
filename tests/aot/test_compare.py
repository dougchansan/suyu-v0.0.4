#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic trace/CLI contracts plus round trips from compiled C++ components."""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "aot"))
import compare_runs as aot

BUILD = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else None


def fixture(count: int = 500, active: float = 10.0) -> list[dict]:
    header = dict(type="session", schema=1, title_id="0000000000000001", host_arch="x86_64",
                  build_id="synthetic-build", workload_id="synthetic-fixture", content_id="test-content",
                  config_id="test-settings", machine_id="test-host", timing_domain="system_frame_active_ms")
    frames = [dict(type="frame", index=i + 1, active_ms=active, period_ms=16.0) for i in range(count)]
    summary = dict(type="summary", complete=True, frames=count, invalid_frames=0,
                   backend_at_shutdown="aot_no_jit_build", static_blocks=count, svc_calls=0,
                   lookup_misses=0, unhandled=0, no_fallback=0, jit_transitions=0,
                   counters_valid=True, observed_aot=True, observed_non_aot_frame=False,
                   all_aot_samples_strict=True, all_aot_samples_guarded=True, jit_compiled_out=True,
                   diagnostic_cutoff=False, strict_workload_observed=True)
    return [header, *frames, summary]


class CompareTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "run.jsonl"

    def load(self, records: list[dict]) -> aot.Run:
        self.path.write_text("\n".join(json.dumps(record) for record in records) + "\n", encoding="utf-8")
        return aot.load_run(self.path)

    def reject(self, records: list[dict]) -> None:
        with self.assertRaises(aot.TraceError):
            self.load(records)

    def test_pass_does_not_claim_unmeasured_coverage(self):
        run = self.load(fixture())
        result = aot.compare(run, run)
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["measured_frames"], 380)
        for field in ("instruction_coverage_percent", "cpu_time_coverage_percent", "gpu_time_ms"):
            self.assertIsNone(result[field])

    def test_regression(self):
        baseline = self.load(fixture())
        candidate = self.load(fixture(active=11))
        self.assertEqual(aot.compare(baseline, candidate)["status"], "regression")

    def test_tail_latency_fails_even_when_median_passes(self):
        baseline = self.load(fixture())
        data = fixture()
        for record in data[-31:-1]:
            record["active_ms"] = 30
        result = aot.compare(baseline, self.load(data))
        self.assertEqual(result["regression_percent"]["median"], 0)
        self.assertGreater(result["regression_percent"]["p95"], 5)
        self.assertEqual(result["status"], "regression")

    def test_threshold_and_faster_run(self):
        baseline = self.load(fixture())
        for active in (9, 10.5):
            with self.subTest(active=active):
                self.assertEqual(aot.compare(baseline, self.load(fixture(active=active)))["status"], "pass")

    def test_identity_mismatch(self):
        baseline = self.load(fixture())
        for field in aot.IDENTITY_FIELDS:
            with self.subTest(field=field):
                data = fixture()
                data[0][field] = "0000000000000002" if field == "title_id" else "different"
                with self.assertRaises(aot.TraceError):
                    aot.compare(baseline, self.load(data))

    def test_build_is_allowed_to_differ(self):
        baseline = self.load(fixture())
        data = fixture()
        data[0]["build_id"] = "new-build"
        self.assertEqual(aot.compare(baseline, self.load(data))["status"], "pass")

    def test_missing_identity(self):
        for field in (*aot.IDENTITY_FIELDS, "build_id"):
            for value in (None, "", "  ", 123, "x" * 1025):
                with self.subTest(field=field, value=str(value)[:10]):
                    data = fixture(1)
                    data[0][field] = value
                    self.reject(data)

    def test_title_schema_domain(self):
        for field, values in (("title_id", ["0000000000000000", "1", "z" * 16]),
                              ("schema", [True, 0, 2, "1"]), ("timing_domain", ["gpu", None])):
            for value in values:
                data = fixture(1)
                data[0][field] = value
                self.reject(data)

    def test_fixed_work_frame_count(self):
        with self.assertRaises(aot.TraceError):
            aot.compare(self.load(fixture(500)), self.load(fixture(501)))

    def test_warmup_minimum_and_bad_limits(self):
        run = self.load(fixture())
        for limits in ({"warmup_frames": 300}, {"warmup_frames": -1}, {"min_frames": 0},
                       {"max_regression_percent": -1}, {"max_regression_percent": float("nan")}):
            with self.subTest(limits=limits), self.assertRaises(aot.TraceError):
                aot.compare(run, run, **limits)

    def test_empty_truncated_and_concatenated_runs(self):
        data = fixture(2)
        for broken in ([], data[:-1], data[1:], [data[0], data[-1]], data + data,
                       data + [data[1]], [data[0], data[0], *data[1:]]):
            self.reject(broken)

    def test_frame_order(self):
        for index in (0, 3, "1", True, -1):
            data = fixture(2)
            data[1]["index"] = index
            self.reject(data)

    def test_record_shapes(self):
        for text in ('[]\n', 'null\n', '{"type":"other"}\n', '{bad json}\n',
                     '{"type":"session","type":"session"}\n'):
            self.path.write_text(text)
            with self.assertRaises(aot.TraceError):
                aot.load_run(self.path)

    def test_bad_numeric_frames(self):
        for field in ("active_ms", "period_ms"):
            for value in (None, True, "10", -1, float("nan"), float("inf"), 10 ** 400):
                data = fixture(1)
                data[1][field] = value
                self.reject(data)

    def test_overlong_or_bad_encoding(self):
        for content in (b"\xff\n", b" " * 1_048_577 + b"{}\n", b'{"x":' + b"9" * 5000 + b"}\n"):
            self.path.write_bytes(content)
            with self.assertRaises(aot.TraceError):
                aot.load_run(self.path)

    def test_counter_types(self):
        for field in (*aot.COUNTERS, "frames", "invalid_frames"):
            for value in (True, -1, 1.5, "0", 1 << 64):
                data = fixture(1)
                data[-1][field] = value
                self.reject(data)

    def test_flag_types(self):
        for field in aot.FLAGS:
            data = fixture(1)
            data[-1][field] = 1
            self.reject(data)

    def test_incomplete_and_invalid_summary(self):
        for field, value in (("complete", False), ("frames", 5), ("invalid_frames", 1),
                             ("counters_valid", False), ("diagnostic_cutoff", True),
                             ("unhandled", 1), ("no_fallback", 1)):
            data = fixture(2)
            data[-1][field] = value
            self.reject(data)

    def test_backend_value(self):
        for value in ("imaginary", [], {}, None, 1):
            data = fixture(1)
            data[-1]["backend_at_shutdown"] = value
            self.reject(data)

    def test_gate_is_recomputed(self):
        for field, value in (("observed_aot", False), ("static_blocks", 0), ("lookup_misses", 1),
                             ("observed_non_aot_frame", True), ("all_aot_samples_strict", False),
                             ("all_aot_samples_guarded", False)):
            data = fixture(2)
            data[-1][field] = value
            self.reject(data)  # Forged gate is still true.
            data[-1]["strict_workload_observed"] = False
            run = self.load(data)
            with self.assertRaises(aot.TraceError):
                aot.compare(run, run, warmup_frames=0, min_frames=1)

    def test_jit_in_compiled_out_build(self):
        data = fixture(2)
        data[-1]["jit_transitions"] = 1
        data[-1]["strict_workload_observed"] = False
        self.reject(data)

    def test_hybrid_is_not_strict_even_without_jit_transition(self):
        data = fixture()
        data[-1].update(backend_at_shutdown="aot_hybrid_allowed", jit_compiled_out=False,
                        all_aot_samples_strict=False, strict_workload_observed=False)
        run = self.load(data)
        with self.assertRaises(aot.TraceError):
            aot.compare(run, run, allow_runtime_strict=True)

    def test_runtime_strict_requires_explicit_override(self):
        data = fixture()
        data[-1].update(backend_at_shutdown="aot_runtime_strict", jit_compiled_out=False)
        run = self.load(data)
        with self.assertRaises(aot.TraceError):
            aot.compare(run, run)
        self.assertEqual(aot.compare(run, run, allow_runtime_strict=True)["status"], "pass")

    def test_non_aot_baseline_not_renamed_jit(self):
        candidate = self.load(fixture())
        data = fixture()
        data[-1].update(backend_at_shutdown="non_aot", observed_aot=False, observed_non_aot_frame=True,
                        static_blocks=0, jit_compiled_out=False, strict_workload_observed=False)
        result = aot.compare(self.load(data), candidate)
        self.assertEqual(result["baseline_backend_at_shutdown"], "non_aot")

    def test_zero_baseline_rejected(self):
        with self.assertRaises(aot.TraceError):
            aot.compare(self.load(fixture(active=0)), self.load(fixture()))

    def test_overflowing_ratio_rejected(self):
        with self.assertRaises(aot.TraceError):
            aot.compare(self.load(fixture(active=1e-300)), self.load(fixture(active=1e300)))

    def test_statistic_overflow_rejected(self):
        with self.assertRaises(aot.TraceError):
            aot.compare(self.load(fixture(active=1e308)), self.load(fixture()))

    def test_percentiles(self):
        self.assertEqual(aot.percentile((1, 3), .5), 2)
        self.assertEqual(aot.percentile((4,), .99), 4)
        with self.assertRaises(aot.TraceError):
            aot.percentile((), .5)

    def test_missing_file(self):
        with self.assertRaises(aot.TraceError):
            aot.load_run(self.path)

    def test_cli_exit_codes(self):
        baseline = Path(self.temp.name) / "baseline.jsonl"
        baseline.write_text("\n".join(map(json.dumps, fixture())))
        command = [sys.executable, str(ROOT / "tools/aot/compare_runs.py"), str(baseline), str(self.path)]
        for active, expected in ((10, 0), (11, 1)):
            self.load(fixture(active=active))
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, expected, result.stderr)
            self.assertIn("status", json.loads(result.stdout))
        self.path.write_text("{}\n")
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 2)
        self.assertIn("Invalid AOT benchmark evidence", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    @unittest.skipIf(BUILD is None, "supply C++ build directory for writer round trips")
    def test_cpp_metrics_writer_roundtrip(self):
        run = aot.load_run(BUILD / "metrics-trace.jsonl")
        self.assertEqual(run.active_ms, (10, 11))
        self.assertEqual(run.summary["static_blocks"], 2)
        self.assertTrue(aot.strict_gate(run.summary))

    @unittest.skipIf(BUILD is None, "supply C++ build directory for component round trip")
    def test_production_perf_stats_writer_roundtrip(self):
        run = aot.load_run(BUILD / "perf-trace.jsonl")
        self.assertEqual(len(run.active_ms), 10)
        self.assertEqual(run.summary["static_blocks"], 10)
        self.assertEqual(run.summary["backend_at_shutdown"], "non_aot")
        self.assertTrue(aot.strict_gate(run.summary))


if __name__ == "__main__":
    unittest.main(verbosity=2)

#!/usr/bin/env python3
"""Unit tests for compare.py: the verdict rule, thresholds, both input formats, metadata
warnings and the exit code. Run directly, or through ctest (cmake/BenchTools.cmake)."""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True  # no __pycache__ next to the sources
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare  # noqa: E402  (needs the path above)
from compare import HIGHER, IMPROVEMENT, LOWER, NOISE, REGRESSION, judge  # noqa: E402


def minihft_doc(metrics, label="", benchmark="orderbook_latency", **meta):
    """A results file as the harnesses write it; `metrics` maps name -> value (lower is better)
    or name -> (value, better)."""
    metadata = {"benchmark": benchmark, "label": label, "args": ["--depths=10"], "commit": "abc1234",
                "cpu": "Test CPU", "compiler": "GCC 13", "build_type": "release", "pinned": True}
    metadata.update(meta)
    out = []
    for name, value in metrics.items():
        value, better = value if isinstance(value, tuple) else (value, LOWER)
        out.append({"name": name, "unit": "ns", "value": value, "better": better})
    return {"schema": "minihft-bench/1", "metadata": metadata, "metrics": out}


def gbench_doc(real_times, name="BM_RingBuffer_PushPopSameThread", aggregates_only=False):
    benchmarks = []
    if aggregates_only:
        for agg, value in (("mean", sum(real_times) / len(real_times)), ("median", sorted(real_times)[len(real_times) // 2])):
            benchmarks.append({"name": f"{name}_{agg}", "run_name": name, "run_type": "aggregate",
                               "aggregate_name": agg, "real_time": value, "cpu_time": value, "time_unit": "ns"})
    else:
        for i, t in enumerate(real_times):
            benchmarks.append({"name": name, "run_name": name, "run_type": "iteration", "repetition_index": i,
                               "real_time": t, "cpu_time": t, "time_unit": "ns", "iterations": 1000})
    return {"context": {"executable": "C:\\build\\Release\\ring_buffer_bench.exe", "library_build_type": "release",
                        "host_name": "test"}, "benchmarks": benchmarks}


class JudgeTest(unittest.TestCase):
    def test_single_run_over_threshold_is_an_unconfirmed_regression(self):
        v = judge([100.0], [110.0], LOWER, 5)
        self.assertEqual(v.verdict, REGRESSION)
        self.assertFalse(v.confirmed)
        self.assertAlmostEqual(v.delta, 10.0)

    def test_single_run_under_threshold_is_noise(self):
        v = judge([100.0], [104.0], LOWER, 5)
        self.assertEqual(v.verdict, NOISE)
        self.assertEqual(v.reason, "below threshold")

    def test_change_equal_to_threshold_is_noise(self):
        self.assertEqual(judge([100.0], [105.0], LOWER, 5).verdict, NOISE)

    def test_separated_ranges_over_threshold_is_a_confirmed_regression(self):
        v = judge([100.0, 101.0, 99.0], [120.0, 118.0, 125.0], LOWER, 5)
        self.assertEqual(v.verdict, REGRESSION)
        self.assertTrue(v.confirmed)
        self.assertAlmostEqual(v.delta, 20.0)

    def test_overlapping_ranges_are_noise_even_over_threshold(self):
        # Medians 100 -> 120, but the current's best run (95) beats the baseline's worst (130).
        v = judge([100.0, 90.0, 130.0], [120.0, 95.0, 140.0], LOWER, 5)
        self.assertEqual(v.verdict, NOISE)
        self.assertEqual(v.reason, "ranges overlap")

    def test_touching_ranges_count_as_overlap(self):
        self.assertEqual(judge([100.0, 110.0], [110.0, 130.0], LOWER, 5).verdict, NOISE)

    def test_separated_ranges_under_threshold_are_noise(self):
        self.assertEqual(judge([100.0, 100.5], [102.0, 102.5], LOWER, 5).verdict, NOISE)

    def test_lower_is_better_drop_is_an_improvement(self):
        v = judge([100.0, 102.0], [80.0, 81.0], LOWER, 5)
        self.assertEqual(v.verdict, IMPROVEMENT)
        self.assertLess(v.worse, 0)

    def test_higher_is_better_drop_is_a_regression(self):
        v = judge([160.0, 158.0, 161.0], [120.0, 125.0, 119.0], HIGHER, 5)
        self.assertEqual(v.verdict, REGRESSION)
        self.assertGreater(v.worse, 0)
        self.assertLess(v.delta, 0)

    def test_higher_is_better_rise_is_an_improvement(self):
        self.assertEqual(judge([100.0, 101.0], [120.0, 121.0], HIGHER, 5).verdict, IMPROVEMENT)

    def test_higher_is_better_overlap_is_noise(self):
        self.assertEqual(judge([100.0, 130.0], [80.0, 105.0], HIGHER, 5).verdict, NOISE)

    def test_median_of_an_even_number_of_runs(self):
        v = judge([10.0, 20.0], [30.0, 40.0], LOWER, 5)
        self.assertAlmostEqual(v.delta, 133.333333, places=4)  # 15 -> 35

    def test_one_run_against_several_checks_the_range_but_stays_unconfirmed(self):
        inside = judge([100.0, 90.0, 130.0], [125.0], LOWER, 5)  # 125 is inside the baseline's range
        self.assertEqual(inside.verdict, NOISE)
        outside = judge([100.0, 99.0, 101.0], [125.0], LOWER, 5)
        self.assertEqual(outside.verdict, REGRESSION)
        self.assertFalse(outside.confirmed)

    def test_zero_baseline(self):
        self.assertEqual(judge([0.0], [0.0], LOWER, 5).verdict, NOISE)
        v = judge([0.0, 0.0], [3.0, 4.0], LOWER, 5)
        self.assertEqual(v.verdict, REGRESSION)
        self.assertEqual(v.delta, float("inf"))

    def test_needs_a_run_on_each_side(self):
        with self.assertRaises(ValueError):
            judge([], [1.0], LOWER, 5)


class ThresholdTest(unittest.TestCase):
    def test_default_is_five_percent(self):
        self.assertEqual(compare.parse_thresholds(None).for_metric("a/b/p50"), 5.0)

    def test_bare_number_sets_the_default(self):
        self.assertEqual(compare.parse_thresholds(["12.5"]).for_metric("a/b/p50"), 12.5)

    def test_pattern_without_slash_matches_the_last_part(self):
        t = compare.parse_thresholds(["max=50", "p99*=15"])
        self.assertEqual(t.for_metric("orderbook/new/depth=10/take/max"), 50)
        self.assertEqual(t.for_metric("orderbook/new/depth=10/take/p99.9"), 15)
        self.assertEqual(t.for_metric("orderbook/new/depth=10/take/p50"), 5)
        self.assertEqual(t.for_metric("maxima/p50"), 5)  # 'max' must match the whole last part

    def test_pattern_with_slash_matches_the_whole_name(self):
        t = compare.parse_thresholds(["*/p99*=15", "orderbook/old/*=30"])
        self.assertEqual(t.for_metric("ring_latency/send_to_receive/p99.99"), 15)
        self.assertEqual(t.for_metric("orderbook/old/depth=10/take/p50"), 30)
        self.assertEqual(t.for_metric("p99"), 5)  # no '/' in the name, so '*/p99*' can't match

    def test_last_matching_pattern_wins(self):
        t = compare.parse_thresholds(["p99*=15", "*/take/p99.9=40"])
        self.assertEqual(t.for_metric("orderbook/new/depth=10/take/p99.9"), 40)
        self.assertEqual(t.for_metric("orderbook/new/depth=10/make/p99.9"), 15)
        reversed_order = compare.parse_thresholds(["*/take/p99.9=40", "p99*=15"])
        self.assertEqual(reversed_order.for_metric("orderbook/new/depth=10/take/p99.9"), 15)

    def test_pattern_may_contain_equals_sign_and_percent_suffix(self):
        t = compare.parse_thresholds(["orderbook/*/depth=1000/*=20%"])
        self.assertEqual(t.for_metric("orderbook/new/depth=1000/take/p50"), 20)
        self.assertEqual(t.for_metric("orderbook/new/depth=10/take/p50"), 5)

    def test_bad_thresholds_are_rejected(self):
        for bad in ("abc", "max=", "=5", "max=-1", "nan"):
            with self.subTest(bad=bad), self.assertRaises(compare.InputError):
                compare.parse_thresholds([bad])


class FilesTest(unittest.TestCase):
    def setUp(self):
        self._dir = tempfile.TemporaryDirectory()
        self.dir = self._dir.name

    def tearDown(self):
        self._dir.cleanup()

    def write(self, name, doc):
        path = os.path.join(self.dir, name)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(doc, f)
        return path

    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = compare.main(list(args))
        return code, out.getvalue(), err.getvalue()

    def test_minihft_file_is_keyed_by_label_and_null_values_are_skipped(self):
        path = self.write("a.json", minihft_doc({"orderbook/new/depth=10/take/p50": 20.0,
                                                 "orderbook/new/depth=10/mean_per_event": None,
                                                 "itch_replay/throughput": (2.9, HIGHER)}, label="ccd0"))
        run = compare.load_run(path)
        self.assertEqual(run.benchmark, "orderbook_latency")
        self.assertEqual(set(run.metrics), {("ccd0", "orderbook/new/depth=10/take/p50"), ("ccd0", "itch_replay/throughput")})
        self.assertEqual(run.metrics[("ccd0", "itch_replay/throughput")].better, HIGHER)

    def test_gbench_repetitions_reduce_to_their_median(self):
        run = compare.load_run(self.write("g.json", gbench_doc([3.0, 1.0, 2.0])))
        self.assertEqual(run.benchmark, "ring_buffer_bench")
        m = run.metrics[("", "ring_buffer_bench/BM_RingBuffer_PushPopSameThread/real_time")]
        self.assertEqual((m.value, m.unit, m.better), (2.0, "ns", LOWER))
        self.assertIn(("", "ring_buffer_bench/BM_RingBuffer_PushPopSameThread/cpu_time"), run.metrics)

    def test_gbench_aggregates_only_uses_the_median(self):
        run = compare.load_run(self.write("g.json", gbench_doc([1.0, 2.0, 9.0], aggregates_only=True)))
        self.assertEqual(run.metrics[("", "ring_buffer_bench/BM_RingBuffer_PushPopSameThread/real_time")].value, 2.0)

    def test_unknown_json_is_rejected(self):
        path = self.write("x.json", {"hello": 1})
        with self.assertRaises(compare.InputError):
            compare.load_run(path)

    def test_directory_stands_for_its_json_files(self):
        self.write("r1.json", minihft_doc({"m/p50": 1.0}))
        self.write("r2.json", minihft_doc({"m/p50": 2.0}))
        self.assertEqual(len(compare.expand_paths([self.dir])), 2)
        self.assertEqual(len(compare.expand_paths([os.path.join(self.dir, "r*.json")])), 2)
        with self.assertRaises(compare.InputError):
            compare.expand_paths([os.path.join(self.dir, "missing.json")])

    def test_labels_keep_same_named_metrics_apart(self):
        base = [compare.load_run(self.write("b1.json", minihft_doc({"ring_latency/send_to_receive/p50": 50.0}, label="same-ccd0"))),
                compare.load_run(self.write("b2.json", minihft_doc({"ring_latency/send_to_receive/p50": 175.0}, label="cross-ccd")))]
        cur = [compare.load_run(self.write("c1.json", minihft_doc({"ring_latency/send_to_receive/p50": 51.0}, label="same-ccd0"))),
               compare.load_run(self.write("c2.json", minihft_doc({"ring_latency/send_to_receive/p50": 260.0}, label="cross-ccd")))]
        rows, only_base, only_cur = compare.compare(base, cur, compare.Thresholds())
        self.assertEqual([(r.label, r.verdict.verdict) for r in rows], [("cross-ccd", REGRESSION), ("same-ccd0", NOISE)])
        self.assertEqual((only_base, only_cur), ([], []))

    def test_rows_sort_worst_regression_first_and_improvements_last(self):
        base = [compare.load_run(self.write("b.json", minihft_doc({"a": 100.0, "b": 100.0, "c": 100.0, "d": 100.0, "e": 100.0})))]
        cur = [compare.load_run(self.write("c.json", minihft_doc({"a": 110.0, "b": 150.0, "c": 101.0, "d": 50.0, "e": 80.0})))]
        rows, _, _ = compare.compare(base, cur, compare.Thresholds())
        self.assertEqual([r.name for r in rows], ["b", "a", "c", "d", "e"])

    def test_metadata_differences_are_warned_about(self):
        base = [compare.load_run(self.write("b.json", minihft_doc({"m": 1.0})))]
        same = [compare.load_run(self.write("s.json", minihft_doc({"m": 1.0})))]
        other = [compare.load_run(self.write("o.json", minihft_doc({"m": 1.0}, cpu="Other CPU", pinned=False,
                                                                     args=["--depths=1000"])))]
        self.assertEqual(compare.metadata_warnings(base, same), [])
        warnings = " ".join(compare.metadata_warnings(base, other))
        self.assertIn("CPU differs", warnings)
        self.assertIn("pinning differs", warnings)
        self.assertIn("different arguments", warnings)
        self.assertNotIn("compiler", warnings)

    def test_gbench_files_without_cpu_raise_no_warning(self):
        base = [compare.load_run(self.write("b.json", gbench_doc([1.0])))]
        cur = [compare.load_run(self.write("c.json", gbench_doc([1.0])))]
        self.assertEqual(compare.metadata_warnings(base, cur), [])

    def test_exit_code_is_1_on_regression_and_0_otherwise(self):
        b = [self.write(f"b{i}.json", minihft_doc({"orderbook/new/depth=10/take/p50": v})) for i, v in enumerate((20.0, 20.5, 19.8))]
        slow = [self.write(f"s{i}.json", minihft_doc({"orderbook/new/depth=10/take/p50": v})) for i, v in enumerate((30.0, 31.0, 29.5))]
        same = [self.write(f"n{i}.json", minihft_doc({"orderbook/new/depth=10/take/p50": v})) for i, v in enumerate((20.2, 19.9, 20.4))]
        code, out, _ = self.run_main(*b, "--current", *slow)
        self.assertEqual(code, 1)
        self.assertIn("REGRESSION", out)
        self.assertIn("1 regression,", out)
        code, out, _ = self.run_main(*b, "--current", *same)
        self.assertEqual(code, 0)
        self.assertIn("0 regressions, 0 improvements, 1 within noise", out)

    def test_single_runs_say_unconfirmed(self):
        b = self.write("b.json", minihft_doc({"m/p50": 20.0}))
        c = self.write("c.json", minihft_doc({"m/p50": 30.0}))
        code, out, _ = self.run_main(b, "--current", c)
        self.assertEqual(code, 1)
        self.assertIn("REGRESSION (unconfirmed)", out)
        self.assertIn("Unconfirmed:", out)

    def test_per_pattern_threshold_turns_a_regression_into_noise(self):
        b = self.write("b.json", minihft_doc({"m/max": 1000.0}))
        c = self.write("c.json", minihft_doc({"m/max": 1300.0}))
        self.assertEqual(self.run_main(b, "--current", c)[0], 1)
        self.assertEqual(self.run_main(b, "--current", c, "--threshold", "max=50")[0], 0)

    def test_markdown_has_a_table_and_the_warning(self):
        b = self.write("b.json", minihft_doc({"m/p50": 20.0}))
        c = self.write("c.json", minihft_doc({"m/p50": 30.0}, compiler="Clang 18"))
        code, out, _ = self.run_main(b, "--current", c, "--markdown")
        self.assertEqual(code, 1)
        self.assertIn("### Benchmark comparison", out)
        self.assertIn("| metric | unit | baseline |", out)
        self.assertIn("**REGRESSION (unconfirmed)**", out)
        self.assertIn("> **Warning:** orderbook_latency: compiler differs", out)
        self.assertIn("<details>", out)

    def test_only_changes_hides_noise_rows(self):
        b = self.write("b.json", minihft_doc({"quiet/p50": 20.0, "loud/p50": 20.0}))
        c = self.write("c.json", minihft_doc({"quiet/p50": 20.1, "loud/p50": 40.0}))
        _, out, _ = self.run_main(b, "--current", c, "--only-changes")
        self.assertIn("loud/p50", out)
        self.assertNotIn("quiet/p50", out)

    def test_bad_input_exits_2(self):
        b = self.write("b.json", minihft_doc({"m/p50": 20.0}))
        c = self.write("c.json", minihft_doc({"other/p50": 20.0}))
        code, _, err = self.run_main(b, "--current", c)
        self.assertEqual(code, 2)
        self.assertIn("no metric in common", err)
        code, _, err = self.run_main(b, "--current", os.path.join(self.dir, "missing.json"))
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()

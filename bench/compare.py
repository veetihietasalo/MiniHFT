#!/usr/bin/env python3
"""Compare benchmark results against a baseline; exit 1 if any metric regressed.

Reads what the latency harnesses write with --json=FILE (ring_latency, ring_study,
orderbook_latency, itch_replay) and Google Benchmark JSON (ring_buffer_bench
--benchmark_format=json, or --benchmark_out=FILE). Each side is one or more files, normally
repeated runs. A directory stands for every *.json file in it.

    compare.py BASELINE... --current CURRENT... [--threshold PCT] [--threshold PATTERN=PCT]...
               [--gate PATTERN]... [--markdown] [--only-changes]

For each metric found on both sides: the median of each side's runs, and the change in %.
A change counts only if
  - |change| is above the metric's threshold (default 5%), and
  - the run ranges don't overlap: the current's best run is worse than the baseline's worst
    (a regression), or the current's worst is better than the baseline's best (an improvement).
With 2 or more runs on both sides the verdict is confirmed. With a single run on either side the
ranges say nothing about run-to-run noise, so a regression or improvement is marked unconfirmed.
It still counts for the exit code.

--threshold PATTERN=PCT sets the threshold for metrics matching a glob pattern. A pattern without
'/' is matched against the last part of the name (max, p99*), one with '/' against the whole
name (orderbook/old/*). The last matching pattern wins. Tails are noisier than medians:
    --threshold 'p99*=15' --threshold max=50

--gate PATTERN limits the exit code to the metrics matching one of the patterns (matched like
--threshold patterns). The other metrics are still compared and listed, as information. Without
--gate every metric counts. Each metric gets its own verdict: over a hundred metrics, a 1-in-20
chance of noise separating the runs turns into several false regressions per comparison. Gate on
the central metrics and read the tails yourself:
    --gate min --gate p50 --gate 'mean_per_*' --gate throughput

Exit code: 0 no regression in a gated metric, 1 at least one, 2 bad input.
"""

import argparse
import dataclasses
import fnmatch
import glob
import json
import math
import os
import re
import statistics
import sys

LOWER, HIGHER = "lower", "higher"
REGRESSION, IMPROVEMENT, NOISE = "regression", "improvement", "noise"
DEFAULT_THRESHOLD = 5.0

# Metadata that must match for a comparison to mean anything.
CHECKED_FIELDS = (("cpu", "CPU"), ("compiler", "compiler"), ("build_type", "build type"), ("pinned", "pinning"))

# Google Benchmark rates: higher is better.
GBENCH_RATES = {"items_per_second": "items/s", "bytes_per_second": "bytes/s"}


class InputError(Exception):
    """A file or argument compare.py can't use."""


@dataclasses.dataclass
class Metric:
    unit: str
    better: str
    value: float


@dataclasses.dataclass
class Run:
    """One results file: its metadata and its metrics, keyed by (label, name)."""
    path: str
    meta: dict
    metrics: dict

    @property
    def benchmark(self):
        return self.meta.get("benchmark") or "?"

    @property
    def label(self):
        return self.meta.get("label") or ""


@dataclasses.dataclass
class Verdict:
    verdict: str      # REGRESSION, IMPROVEMENT or NOISE
    delta: float      # change of the median in %, current vs baseline
    worse: float      # delta signed so that > 0 means the current is worse
    confirmed: bool   # both sides had 2+ runs
    reason: str = ""  # for NOISE: "below threshold" or "ranges overlap"


@dataclasses.dataclass
class Row:
    label: str
    name: str
    unit: str
    better: str
    base: list
    cur: list
    threshold: float
    verdict: Verdict
    gated: bool = True  # counts for the exit code (--gate)

    @property
    def display(self):
        return f"[{self.label}] {self.name}" if self.label else self.name


# --- verdict ----------------------------------------------------------------------------------

def pct_change(base, cur):
    """Change from base to cur in %. From 0, any change is infinite."""
    if base == 0:
        return 0.0 if cur == 0 else math.copysign(math.inf, cur)
    return (cur - base) / abs(base) * 100.0


def judge(base, cur, better, threshold):
    """Verdict for one metric, from each side's per-run values."""
    if not base or not cur:
        raise ValueError("judge() needs at least one run on each side")
    delta = pct_change(statistics.median(base), statistics.median(cur))
    worse = delta if better == LOWER else -delta
    confirmed = len(base) >= 2 and len(cur) >= 2
    if not abs(delta) > threshold:  # written this way so that NaN counts as noise
        return Verdict(NOISE, delta, worse, confirmed, "below threshold")
    # Score each run so that higher is worse; then a regression needs the current's best run to
    # score worse than the baseline's worst, and an improvement the reverse.
    sign = 1.0 if better == LOWER else -1.0
    cur_scores = [sign * v for v in cur]
    base_scores = [sign * v for v in base]
    if worse > 0:
        verdict, separated = REGRESSION, min(cur_scores) > max(base_scores)
    else:
        verdict, separated = IMPROVEMENT, max(cur_scores) < min(base_scores)
    if not separated:
        return Verdict(NOISE, delta, worse, confirmed, "ranges overlap")
    return Verdict(verdict, delta, worse, confirmed)


# --- thresholds and gates ---------------------------------------------------------------------

def matches(name, pattern):
    """A pattern without '/' matches the last part of the name, one with '/' the whole name."""
    return fnmatch.fnmatchcase(name if "/" in pattern else name.rsplit("/", 1)[-1], pattern)


@dataclasses.dataclass
class Thresholds:
    default: float = DEFAULT_THRESHOLD
    rules: list = dataclasses.field(default_factory=list)  # (pattern, pct); the last match wins

    def for_metric(self, name):
        result = self.default
        for pattern, pct in self.rules:
            if matches(name, pattern):
                result = pct
        return result

    def describe(self):
        text = f"{self.default:g}%"
        if self.rules:
            text += " (" + ", ".join(f"{p}={pct:g}%" for p, pct in self.rules) + ")"
        return text


def parse_thresholds(values):
    thresholds = Thresholds()
    for value in values or []:
        pattern, sep, number = value.rpartition("=")  # rpartition: a pattern may contain '='
        try:
            pct = float(number.strip().rstrip("%"))
        except ValueError:
            raise InputError(f"--threshold {value!r}: expected PCT or PATTERN=PCT") from None
        if not math.isfinite(pct) or pct < 0:
            raise InputError(f"--threshold {value!r}: the threshold must be a percentage >= 0")
        if not sep:
            thresholds.default = pct
        elif not pattern:
            raise InputError(f"--threshold {value!r}: empty pattern")
        else:
            thresholds.rules.append((pattern, pct))
    return thresholds


# --- loading ----------------------------------------------------------------------------------

def expand_paths(paths):
    """Files to read: directories become their *.json files, and wildcards are expanded here
    too, because Windows shells leave them alone."""
    files = []
    for path in paths:
        if os.path.isdir(path):
            found = sorted(glob.glob(os.path.join(path, "*.json")))
            if not found:
                raise InputError(f"{path}: no .json files in this directory")
            files += found
        elif os.path.exists(path):
            files.append(path)
        elif any(c in path for c in "*?["):
            found = sorted(glob.glob(path))
            if not found:
                raise InputError(f"{path}: matches no file")
            files += found
        else:
            raise InputError(f"{path}: no such file or directory")
    return files


def load_run(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            doc = json.load(f)
    except (OSError, ValueError) as e:
        raise InputError(f"{path}: {e}") from None
    if isinstance(doc, dict) and isinstance(doc.get("benchmarks"), list):
        return parse_gbench(doc, path)
    if isinstance(doc, dict) and isinstance(doc.get("metrics"), list):
        return parse_minihft(doc, path)
    raise InputError(f"{path}: neither a MiniHFT --json file nor Google Benchmark JSON")


def parse_minihft(doc, path):
    meta = doc.get("metadata")
    meta = dict(meta) if isinstance(meta, dict) else {}
    label = meta.get("label") or ""
    metrics = {}
    for m in doc["metrics"]:
        if not isinstance(m, dict) or not isinstance(m.get("name"), str) or "value" not in m:
            raise InputError(f"{path}: malformed metric {m!r}")
        better = m.get("better", LOWER)
        if better not in (LOWER, HIGHER):
            raise InputError(f"{path}: metric {m['name']}: 'better' must be 'lower' or 'higher'")
        if m["value"] is None:  # the harness writes NaN and infinities as null
            continue
        try:
            value = float(m["value"])
        except (TypeError, ValueError):
            raise InputError(f"{path}: metric {m['name']}: value {m['value']!r} is not a number") from None
        metrics[(label, m["name"])] = Metric(str(m.get("unit", "")), better, value)
    return Run(path, meta, metrics)


def parse_gbench(doc, path):
    """Google Benchmark JSON. Metrics are <executable>/<benchmark>/real_time, /cpu_time and the
    rates it reports. Repetitions within one file are reduced to their median, so that one file
    stays one run; a file with aggregates only (--benchmark_report_aggregates_only) gives its
    median, or else its mean."""
    context = doc.get("context") if isinstance(doc.get("context"), dict) else {}
    exe = re.split(r"[\\/]", str(context.get("executable") or ""))[-1]
    if exe.lower().endswith(".exe"):
        exe = exe[:-4]
    bench = exe or "gbench"
    meta = {"benchmark": bench, "build_type": context.get("library_build_type"), "host": context.get("host_name")}

    iterations = {}  # run name -> its iteration entries, one per repetition
    aggregates = {}  # run name -> {aggregate name: entry}
    for b in doc["benchmarks"]:
        if not isinstance(b, dict) or b.get("error_occurred"):
            continue
        run_name = b.get("run_name") or b.get("name")
        if not run_name:
            continue
        if b.get("run_type") == "aggregate":
            aggregates.setdefault(run_name, {})[b.get("aggregate_name")] = b
        else:
            iterations.setdefault(run_name, []).append(b)

    metrics = {}
    for run_name in list(iterations) + [n for n in aggregates if n not in iterations]:
        entries = iterations.get(run_name)
        if not entries:
            entry = aggregates[run_name].get("median") or aggregates[run_name].get("mean")
            if entry is None:
                continue
            entries = [entry]
        time_unit = str(entries[0].get("time_unit", "ns"))
        fields = [("real_time", time_unit, LOWER), ("cpu_time", time_unit, LOWER)]
        fields += [(key, unit, HIGHER) for key, unit in GBENCH_RATES.items()]
        for key, unit, better in fields:
            values = [float(e[key]) for e in entries
                      if isinstance(e.get(key), (int, float)) and math.isfinite(e[key])]
            if values:
                metrics[("", f"{bench}/{run_name}/{key}")] = Metric(unit, better, statistics.median(values))
    return Run(path, meta, metrics)


def load_side(paths):
    runs = [load_run(p) for p in expand_paths(paths)]
    if not any(r.metrics for r in runs):
        raise InputError("no metrics in " + ", ".join(r.path for r in runs))
    return runs


# --- comparison -------------------------------------------------------------------------------

def collect(runs):
    """(label, name) -> (unit, better, [one value per run that has the metric])."""
    out = {}
    for run in runs:
        for key, m in run.metrics.items():
            entry = out.setdefault(key, (m.unit, m.better, []))
            entry[2].append(m.value)
    return out


def sort_key(row):
    """Gated regressions first, then the others, worst first; then noise, worst first; then
    improvements, best first."""
    worse = 0.0 if math.isnan(row.verdict.worse) else row.verdict.worse
    group = {REGRESSION: 0 if row.gated else 1, NOISE: 2, IMPROVEMENT: 3}[row.verdict.verdict]
    return (group, worse if group == 3 else -worse, row.display)


def compare(base_runs, cur_runs, thresholds, gates=()):
    """Rows for the metrics on both sides, sorted; plus the keys found on one side only.
    With gates, only the metrics matching one of them count for the exit code."""
    base, cur = collect(base_runs), collect(cur_runs)
    rows = []
    for key in base.keys() & cur.keys():
        unit, better, base_values = base[key]
        cur_values = cur[key][2]
        threshold = thresholds.for_metric(key[1])
        gated = not gates or any(matches(key[1], g) for g in gates)
        rows.append(Row(key[0], key[1], unit, better, base_values, cur_values, threshold,
                        judge(base_values, cur_values, better, threshold), gated))
    rows.sort(key=sort_key)
    return rows, sorted(base.keys() - cur.keys()), sorted(cur.keys() - base.keys())


def metadata_warnings(base_runs, cur_runs):
    """Differences in CPU, compiler, build type or pinning between the two sides, per benchmark,
    and different arguments for the same benchmark and label."""
    def values(runs, field):
        out = set()
        for r in runs:
            v = r.meta.get(field)
            if v is None or v == "":
                continue
            if field == "pinned":
                v = "pinned" if v else "not pinned"
            out.add(str(v))
        return out

    warnings = []
    for bench in sorted({r.benchmark for r in base_runs} & {r.benchmark for r in cur_runs}):
        b = [r for r in base_runs if r.benchmark == bench]
        c = [r for r in cur_runs if r.benchmark == bench]
        for field, what in CHECKED_FIELDS:
            bv, cv = values(b, field), values(c, field)
            if bv and cv and bv != cv:
                warnings.append(f"{bench}: {what} differs (baseline: {'; '.join(sorted(bv))}, "
                                f"current: {'; '.join(sorted(cv))}). Comparing these runs is meaningless.")
        for label in sorted({r.label for r in b} & {r.label for r in c}):
            ba = {" ".join(r.meta["args"]) for r in b if r.label == label and isinstance(r.meta.get("args"), list)}
            ca = {" ".join(r.meta["args"]) for r in c if r.label == label and isinstance(r.meta.get("args"), list)}
            if ba and ca and ba != ca:
                name = f"{bench} [{label}]" if label else bench
                warnings.append(f"{name}: run with different arguments (baseline: {' | '.join(sorted(ba))}, "
                                f"current: {' | '.join(sorted(ca))}).")
    return warnings


# --- output -----------------------------------------------------------------------------------

def fmt(v):
    if math.isnan(v):
        return "nan"
    if math.isinf(v):
        return "inf" if v > 0 else "-inf"
    a = abs(v)
    if a >= 1000:
        return f"{v:,.0f}"
    if a >= 100:
        return f"{v:.1f}"
    if a >= 1:
        return f"{v:.2f}"
    return f"{v:.3g}"


def fmt_values(values):
    text = fmt(statistics.median(values))
    if len(values) > 1:
        text += f" ({fmt(min(values))}..{fmt(max(values))})"
    return text


def fmt_delta(delta):
    if math.isnan(delta):
        return "nan"
    if math.isinf(delta):
        return "+inf%" if delta > 0 else "-inf%"
    return f"{delta:+.1f}%"


def verdict_text(row):
    v = row.verdict
    if v.verdict == NOISE:
        return "noise (overlap)" if v.reason == "ranges overlap" else "noise"
    if v.verdict == REGRESSION:
        text = "REGRESSION" if row.gated else "slower (not gated)"
    else:
        text = "improved"
    return text if v.confirmed else text + " (unconfirmed)"


def cells(row):
    return [row.display, row.unit, fmt_values(row.base), fmt_values(row.cur), fmt_delta(row.verdict.delta),
            f"{row.threshold:g}%", f"{len(row.base)}/{len(row.cur)}", verdict_text(row)]


HEADER = ["metric", "unit", "baseline", "current", "delta", "threshold", "runs", "verdict"]
RIGHT_ALIGNED = {2, 3, 4, 5, 6}


def describe_side(runs):
    commits = sorted({str(r.meta.get("commit")) for r in runs if r.meta.get("commit")})
    text = f"{len(runs)} file{'s' if len(runs) != 1 else ''}"
    if commits:
        text += f", commit{'s' if len(commits) > 1 else ''} {', '.join(commits)}"
    return text


def counts_line(rows, only_base, only_cur):
    n = {k: sum(1 for r in rows if r.verdict.verdict == k) for k in (REGRESSION, IMPROVEMENT, NOISE)}
    ungated = sum(1 for r in rows if r.verdict.verdict == REGRESSION and not r.gated)
    gated = n[REGRESSION] - ungated
    text = f"{gated} regression{'s' if gated != 1 else ''}"
    if ungated:
        text += f" ({ungated} more slower in metrics not gated)"
    text += (f", {n[IMPROVEMENT]} improvement{'s' if n[IMPROVEMENT] != 1 else ''}, "
             f"{n[NOISE]} within noise")
    if only_base or only_cur:
        text += f"; {len(only_base)} metrics only in the baseline, {len(only_cur)} only in the current"
    return text + "."


def unconfirmed_note(rows):
    if any(r.verdict.verdict != NOISE and not r.verdict.confirmed for r in rows):
        return ("Unconfirmed: a side had a single run, so run-to-run noise is unknown; "
                "repeat the runs (2+ per side) to confirm.")
    return ""


def key_text(key):
    return f"[{key[0]}] {key[1]}" if key[0] else key[1]


RULE = ("A change counts only if |delta| > threshold and the run ranges don't overlap "
        "(with 2+ runs per side).")


def gate_text(gates):
    return f"Gated (counts for the exit code): {', '.join(gates)}." if gates else ""


def render_text(base_runs, cur_runs, thresholds, rows, only_base, only_cur, warnings, only_changes, gates=()):
    out = [f"Baseline: {describe_side(base_runs)}. Current: {describe_side(cur_runs)}.",
           f"Thresholds: {thresholds.describe()}. {RULE}"]
    if gates:
        out.append(gate_text(gates))
    out += [f"WARNING: {w}" for w in warnings]
    shown = [r for r in rows if r.verdict.verdict != NOISE] if only_changes else rows
    if shown:
        table = [HEADER] + [cells(r) for r in shown]
        widths = [max(len(line[i]) for line in table) for i in range(len(HEADER))]
        out.append("")
        for line in table:
            out.append("  ".join(c.rjust(w) if i in RIGHT_ALIGNED else c.ljust(w)
                                 for i, (c, w) in enumerate(zip(line, widths))).rstrip())
    out.append("")
    for title, keys in (("Only in the baseline", only_base), ("Only in the current", only_cur)):
        if keys:
            more = f" and {len(keys) - 10} more" if len(keys) > 10 else ""
            out.append(f"{title}: {', '.join(key_text(k) for k in keys[:10])}{more}")
    out.append(counts_line(rows, only_base, only_cur))
    note = unconfirmed_note(rows)
    if note:
        out.append(note)
    return "\n".join(out) + "\n"


def md_escape(text):
    return text.replace("\\", "\\\\").replace("|", "\\|")


def md_table(rows):
    out = ["| " + " | ".join(HEADER) + " |",
           "|" + "|".join("---:" if i in RIGHT_ALIGNED else "---" for i in range(len(HEADER))) + "|"]
    for row in rows:
        c = cells(row)
        c[0] = f"`{c[0]}`"
        if row.verdict.verdict == REGRESSION and row.gated:
            c[-1] = f"**{c[-1]}**"
        out.append("| " + " | ".join(md_escape(x) for x in c) + " |")
    return out


def render_markdown(base_runs, cur_runs, thresholds, rows, only_base, only_cur, warnings, gates=()):
    out = ["### Benchmark comparison", "",
           f"Baseline: {md_escape(describe_side(base_runs))}. Current: {md_escape(describe_side(cur_runs))}.  ",
           f"Thresholds: {md_escape(thresholds.describe())}. {RULE}" + ("  " if gates else "")]
    if gates:
        out.append(md_escape(gate_text(gates)))
    out.append("")
    for w in warnings:
        out += [f"> **Warning:** {md_escape(w)}", ""]
    out += [f"**{counts_line(rows, only_base, only_cur)}**", ""]
    note = unconfirmed_note(rows)
    if note:
        out += [note, ""]
    changed = [r for r in rows if r.verdict.verdict != NOISE]
    if changed:
        out += md_table(changed) + [""]
    if rows:
        out += [f"<details><summary>All {len(rows)} metrics</summary>", ""] + md_table(rows) + ["", "</details>", ""]
    return "\n".join(out)


# --- command line -----------------------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="compare.py",
        description="Compare benchmark results against a baseline; exit 1 if any metric regressed.",
        epilog="Example: compare.py base-run*.json --current head-run*.json --threshold 'p99*=15' --threshold max=50")
    parser.add_argument("baseline", nargs="+", help="baseline results: JSON files or directories of them")
    parser.add_argument("--current", nargs="+", required=True, help="current results: JSON files or directories")
    parser.add_argument("--threshold", action="append", metavar="[PATTERN=]PCT",
                        help=f"change in %% that counts (default {DEFAULT_THRESHOLD:g}); with PATTERN, only for "
                             "matching metrics. Repeatable; the last matching pattern wins")
    parser.add_argument("--gate", action="append", default=[], metavar="PATTERN",
                        help="only metrics matching a PATTERN count for the exit code; the rest are listed as "
                             "information. Repeatable. Default: every metric counts")
    parser.add_argument("--markdown", action="store_true", help="print Markdown, e.g. for a CI job summary")
    parser.add_argument("--only-changes", action="store_true", help="list only regressions and improvements")
    args = parser.parse_args(argv)

    try:
        thresholds = parse_thresholds(args.threshold)
        base_runs, cur_runs = load_side(args.baseline), load_side(args.current)
        rows, only_base, only_cur = compare(base_runs, cur_runs, thresholds, args.gate)
        if not rows:
            raise InputError("the baseline and the current have no metric in common")
    except InputError as e:
        print(f"compare.py: error: {e}", file=sys.stderr)
        return 2

    warnings = metadata_warnings(base_runs, cur_runs)
    if args.markdown:
        sys.stdout.write(render_markdown(base_runs, cur_runs, thresholds, rows, only_base, only_cur, warnings,
                                         args.gate))
    else:
        sys.stdout.write(render_text(base_runs, cur_runs, thresholds, rows, only_base, only_cur, warnings,
                                     args.only_changes, args.gate))
    return 1 if any(r.verdict.verdict == REGRESSION and r.gated for r in rows) else 0


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")  # a console that can't show a character gets '?'
    sys.exit(main())

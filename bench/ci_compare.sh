#!/usr/bin/env bash
# A/B benchmark comparison for CI: the bench-compare job in .github/workflows/build.yml.
#
# Builds a base commit and the working tree in two build directories, runs orderbook_latency and
# ring_buffer_bench on each, alternating base and head, and compares the two with compare.py.
# On a shared runner absolute numbers are noise, but base and head measured on the same machine in
# the same few minutes can be compared, within wide thresholds.
#
# The base is the merge-base with origin/main; on main itself, or with no merge-base, HEAD~1.
# Report only: a regression gives a ::warning:: annotation, not a failure. The script fails only if
# the head can't be built or run, or compare.py can't read the results.
#
# Environment, all optional:
#   BASE_REF  compare against this commit instead
#   RUNS      runs per side, alternating base and head (default 5)
#   OUT_DIR   work directory (default build/bench-compare)
#   JOBS      parallel build jobs (default: every CPU)
#   DRY_RUN   1: print the base and head, build and run nothing
#   GITHUB_STEP_SUMMARY  set by GitHub Actions: the Markdown report is appended to it

set -euo pipefail
shopt -s nullglob

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runs="${RUNS:-5}"
out="${OUT_DIR:-$root/build/bench-compare}"
jobs="${JOBS:-$(nproc)}"
summary="${GITHUB_STEP_SUMMARY:-}"

# Thresholds in %. A GitHub runner is a shared 4-vCPU VM: medians move by several % between runs
# and tails by far more, so only large changes are reported.
thresholds=(--threshold 10 --threshold 'p99=25' --threshold 'p99.9=50' --threshold 'p99.99=100' --threshold 'max=200')
# Only the central metrics raise the warning; tails are listed for reading. With ~120 metrics, noise
# alone separates a few of them in a no-op comparison (measured: 1 tail "regression" and 5 tail
# "improvements" for a comment-only change), while a real +50 ns shows in every gated take metric.
gates=(--gate min --gate p50 --gate 'mean_per_*' --gate real_time --gate cpu_time)
orderbook_args=(--events=200000 --depths=10,1000)
gbench_args=(--benchmark_min_time=0.5s)
pin=() # ring_buffer_bench doesn't pin itself; orderbook_latency pins itself to core 2
if command -v taskset > /dev/null && (($(nproc) > 2)); then pin=(taskset -c 2); fi

to_summary() { if [[ -n $summary ]]; then printf '%s\n' "$@" >> "$summary"; fi; }
skip() {
    echo "Skipped: $1"
    to_summary "## Benchmarks: base vs head" "" "Skipped: $1" ""
    exit 0
}
in_repo() { git -C "$root" "$@"; }

# --- which commits -------------------------------------------------------------------------------
head_sha="$(in_repo rev-parse HEAD)"
head_label="${head_sha:0:12}"
if ! in_repo diff --quiet HEAD --; then head_label+="-dirty"; fi # the head is the working tree

if [[ -n ${BASE_REF:-} ]]; then
    base_sha="$(in_repo rev-parse --verify "$BASE_REF^{commit}")"
    why="BASE_REF=$BASE_REF"
else
    base_sha=""
    if in_repo rev-parse -q --verify 'origin/main^{commit}' > /dev/null; then
        base_sha="$(in_repo merge-base origin/main HEAD || true)"
        why="merge-base with origin/main"
    fi
    if [[ -z $base_sha || $base_sha == "$head_sha" ]]; then
        base_sha="$(in_repo rev-parse -q --verify 'HEAD~1^{commit}' || true)"
        why="HEAD~1: this is main, or it has no merge-base with main"
    fi
fi
[[ -n $base_sha ]] || skip "no base commit to compare against: no origin/main and no parent commit."
base_label="${base_sha:0:12}"
echo "base: $base_label ($why)"
echo "head: $head_label"
if [[ ${DRY_RUN:-} == 1 ]]; then exit 0; fi

# --- build both ----------------------------------------------------------------------------------
mkdir -p "$out"
base_src="$out/base-src"
if [[ ! -f $base_src/.base-commit || $(< "$base_src/.base-commit") != "$base_sha" ]]; then
    rm -rf "$base_src" "$out/base"
    mkdir -p "$base_src"
    in_repo archive "$base_sha" | tar -x -C "$base_src"
    echo "$base_sha" > "$base_src/.base-commit"
fi

# build NAME SOURCE: the two benchmarks into $out/NAME, with the same flags on both sides. Warnings
# don't fail this build (the build job checks them): an older base may not be clean under a newer
# compiler.
build() {
    local dir="$out/$1"
    echo "== building $1 from $2"
    if ! cmake -S "$2" -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ \
        -DMINIHFT_BUILD_TESTS=OFF -DMINIHFT_BUILD_BENCHMARKS=ON -DMINIHFT_WARNINGS_AS_ERRORS=OFF > "$dir.log" 2>&1 ||
        ! cmake --build "$dir" -j "$jobs" --target orderbook_latency ring_buffer_bench >> "$dir.log" 2>&1; then
        tail -n 40 "$dir.log"
        return 1
    fi
}
build head "$root" || { echo "::error::The head doesn't build (log above)."; exit 1; }
build base "$base_src" || skip "the base $base_label doesn't build, so there is nothing to compare against."

# --- run, alternating ----------------------------------------------------------------------------
results="$out/results"
rm -rf "$results"
mkdir -p "$results/base" "$results/head"

# run_side SIDE N: one run of each benchmark. A failure on the head stops the script; on the base,
# that run is left out.
run_side() {
    local side=$1 n=$2 commit=$base_label
    [[ $side == head ]] && commit=$head_label
    local bin="$out/$side" res="$results/$side"
    if ! "$bin/orderbook_latency" "${orderbook_args[@]}" --json="$res/orderbook-run$n.json" --commit="$commit" \
        > "$res/orderbook-run$n.log" 2>&1; then
        echo "orderbook_latency failed ($side, run $n):"
        tail -n 20 "$res/orderbook-run$n.log"
        [[ $side == base ]] || exit 1
    fi
    if ! "${pin[@]}" "$bin/ring_buffer_bench" "${gbench_args[@]}" --benchmark_out="$res/ring_buffer_bench-run$n.json" \
        --benchmark_out_format=json > "$res/ring_buffer_bench-run$n.log" 2>&1; then
        echo "ring_buffer_bench failed ($side, run $n):"
        tail -n 20 "$res/ring_buffer_bench-run$n.log"
        [[ $side == base ]] || exit 1
    fi
}

echo "== $runs runs per side, alternating base and head"
for ((n = 1; n <= runs; n++)); do
    if ((n % 2)); then order=(base head); else order=(head base); fi # ABBA: drift hits both alike
    for side in "${order[@]}"; do run_side "$side" "$n"; done
done

# --- compare -------------------------------------------------------------------------------------
base_files=()
head_files=()
notes=()
for bench in orderbook ring_buffer_bench; do
    b=("$results/base/$bench"-run*.json)
    h=("$results/head/$bench"-run*.json)
    if ((${#h[@]} == 0)); then echo "::error::The head wrote no $bench results."; exit 1; fi
    if ((${#b[@]} == 0)); then
        notes+=("- The base wrote no $bench results (its harness may predate --json), so $bench isn't compared.")
        continue
    fi
    base_files+=("${b[@]}")
    head_files+=("${h[@]}")
done
for note in "${notes[@]}"; do echo "${note#- }"; done
((${#base_files[@]} > 0)) || skip "the base $base_label wrote no results this script can compare."

set +e
python3 "$root/bench/compare.py" "${base_files[@]}" --current "${head_files[@]}" "${thresholds[@]}" "${gates[@]}"
rc=$?
markdown="$(python3 "$root/bench/compare.py" "${base_files[@]}" --current "${head_files[@]}" "${thresholds[@]}" "${gates[@]}" --markdown)"
set -e

to_summary "## Benchmarks: base vs head" "" \
    "Base \`$base_label\` ($why) against head \`$head_label\`: same build flags, $runs runs each, alternating, on one shared runner." \
    "Its numbers are noisy, so only changes beyond wide thresholds count. Confirm one on the benchmark machine before trusting it." \
    "" "${notes[@]}" "" "$markdown"

case $rc in
    0) echo "No regression beyond the thresholds." ;;
    1) echo "::warning title=Benchmark regression::compare.py reports a regression against the base $base_label ($why). Shared runners are noisy: see the job summary, and confirm on the benchmark machine before trusting it." ;;
    *) echo "::error::compare.py failed with exit code $rc."; exit 1 ;;
esac

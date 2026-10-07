#!/usr/bin/env bash
# Linux counterpart of run_ccd_matrix.ps1: runs the latency harnesses on a core pair inside
# each CCD and on a pair across CCDs, and can replay an ITCH day once on each CCD.
#
# Reads the topology from sysfs (one L3 per CCD, the SMT siblings of each core), so the same
# command works with one CCD or two and with SMT on or off. Producer and consumer always sit
# on different physical cores, using the third and fourth physical core of a CCD.
#
# Usage: bench/run_ccd_matrix.sh [--runs=2] [--itch=FILE] [--no-build] [--dry-run]
#   --runs=N     repetitions of the whole matrix; each run goes through every pair in turn
#   --itch=FILE  ITCH 5.0 day, gzipped or not: also replay it once on each CCD
#   --no-build   skip the cmake configure and build
#   --dry-run    print the topology and the pairs, run nothing
#
# --high-priority (SCHED_FIFO) needs root. Build as yourself, then run the matrix with sudo:
#   bench/run_ccd_matrix.sh --dry-run && sudo bench/run_ccd_matrix.sh --no-build --itch=...
# Output goes to the console and to build/bench-results/<timestamp>.log.
# Only native Linux gives meaningful CCD numbers: in a VM or WSL2, pinning picks virtual CPUs.

set -euo pipefail

runs=2
itch=""
build=1
dry=0
for arg in "$@"; do
    case "$arg" in
        --runs=*) runs="${arg#*=}" ;;
        --itch=*) itch="${arg#*=}" ;;
        --no-build) build=0 ;;
        --dry-run) dry=1 ;;
        -h | --help) sed -n '2,19p' "$0"; exit 0 ;;
        *) echo "unknown option: $arg (see --help)" >&2; exit 2 ;;
    esac
done
if [[ -n $itch && ! -r $itch ]]; then echo "cannot read $itch" >&2; exit 2; fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bin="$root/build/gcc-release"
sys="${MINIHFT_SYSFS:-/sys/devices/system/cpu}" # overridable so the topology code can be tested

# "0-5,12-17" -> "0 1 2 3 4 5 12 13 14 15 16 17"
expand() {
    local part out=()
    local -a parts nums
    IFS=',' read -ra parts <<< "$1"
    for part in "${parts[@]}"; do
        if [[ $part == *-* ]]; then
            read -ra nums <<< "$(seq -s ' ' "${part%-*}" "${part#*-}")"
            out+=("${nums[@]}")
        else
            out+=("$part")
        fi
    done
    echo "${out[@]}"
}

# Element $2 of the space-separated list $1, or its last element if the list is shorter.
pick() {
    local -a a
    read -ra a <<< "$1"
    local i=$2
    ((i < ${#a[@]})) || i=$((${#a[@]} - 1))
    echo "${a[$i]}"
}

# --- topology ---------------------------------------------------------------------------------
online="$(cat "$sys/online")"
read -ra cpus <<< "$(expand "$online")"
primaries=()             # first logical CPU of each physical core
declare -A l3_of=()      # cpu -> its L3's shared_cpu_list
declare -A l3_size=()    # shared_cpu_list -> size as sysfs prints it
for c in "${cpus[@]}"; do
    sib="$(cat "$sys/cpu$c/topology/thread_siblings_list")"
    [[ ${sib%%[,-]*} == "$c" ]] && primaries+=("$c")
    l3_of[$c]="$online" # no L3 reported: treat the machine as one group
    for idx in "$sys/cpu$c/cache"/index*; do
        [[ -r $idx/level && $(cat "$idx/level") == 3 ]] || continue
        l3_of[$c]="$(cat "$idx/shared_cpu_list")"
        l3_size[${l3_of[$c]}]="$(cat "$idx/size")"
    done
done

l3_lists=() # one per CCD, in order of their first CPU
for c in "${cpus[@]}"; do
    seen=0
    for l in "${l3_lists[@]}"; do [[ $l == "${l3_of[$c]}" ]] && seen=1; done
    ((seen)) || l3_lists+=("${l3_of[$c]}")
done

ccds=() # physical cores of each CCD, as logical CPU numbers the harnesses can pin to
for l in "${l3_lists[@]}"; do
    members=" $(expand "$l") "
    cores=()
    for p in "${primaries[@]}"; do [[ $members == *" $p "* ]] && cores+=("$p"); done
    ccds+=("${cores[*]}")
done

names=() producers=() consumers=()
add_pair() { names+=("$1"); producers+=("$2"); consumers+=("$3"); }
count() { local -a a; read -ra a <<< "$1"; echo "${#a[@]}"; }
if (($(count "${ccds[0]}") >= 2)); then add_pair "same CCD0" "$(pick "${ccds[0]}" 2)" "$(pick "${ccds[0]}" 3)"; fi
if ((${#ccds[@]} >= 2)); then
    if (($(count "${ccds[1]}") >= 2)); then add_pair "same CCD1" "$(pick "${ccds[1]}" 2)" "$(pick "${ccds[1]}" 3)"; fi
    add_pair "cross CCD" "$(pick "${ccds[0]}" 2)" "$(pick "${ccds[1]}" 2)"
fi
if ((${#names[@]} == 0)); then echo "found no CCD with two physical cores" >&2; exit 1; fi

# --- machine state that changes the numbers ---------------------------------------------------
read_or() { cat "$1" 2>/dev/null || echo "$2"; }
virt="$(systemd-detect-virt 2>/dev/null || true)"
[[ -z $virt ]] && grep -qi microsoft /proc/version 2>/dev/null && virt="wsl"
[[ -z $virt ]] && virt="none"
thp="$(read_or /sys/kernel/mm/transparent_hugepage/enabled n/a)"
thp="${thp#*\[}" && thp="${thp%%\]*}"
rt_runtime="$(read_or /proc/sys/kernel/sched_rt_runtime_us n/a)"
if ((EUID == 0)); then priority="SCHED_FIFO (root)"; else priority="normal (SCHED_FIFO needs root)"; fi

header=(
    "# MiniHFT CCD matrix, $(date '+%Y-%m-%d %H:%M'), commit $(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    "cpu         $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"
    "cores       ${#primaries[@]} physical, ${#cpus[@]} logical, SMT $( ((${#primaries[@]} < ${#cpus[@]})) && echo on || echo off)"
)
for i in "${!l3_lists[@]}"; do
    size="${l3_size[${l3_lists[$i]}]:-?}"
    [[ $size == *K ]] && size="$((${size%K} / 1024)) MB"
    header+=("CCD$i        L3 $size, logical CPUs ${l3_lists[$i]}, physical cores at ${ccds[$i]// /,}")
done
header+=(
    "kernel      $(uname -r), virtualization: $virt"
    "cpufreq     governor $(read_or "$sys/cpu0/cpufreq/scaling_governor" n/a), EPP $(read_or "$sys/cpu0/cpufreq/energy_performance_preference" n/a), amd-pstate $(read_or "$sys/amd_pstate/status" n/a)"
    "isolation   isolcpus [$(read_or "$sys/isolated" n/a)], nohz_full [$(read_or "$sys/nohz_full" n/a)]"
    "memory      transparent huge pages: $thp"
    "priority    $priority, sched_rt_runtime_us $rt_runtime"
)
for i in "${!names[@]}"; do header+=("$(printf 'pair        %-10s producer %s -> consumer %s' "${names[$i]}" "${producers[$i]}" "${consumers[$i]}")"); done
[[ -n $itch ]] && header+=("itch        $itch")

warnings=()
[[ $virt != none ]] && warnings+=("WARNING: running under $virt. Pinning picks virtual CPUs, so the CCD comparison means nothing here.")
((EUID == 0)) && [[ $rt_runtime != -1 ]] &&
    warnings+=("NOTE: RT throttling is on (sched_rt_runtime_us=$rt_runtime). On kernels that still throttle, a spinning SCHED_FIFO thread can be stopped for up to 50 ms per second, which lands in max.")

if ((dry)); then
    printf '%s\n' "${header[@]}" "${warnings[@]}"
    exit 0
fi

# --- build and run ----------------------------------------------------------------------------
if ((build)); then
    (cd "$root" && cmake --preset gcc-release && cmake --build --preset gcc-release)
fi

out_dir="$root/build/bench-results"
mkdir -p "$out_dir"
log_file="$out_dir/$(date '+%Y%m%d-%H%M%S').log"
log() { printf '%s\n' "$@" | tee -a "$log_file"; }

run() {
    log "" "> $*"
    set +e
    "$bin/$1" "${@:2}" 2>&1 | tee -a "$log_file"
    local rc=${PIPESTATUS[0]}
    set -e
    ((rc == 0)) || log "!! $1 exited with code $rc"
}

replay() { # $1 = core for itch_replay, $2 = core for gzip (same CCD, another physical core)
    local rc
    set +e
    if [[ $itch == *.gz ]]; then
        log "" "> gzip -dc $itch (core $2) | itch_replay - --core=$1"
        taskset -c "$2" gzip -dc "$itch" | "$bin/itch_replay" - --core="$1" 2>&1 | tee -a "$log_file"
        rc=${PIPESTATUS[1]}
    else
        log "" "> itch_replay $itch --core=$1"
        "$bin/itch_replay" "$itch" --core="$1" 2>&1 | tee -a "$log_file"
        rc=${PIPESTATUS[0]}
    fi
    set -e
    ((rc == 0)) || log "!! itch_replay exited with code $rc"
}

log "${header[@]}" "${warnings[@]}"

for ((pass = 1; pass <= runs; pass++)); do
    for i in "${!names[@]}"; do
        log "" "## run $pass, ${names[$i]}: producer ${producers[$i]} -> consumer ${consumers[$i]}"
        pc=("--producer=${producers[$i]}" "--consumer=${consumers[$i]}" "--high-priority")
        run ring_latency "${pc[@]}"
        run ring_study "${pc[@]}"
        run ring_study "${pc[@]}" --producer-work-ns=20 --burst=5000000 --paced=200000
        run ring_study "${pc[@]}" --consumer-work-ns=20 --burst=5000000 --paced=200000
    done
    # Single-threaded: only the CCD (and its L3 size) can matter.
    for i in "${!ccds[@]}"; do
        log "" "## run $pass, orderbook_latency on CCD$i"
        run orderbook_latency "--core=$(pick "${ccds[$i]}" 2)"
    done
done

if [[ -n $itch ]]; then
    for i in "${!ccds[@]}"; do
        log "" "## itch_replay on CCD$i"
        replay "$(pick "${ccds[$i]}" 2)" "$(pick "${ccds[$i]}" 4)"
    done
fi

log "" "Results written to $log_file"

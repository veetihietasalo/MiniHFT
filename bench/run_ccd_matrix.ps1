<#
.SYNOPSIS
    Runs the latency harnesses on a core pair inside each CCD and on a pair across CCDs.

.DESCRIPTION
    Reads the topology from Windows (one L3 cache per CCD, one entry per physical core), so the
    same command works with one CCD or two, and with SMT on or off. Producer and consumer always
    sit on different physical cores, never on two SMT siblings.

    Pairs, using the third and fourth physical core of a CCD to stay off core 0:
      same CCD0    CCD0 -> CCD0   (the pair the README and docs/ring_buffer_v2.md use)
      same CCD1    CCD1 -> CCD1   (only with two CCDs)
      cross CCD    CCD0 -> CCD1   (only with two CCDs)

    Each run executes every pair once, then the next run starts, so drift over time hits every
    pair alike. Everything runs at high priority. Output goes to the console and to
    build\bench-results\<timestamp>.log. Each harness run also writes its results as JSON, to
    build\bench-results\<timestamp>\<pair>-<bench>-run<N>.json.

    -SaveBaseline keeps this run's JSON results as the baseline, in build\bench-baseline\.
    -Check compares this run with that baseline (bench\compare.py, needs Python 3) and exits
    with 1 on a regression.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File bench\run_ccd_matrix.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File bench\run_ccd_matrix.ps1 -NoBuild -Runs 3 -ItchFile D:\itch\12302019.NASDAQ_ITCH50

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File bench\run_ccd_matrix.ps1 -SaveBaseline   # on the commit to compare against
    powershell -ExecutionPolicy Bypass -File bench\run_ccd_matrix.ps1 -Check          # later, on the commit to check
#>
param(
    [int]$Runs = 2,             # repetitions of the whole matrix
    [string]$ItchFile = "",     # uncompressed ITCH 5.0 day: also replay it once on each CCD
    [switch]$NoBuild,           # skip the cmake configure and build
    [switch]$SaveBaseline,      # keep this run's JSON results as the baseline
    [switch]$Check              # compare this run with the baseline; exit 1 on a regression
)

$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\msvc-release\Release"
$baselineDir = Join-Path $root "build\bench-baseline"
# -Check thresholds in %: the tail is set by the OS more than by the code, and varies far more
# between runs than the median does.
$thresholds = @("--threshold", "5", "--threshold", "p99=15", "--threshold", "p99.9=30",
                "--threshold", "p99.99=50", "--threshold", "max=100")
# Only the central metrics fail the check; the tails are listed for reading. Every metric gets its
# own verdict, so across a hundred-odd metrics noise alone would fail some check every run.
$gates = @("--gate", "min", "--gate", "p50", "--gate", "mean_per_*", "--gate", "throughput", "--gate", "ns_per_msg")

if ([IntPtr]::Size -ne 8) { throw "Run this from 64-bit PowerShell." }
if ($SaveBaseline -and $Check) { throw "-SaveBaseline and -Check exclude each other." }

$python = $null
if ($Check) {
    if (-not (Test-Path (Join-Path $baselineDir "*.json"))) {
        throw "No baseline in $baselineDir`: save one first with -SaveBaseline."
    }
    # The py launcher first: "python" may be the Microsoft Store stub.
    foreach ($candidate in @("py", "python", "python3")) {
        if (Get-Command $candidate -ErrorAction SilentlyContinue) { $python = $candidate; break }
    }
    if ($null -eq $python) { throw "-Check needs Python 3 (py, python or python3 on PATH)." }
}

if (-not $NoBuild) {
    Push-Location $root
    cmake --preset msvc-release
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "cmake configure failed" }
    cmake --build --preset msvc-release
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "cmake build failed" }
    Pop-Location
}

Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;

public class CpuGroup {
    public int[] Cpus;
    public long CacheBytes;
}

public static class MiniHftTopology {
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetLogicalProcessorInformation(IntPtr buffer, ref uint returnLength);

    // relationship 0 = one entry per physical core, 2 = one entry per cache (filtered by level).
    // Entries are SYSTEM_LOGICAL_PROCESSOR_INFORMATION, 32 bytes on x64: ProcessorMask at 0,
    // Relationship at 8, then the union; for a cache, Level at 16 and Size at 20.
    public static CpuGroup[] Groups(int relationship, int cacheLevel) {
        uint len = 0;
        GetLogicalProcessorInformation(IntPtr.Zero, ref len);
        IntPtr buf = Marshal.AllocHGlobal((int)len);
        try {
            if (!GetLogicalProcessorInformation(buf, ref len)) throw new Win32Exception();
            return Parse(buf, (int)len, relationship, cacheLevel);
        } finally {
            Marshal.FreeHGlobal(buf);
        }
    }

    public static CpuGroup[] Parse(IntPtr buf, int len, int relationship, int cacheLevel) {
        List<CpuGroup> groups = new List<CpuGroup>();
        for (int off = 0; off + 32 <= len; off += 32) {
            if (Marshal.ReadInt32(buf, off + 8) != relationship) continue;
            if (relationship == 2 && Marshal.ReadByte(buf, off + 16) != cacheLevel) continue;
            ulong mask = (ulong)Marshal.ReadInt64(buf, off);
            List<int> cpus = new List<int>();
            for (int i = 0; i < 64; i++) if ((mask & (1UL << i)) != 0) cpus.Add(i);
            CpuGroup g = new CpuGroup();
            g.Cpus = cpus.ToArray();
            g.CacheBytes = relationship == 2 ? (long)(uint)Marshal.ReadInt32(buf, off + 20) : 0;
            groups.Add(g);
        }
        groups.Sort((a, b) => a.Cpus[0].CompareTo(b.Cpus[0]));
        return groups.ToArray();
    }
}
"@

$l3 = [MiniHftTopology]::Groups(2, 3)      # one per CCD
$cores = [MiniHftTopology]::Groups(0, 0)   # one per physical core
$smt = @($cores | Where-Object { $_.Cpus.Count -gt 1 }).Count -gt 0
$primary = @($cores | ForEach-Object { $_.Cpus[0] })   # first logical CPU of each physical core

# Physical cores of each CCD, as logical CPU numbers the harnesses can pin to.
$ccds = @()
foreach ($g in $l3) {
    $ccds += , @($primary | Where-Object { $g.Cpus -contains $_ })
}

function Pick($list, [int]$index) { $list[[Math]::Min($index, $list.Count - 1)] }

$pairs = @()
if ($ccds[0].Count -ge 2) {
    $pairs += [pscustomobject]@{ Name = "same CCD0"; Producer = (Pick $ccds[0] 2); Consumer = (Pick $ccds[0] 3) }
}
if ($ccds.Count -ge 2) {
    if ($ccds[1].Count -ge 2) {
        $pairs += [pscustomobject]@{ Name = "same CCD1"; Producer = (Pick $ccds[1] 2); Consumer = (Pick $ccds[1] 3) }
    }
    $pairs += [pscustomobject]@{ Name = "cross CCD"; Producer = (Pick $ccds[0] 2); Consumer = (Pick $ccds[1] 2) }
}
if ($pairs.Count -eq 0) { throw "Found no CCD with two physical cores." }

$outDir = Join-Path $root "build\bench-results"
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$log = Join-Path $outDir ($stamp + ".log")
$jsonDir = Join-Path $outDir $stamp   # one JSON file per harness run, for bench\compare.py
New-Item -ItemType Directory -Force -Path $jsonDir | Out-Null

function Log([string]$line) {
    Write-Host $line
    Add-Content -Path $log -Value $line -Encoding UTF8
}

# The results also go to $jsonDir\<name>-run<run>.json, labelled <name>.
function Run([string]$name, [string]$exe, [string[]]$arguments) {
    Log ""
    Log ("> " + $exe + " " + ($arguments -join " "))
    $json = @("--json=$(Join-Path $jsonDir "$name-run$run.json")", "--label=$name", "--commit=$commit")
    & (Join-Path $bin "$exe.exe") @arguments @json 2>&1 | ForEach-Object { Log ([string]$_) }
    if ($LASTEXITCODE -ne 0) { Log "!! $exe exited with code $LASTEXITCODE" }
}

$commit = (git -C $root rev-parse --short HEAD) 2>$null
if ($commit) {
    git -C $root diff --quiet HEAD 2>$null
    if ($LASTEXITCODE -ne 0) { $commit += "-dirty" }
}

Log "# MiniHFT CCD matrix, $(Get-Date -Format 'yyyy-MM-dd HH:mm'), commit $commit"
Log "cpu         $((Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim())"
Log "cores       $($cores.Count) physical, $(($cores | ForEach-Object { $_.Cpus.Count } | Measure-Object -Sum).Sum) logical, SMT $(if ($smt) { 'on' } else { 'off' })"
for ($i = 0; $i -lt $l3.Count; $i++) {
    Log ("CCD$i        L3 {0} MB, logical CPUs {1}, physical cores at {2}" -f ($l3[$i].CacheBytes / 1MB), ($l3[$i].Cpus -join ","), ($ccds[$i] -join ","))
}
Log "power plan  $((powercfg /getactivescheme) -replace '^.*\((.*)\).*$', '$1')"
foreach ($p in $pairs) { Log ("pair        {0,-10} producer {1} -> consumer {2}" -f $p.Name, $p.Producer, $p.Consumer) }
if (($SaveBaseline -or $Check) -and $Runs -lt 3) {
    Log "NOTE: -Runs $Runs. compare.py trusts a change only if the runs of the two sides don't overlap; with 2 runs a side, noise alone separates them 1 time in 6. Use -Runs 3 or more for baselines and checks."
}

for ($run = 1; $run -le $Runs; $run++) {
    foreach ($p in $pairs) {
        Log ""
        Log "## run $run, $($p.Name): producer $($p.Producer) -> consumer $($p.Consumer)"
        $pc = @("--producer=$($p.Producer)", "--consumer=$($p.Consumer)", "--high-priority")
        $pair = $p.Name.ToLower().Replace(" ", "-")   # "same CCD0" -> same-ccd0
        Run "$pair-ring_latency" "ring_latency" $pc
        Run "$pair-ring_study" "ring_study" $pc
        Run "$pair-ring_study-pwork20" "ring_study" ($pc + @("--producer-work-ns=20", "--burst=5000000", "--paced=200000"))
        Run "$pair-ring_study-cwork20" "ring_study" ($pc + @("--consumer-work-ns=20", "--burst=5000000", "--paced=200000"))
    }
    # Single-threaded: only the CCD (and its L3 size) can matter.
    for ($i = 0; $i -lt $ccds.Count; $i++) {
        Log ""
        Log "## run $run, orderbook_latency on CCD$i"
        Run "ccd$i-orderbook_latency" "orderbook_latency" @("--core=$(Pick $ccds[$i] 2)")
    }
}

if ($ItchFile -ne "") {
    $run = 1   # the replay runs once: its JSON is ccd<i>-itch_replay-run1.json
    for ($i = 0; $i -lt $ccds.Count; $i++) {
        Log ""
        Log "## itch_replay on CCD$i"
        Run "ccd$i-itch_replay" "itch_replay" @($ItchFile, "--core=$(Pick $ccds[$i] 2)")
    }
}

Log ""
Log "Results written to $log, JSON in $jsonDir"

if ($SaveBaseline) {
    if (-not (Test-Path (Join-Path $jsonDir "*.json"))) {
        Log "!! no JSON results to save as the baseline"
        exit 1
    }
    if (Test-Path $baselineDir) { Remove-Item -Recurse -Force $baselineDir }
    New-Item -ItemType Directory -Force -Path $baselineDir | Out-Null
    Copy-Item -Path (Join-Path $jsonDir "*.json") -Destination $baselineDir
    Log "Saved as the baseline in $baselineDir (commit $commit). Check a later build with -Check."
}

if ($Check) {
    Log ""
    Log "## compared with the baseline in $baselineDir"
    & $python (Join-Path $root "bench\compare.py") $baselineDir --current $jsonDir @thresholds @gates 2>&1 |
        ForEach-Object { Log ([string]$_) }
    exit $LASTEXITCODE
}

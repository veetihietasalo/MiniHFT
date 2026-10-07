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
    build\bench-results\<timestamp>.log.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File bench\run_ccd_matrix.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File bench\run_ccd_matrix.ps1 -NoBuild -Runs 3 -ItchFile D:\itch\12302019.NASDAQ_ITCH50
#>
param(
    [int]$Runs = 2,             # repetitions of the whole matrix
    [string]$ItchFile = "",     # uncompressed ITCH 5.0 day: also replay it once on each CCD
    [switch]$NoBuild            # skip the cmake configure and build
)

$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\msvc-release\Release"

if ([IntPtr]::Size -ne 8) { throw "Run this from 64-bit PowerShell." }

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
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$log = Join-Path $outDir ((Get-Date -Format "yyyyMMdd-HHmmss") + ".log")

function Log([string]$line) {
    Write-Host $line
    Add-Content -Path $log -Value $line -Encoding UTF8
}

function Run([string]$exe, [string[]]$arguments) {
    Log ""
    Log ("> " + $exe + " " + ($arguments -join " "))
    & (Join-Path $bin "$exe.exe") @arguments 2>&1 | ForEach-Object { Log ([string]$_) }
    if ($LASTEXITCODE -ne 0) { Log "!! $exe exited with code $LASTEXITCODE" }
}

$commit = (git -C $root rev-parse --short HEAD) 2>$null

Log "# MiniHFT CCD matrix, $(Get-Date -Format 'yyyy-MM-dd HH:mm'), commit $commit"
Log "cpu         $((Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim())"
Log "cores       $($cores.Count) physical, $(($cores | ForEach-Object { $_.Cpus.Count } | Measure-Object -Sum).Sum) logical, SMT $(if ($smt) { 'on' } else { 'off' })"
for ($i = 0; $i -lt $l3.Count; $i++) {
    Log ("CCD$i        L3 {0} MB, logical CPUs {1}, physical cores at {2}" -f ($l3[$i].CacheBytes / 1MB), ($l3[$i].Cpus -join ","), ($ccds[$i] -join ","))
}
Log "power plan  $((powercfg /getactivescheme) -replace '^.*\((.*)\).*$', '$1')"
foreach ($p in $pairs) { Log ("pair        {0,-10} producer {1} -> consumer {2}" -f $p.Name, $p.Producer, $p.Consumer) }

for ($run = 1; $run -le $Runs; $run++) {
    foreach ($p in $pairs) {
        Log ""
        Log "## run $run, $($p.Name): producer $($p.Producer) -> consumer $($p.Consumer)"
        $pc = @("--producer=$($p.Producer)", "--consumer=$($p.Consumer)", "--high-priority")
        Run "ring_latency" $pc
        Run "ring_study" $pc
        Run "ring_study" ($pc + @("--producer-work-ns=20", "--burst=5000000", "--paced=200000"))
        Run "ring_study" ($pc + @("--consumer-work-ns=20", "--burst=5000000", "--paced=200000"))
    }
    # Single-threaded: only the CCD (and its L3 size) can matter.
    for ($i = 0; $i -lt $ccds.Count; $i++) {
        Log ""
        Log "## run $run, orderbook_latency on CCD$i"
        Run "orderbook_latency" @("--core=$(Pick $ccds[$i] 2)")
    }
}

if ($ItchFile -ne "") {
    for ($i = 0; $i -lt $ccds.Count; $i++) {
        Log ""
        Log "## itch_replay on CCD$i"
        Run "itch_replay" @($ItchFile, "--core=$(Pick $ccds[$i] 2)")
    }
}

Log ""
Log "Results written to $log"

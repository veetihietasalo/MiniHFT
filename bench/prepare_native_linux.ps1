<#
.SYNOPSIS
    Prepares a Windows machine for a native Ubuntu Server install next to Windows
    (docs/native_linux.md).

.DESCRIPTION
    Does the Windows side, in this order:
      1. Checks: UEFI firmware, Secure Boot, BitLocker on C:, and the disks with their free space.
      2. BitLocker: saves C:'s recovery password to the output folder. Installing a second boot
         loader can make Windows ask for it at the next boot. With -SuspendBitLocker, protection
         is also suspended for the next 3 reboots, so it doesn't ask at all.
      3. Turns off Fast Startup. With it on, Windows hibernates instead of shutting down, and its
         partitions stay locked while Linux runs.
      4. -ShrinkGB N: shrinks C: by N GB, leaving unallocated space for the installer. Skip this
         if Ubuntu gets a drive of its own.
      5. Downloads the newest Ubuntu Server ISO of -Release and checks it against Ubuntu's
         SHA256SUMS. A file that's already there and matches is not downloaded again.
      6. Installs Rufus with winget if it's missing and opens it, to write the ISO to a USB stick.
    It never writes to a disk except for -ShrinkGB, and asks before that. -WhatIf shows the
    changes without making them. Run it from an elevated (administrator) PowerShell.

    24.04 is the default because it matches CI (GCC 13, Clang 18). 26.04 has newer compilers,
    whose new warnings can fail the -Werror build.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File bench\prepare_native_linux.ps1 -WhatIf

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File bench\prepare_native_linux.ps1 -SuspendBitLocker

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File bench\prepare_native_linux.ps1 -ShrinkGB 200 -Release 26.04
#>
#Requires -RunAsAdministrator
[CmdletBinding(SupportsShouldProcess)]
param(
    [ValidateSet("24.04", "26.04")]
    [string]$Release = "24.04",
    [string]$OutDir = (Join-Path $env:USERPROFILE "Downloads\ubuntu-native"),
    [ValidateRange(0, 4096)]
    [int]$ShrinkGB = 0,              # shrink C: by this much for Ubuntu; 0 = leave the disks alone
    [switch]$SuspendBitLocker,       # suspend C:'s BitLocker for 3 reboots
    [switch]$SkipDownload,
    [switch]$SkipRufus
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue" # Invoke-WebRequest is many times slower with its progress bar
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$warnings = [System.Collections.Generic.List[string]]::new()
function Section([string]$title) { Write-Host "`n== $title" -ForegroundColor Cyan }

# --- 1. Checks ------------------------------------------------------------------------------

Section "Firmware"
if ($env:firmware_type -ne "UEFI") {
    throw "Firmware mode is '$env:firmware_type'. Ubuntu next to Windows needs UEFI; legacy BIOS installs are not covered here."
}
try {
    $secureBoot = Confirm-SecureBootUEFI
    Write-Host "UEFI, Secure Boot $(if ($secureBoot) { 'on (fine: Ubuntu boots through a signed shim)' } else { 'off' })"
} catch {
    Write-Host "UEFI, Secure Boot state unknown"
}

Section "BitLocker on C:"
# The CIM class works on Home editions too (device encryption) and doesn't depend on the display language.
$volume = Get-CimInstance -Namespace "root\CIMV2\Security\MicrosoftVolumeEncryption" -ClassName Win32_EncryptableVolume `
    -Filter "DriveLetter = 'C:'" -ErrorAction SilentlyContinue
$bitLockerOn = $volume -and $volume.ProtectionStatus -eq 1
if (-not $bitLockerOn) {
    Write-Host "Not protecting C:."
} else {
    $keyFile = Join-Path $OutDir "bitlocker-recovery-C.txt"
    # Home editions may lack the BitLocker module; manage-bde is on every edition.
    $haveModule = [bool](Get-Command Get-BitLockerVolume -ErrorAction SilentlyContinue)
    if ($haveModule) {
        $passwords = (Get-BitLockerVolume -MountPoint "C:").KeyProtector | Where-Object KeyProtectorType -eq "RecoveryPassword"
        $passwords | ForEach-Object { "Key ID $($_.KeyProtectorId)`r`nRecovery password $($_.RecoveryPassword)`r`n" } |
            Set-Content -Path $keyFile -Encoding ASCII
    } else {
        $passwords = manage-bde -protectors -get C: -Type RecoveryPassword
        $passwords | Set-Content -Path $keyFile -Encoding UTF8
        if ($LASTEXITCODE -ne 0) { $passwords = $null }
    }
    if (-not $passwords) {
        $warnings.Add("BitLocker is on but no recovery password was found for C:. Add one before installing: manage-bde -protectors -add C: -RecoveryPassword")
    } else {
        Write-Host "On. Recovery password saved to $keyFile"
        $warnings.Add("Copy $keyFile off this machine (phone, password manager) before you install. It's also at https://account.microsoft.com/devices/recoverykey if the PC is linked to a Microsoft account.")
    }
    if ($SuspendBitLocker -and $PSCmdlet.ShouldProcess("C:", "Suspend BitLocker for 3 reboots")) {
        if ($haveModule) { Suspend-BitLocker -MountPoint "C:" -RebootCount 3 | Out-Null }
        else { manage-bde -protectors -disable C: -RebootCount 3 | Out-Null }
        Write-Host "Suspended for the next 3 reboots; it resumes by itself after that."
    }
}

Section "Disks"
$disks = Get-Disk | Sort-Object Number
$disks | Format-Table -AutoSize Number, FriendlyName, BusType, PartitionStyle,
    @{ n = "SizeGB"; e = { [math]::Round($_.Size / 1GB) } },
    @{ n = "FreeGB"; e = { [math]::Round($_.LargestFreeExtent / 1GB) } },
    @{ n = "Windows"; e = { if ($_.IsBoot -or $_.IsSystem) { "yes" } else { "" } } } | Out-Host
$spare = $disks | Where-Object { -not $_.IsBoot -and -not $_.IsSystem -and $_.BusType -ne "USB" }
if ($spare) {
    Write-Host "Disks other than the Windows one: $(($spare | ForEach-Object { "#$($_.Number) $($_.FriendlyName)" }) -join ', ')."
    Write-Host "A drive of its own is the safest place for Ubuntu: pick it in the installer and leave -ShrinkGB at 0."
}

# --- 2. Fast Startup ------------------------------------------------------------------------

Section "Fast Startup"
$powerKey = "HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power"
if ((Get-ItemProperty -Path $powerKey -Name HiberbootEnabled -ErrorAction SilentlyContinue).HiberbootEnabled -eq 0) {
    Write-Host "Already off."
} elseif ($PSCmdlet.ShouldProcess("Fast Startup", "Turn off")) {
    Set-ItemProperty -Path $powerKey -Name HiberbootEnabled -Value 0 -Type DWord
    Write-Host "Turned off."
}

# --- 3. Space for Ubuntu --------------------------------------------------------------------

if ($ShrinkGB -gt 0) {
    Section "Shrink C: by $ShrinkGB GB"
    $partition = Get-Partition -DriveLetter C
    $supported = Get-PartitionSupportedSize -DriveLetter C
    $newSize = $partition.Size - [int64]$ShrinkGB * 1GB
    $freeAfter = (Get-Volume -DriveLetter C).SizeRemaining - [int64]$ShrinkGB * 1GB
    if ($newSize -lt $supported.SizeMin) {
        throw "C: can shrink by at most $([math]::Floor(($partition.Size - $supported.SizeMin) / 1GB)) GB (files near the end of the volume stop it). Ask for less."
    }
    if ($freeAfter -lt 30GB) {
        throw "That would leave C: with $([math]::Round($freeAfter / 1GB)) GB free. Keep at least 30 GB for Windows updates."
    }
    $question = "Shrink C: from $([math]::Round($partition.Size / 1GB)) GB to $([math]::Round($newSize / 1GB)) GB?"
    if ($PSCmdlet.ShouldProcess("C:", "Resize to $([math]::Round($newSize / 1GB)) GB") -and
        $PSCmdlet.ShouldContinue($question, "Shrink C:")) {
        Resize-Partition -DriveLetter C -Size $newSize
        Write-Host "Done: $ShrinkGB GB unallocated after C:. In the installer, put Ubuntu in that free space."
    }
}

# --- 4. The ISO -----------------------------------------------------------------------------

$iso = $null
if (-not $SkipDownload) {
    Section "Ubuntu Server $Release"
    $base = "https://releases.ubuntu.com/$Release"
    # SHA256SUMS lists every point release still on the mirror; take the newest server ISO.
    # Saved to a file: the server sends no text content type, so .Content would be raw bytes.
    $sumsFile = Join-Path $OutDir "SHA256SUMS-$Release"
    Invoke-WebRequest -UseBasicParsing -Uri "$base/SHA256SUMS" -OutFile $sumsFile
    $newest = Get-Content $sumsFile | ForEach-Object {
        if ($_ -match '^([0-9a-f]{64}) \*?(ubuntu-([0-9.]+)-live-server-amd64\.iso)\s*$') {
            [pscustomobject]@{ Hash = $Matches[1]; Name = $Matches[2]; Version = [version]$Matches[3] }
        }
    } | Sort-Object Version | Select-Object -Last 1
    if (-not $newest) { throw "No live-server-amd64 ISO in $base/SHA256SUMS." }

    $iso = Join-Path $OutDir $newest.Name
    $haveIt = (Test-Path $iso) -and ((Get-FileHash -Algorithm SHA256 $iso).Hash -eq $newest.Hash)
    if ($haveIt) {
        Write-Host "$($newest.Name) is already in $OutDir and matches its checksum."
    } elseif ($PSCmdlet.ShouldProcess($iso, "Download $base/$($newest.Name)")) {
        Write-Host "Downloading $($newest.Name) (about 3 GB)..."
        try {
            Start-BitsTransfer -Source "$base/$($newest.Name)" -Destination $iso # resumable, shows progress
        } catch {
            Invoke-WebRequest -UseBasicParsing -Uri "$base/$($newest.Name)" -OutFile $iso
        }
        if ((Get-FileHash -Algorithm SHA256 $iso).Hash -ne $newest.Hash) {
            Remove-Item $iso
            throw "Checksum mismatch: the download was corrupt and has been deleted. Run the script again."
        }
        Write-Host "Saved to $iso, checksum OK."
    }
}

# --- 5. Rufus -------------------------------------------------------------------------------

if (-not $SkipRufus) {
    Section "USB stick"
    $rufus = Get-Command rufus*.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $rufus -and (Get-Command winget -ErrorAction SilentlyContinue) -and
        $PSCmdlet.ShouldProcess("Rufus", "Install with winget")) {
        winget install --id Rufus.Rufus --exact --accept-package-agreements --accept-source-agreements
        $rufus = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Links\rufus*.exe", "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Rufus*\rufus*.exe" `
            -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    if ($rufus -and -not $WhatIfPreference) {
        Start-Process -FilePath $(if ($rufus.Source) { $rufus.Source } else { $rufus.FullName })
        Write-Host "Rufus is open. Device: your USB stick (8 GB or more; it gets erased). Boot selection: $(if ($iso) { $iso } else { 'the Ubuntu ISO' })"
        Write-Host "Partition scheme GPT, target UEFI. If it asks, 'Write in ISO Image mode' is the default; use DD mode if the stick won't boot."
    } elseif (-not $rufus) {
        Write-Host "Get Rufus from https://rufus.ie and write $(if ($iso) { $iso } else { 'the Ubuntu ISO' }) to a USB stick with it."
    }
}

# --- Next steps -----------------------------------------------------------------------------

Section "Next"
Write-Host @"
1. Shut down (with Fast Startup off, this really shuts down), plug in the stick, and open your
   board's boot menu (often F8, F11 or F12). Pick the UEFI entry of the stick.
2. Installer: Ubuntu Server (not minimized). Enable "Install OpenSSH server".
   Storage, on a drive of its own: "Use an entire disk" on THAT drive. Check the disk name twice.
   Storage, next to Windows on one disk: "Custom storage layout", put an ext4 partition mounted at /
   in the free space, and keep the existing EFI partition as it is (don't format it).
3. Windows stays in your board's boot menu. To list it in GRUB as well: set
   GRUB_DISABLE_OS_PROBER=false in /etc/default/grub, then sudo update-grub.
4. Clock: Windows keeps the hardware clock in local time, Ubuntu in UTC, so one of them shows the
   wrong hour after switching. In Ubuntu: sudo timedatectl set-local-rtc 1
5. Then follow docs/native_linux.md from "Packages".
"@
if ($warnings.Count -gt 0) {
    Section "Before you reboot"
    $warnings | ForEach-Object { Write-Host "- $_" -ForegroundColor Yellow }
}

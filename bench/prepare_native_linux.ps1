<#
.SYNOPSIS
    Prepares a Windows machine for a native Ubuntu Server install next to Windows
    (docs/native_linux.md).

.DESCRIPTION
    Does the Windows side, in this order:
      1. Checks: UEFI firmware, Secure Boot, BitLocker on C:, and every disk with its partitions.
         Stops if C: is encrypted with key protectors but no recovery password.
      2. BitLocker: if C: is encrypted, saves its recovery password to the output folder. Installing a second boot
         loader can make Windows ask for it at the next boot. With -SuspendBitLocker, protection
         is also suspended for the next 3 reboots, which covers the install. It doesn't make it
         safe to boot Windows through GRUB later: start Windows from the firmware boot menu.
      3. Turns off Fast Startup. With it on, Windows hibernates instead of shutting down, and its
         partitions stay locked while Linux runs.
      4. -ShrinkGB N: shrinks C: by N GB, leaving unallocated space for the installer, unless the disk
         already has that much unallocated (a rerun). Skip this if Ubuntu gets a drive of its own.
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
# Older Windows PowerShell 5.1 builds offer only SSL 3 and TLS 1.0, and releases.ubuntu.com accepts TLS 1.2 and 1.3.
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
# -WhatIf:$false: the folder and the files saved in it (recovery password, checksums) change nothing
# on the system, and a dry run needs them too.
New-Item -ItemType Directory -Force -Path $OutDir -WhatIf:$false | Out-Null
$warnings = [System.Collections.Generic.List[string]]::new()
function Section([string]$title) { Write-Host "`n== $title" -ForegroundColor Cyan }

# --- 1. Checks ------------------------------------------------------------------------------

Section "Firmware"
# PEFirmwareType: 1 = legacy BIOS, 2 = UEFI. ($env:firmware_type doesn't work here: %FIRMWARE_TYPE% is
# computed by cmd.exe when it expands it, and isn't in the environment PowerShell reads.)
$firmware = (Get-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control" -Name PEFirmwareType -ErrorAction SilentlyContinue).PEFirmwareType
if ($firmware -eq 1) {
    throw "Windows booted in legacy BIOS mode. Ubuntu next to this Windows would need legacy mode too, which these steps don't cover. Don't change firmware settings to fix this: a BIOS-mode Windows won't boot in UEFI mode."
} elseif ($firmware -ne 2) {
    Write-Warning "Couldn't read the firmware type; carrying on as if it's UEFI."
}
try {
    $secureBoot = Confirm-SecureBootUEFI
    Write-Host "UEFI, Secure Boot $(if ($secureBoot) { 'on (fine: Ubuntu boots through a signed shim)' } else { 'off' })"
} catch {
    Write-Host "UEFI, Secure Boot state unknown"
}

Section "BitLocker on C:"
# The CIM class works on Home editions too (device encryption) and doesn't depend on the display language.
# Decide on encryption, not protection: a suspended volume, or one waiting for activation, reports
# ProtectionStatus 0 but is still encrypted.
$volume = Get-CimInstance -Namespace "root\CIMV2\Security\MicrosoftVolumeEncryption" -ClassName Win32_EncryptableVolume `
    -Filter "DriveLetter = 'C:'" -ErrorAction SilentlyContinue
$encrypted = $false
if ($volume) {
    $conversion = Invoke-CimMethod -InputObject $volume -MethodName GetConversionStatus -ErrorAction SilentlyContinue
    $encrypted = ($conversion -and $conversion.ConversionStatus -ne 0) -or $volume.ProtectionStatus -eq 1
}
if (-not $volume) {
    Write-Host "Couldn't read the BitLocker state (this edition may not have BitLocker)."
} elseif (-not $encrypted) {
    Write-Host "C: is not encrypted."
} else {
    # No key protectors at all: device encryption waiting for activation, which a local account leaves in
    # place. The key is stored in the clear, so nothing can ask for a recovery password. -1 = couldn't tell.
    $protectorList = Invoke-CimMethod -InputObject $volume -MethodName GetKeyProtectors -ErrorAction SilentlyContinue
    $protectorCount = if ($protectorList -and $protectorList.ReturnValue -eq 0) { @($protectorList.VolumeKeyProtectorID | Where-Object { $_ }).Count } else { -1 }
    $clearKey = $protectorCount -eq 0 -and $volume.ProtectionStatus -ne 1
    Write-Host "C: is encrypted, protection $(if ($volume.ProtectionStatus -eq 1) { 'on' } elseif ($clearKey) { 'off: waiting for activation, with no key protectors' } else { 'suspended' })."
}
if ($encrypted -and $clearKey) {
    Write-Host "There's no recovery password to save yet. Signing in with a Microsoft account later adds one, and from then on BitLocker can ask for it."
} elseif ($encrypted) {
    $keyFile = Join-Path $OutDir "bitlocker-recovery-C.txt"
    # Home editions may lack the BitLocker module; manage-bde is on every edition.
    $haveModule = [bool](Get-Command Get-BitLockerVolume -ErrorAction SilentlyContinue)
    if ($haveModule) {
        $passwords = (Get-BitLockerVolume -MountPoint "C:").KeyProtector | Where-Object KeyProtectorType -eq "RecoveryPassword" |
            ForEach-Object { "Key ID $($_.KeyProtectorId)`r`nRecovery password $($_.RecoveryPassword)`r`n" }
    } else {
        $passwords = manage-bde -protectors -get C: -Type RecoveryPassword
        if ($LASTEXITCODE -ne 0) { $passwords = $null } # no recovery password, or manage-bde failed
    }
    if (-not $passwords) {
        throw "C: is encrypted but has no recovery password, so a boot-loader change could lock you out of Windows. Add one, note it down, then run this again: manage-bde -protectors -add C: -RecoveryPassword"
    }
    $passwords | Set-Content -Path $keyFile -Encoding UTF8 -WhatIf:$false
    Write-Host "Recovery password saved to $keyFile"
    $copyOff = "Copy $keyFile off this machine (phone, password manager) before you install, then delete it here: when Windows asks for it, C: is locked, and on this PC it only helps whoever copies the disk. It's also at https://account.microsoft.com/devices/recoverykey if the PC is linked to a Microsoft account."
    Write-Warning $copyOff
    $warnings.Add($copyOff)
    if ($SuspendBitLocker -and $PSCmdlet.ShouldProcess("C:", "Suspend BitLocker for 3 reboots")) {
        if ($haveModule) {
            Suspend-BitLocker -MountPoint "C:" -RebootCount 3 | Out-Null
        } else {
            manage-bde -protectors -disable C: -RebootCount 3 | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "manage-bde could not suspend BitLocker (exit code $LASTEXITCODE)." }
        }
        Write-Host "Suspended for the next 3 reboots; it resumes by itself after that."
    }
}

Section "Disks"
$disks = Get-Disk | Sort-Object Number
# The columns that say which disk holds Windows come first: a table wider than the console loses its last
# columns. No serial number: for NVMe drives Windows shows an EUI-64 here, not the serial the installer shows.
$disks | Format-Table -AutoSize Number,
    @{ n = "Windows"; e = { if ($_.IsBoot -or $_.IsSystem) { "yes" } else { "" } } },
    @{ n = "Letters"; e = { (Get-Partition -DiskNumber $_.Number -ErrorAction SilentlyContinue | Where-Object DriveLetter |
        ForEach-Object { "$($_.DriveLetter):" }) -join " " } },
    FriendlyName,
    @{ n = "SizeGB"; e = { [math]::Round($_.Size / 1GB) } },
    @{ n = "FreeGB"; e = { [math]::Round($_.LargestFreeExtent / 1GB) } },
    @{ n = "Partitions"; e = { $_.NumberOfPartitions } },
    BusType, PartitionStyle | Out-Host
$others = $disks | Where-Object { -not $_.IsBoot -and -not $_.IsSystem -and $_.BusType -ne "USB" }
# Disk Management gives a new GPT disk a 16 MB Microsoft Reserved partition, so that one doesn't count.
function Get-DataPartitionCount($disk) {
    @(Get-Partition -DiskNumber $disk.Number -ErrorAction SilentlyContinue | Where-Object Type -ne "Reserved").Count
}
$empty = $others | Where-Object { $_.PartitionStyle -ne "RAW" -and (Get-DataPartitionCount $_) -eq 0 }
if ($empty) {
    Write-Host "Empty disks: $(($empty | ForEach-Object { "#$($_.Number) $($_.FriendlyName), $([math]::Round($_.Size / 1GB)) GB" }) -join '; ')."
    Write-Host "An empty drive of its own is the safest place for Ubuntu: pick it in the installer by its size and model, and leave -ShrinkGB at 0."
}
# RAW only means no partition table Windows recognizes. A disk used whole by Linux (LUKS, LVM, ext4) looks the same.
$raw = $others | Where-Object { $_.PartitionStyle -eq "RAW" }
if ($raw) {
    Write-Host "Disks with no partition table Windows recognizes: $(($raw | ForEach-Object { "#$($_.Number) $($_.FriendlyName), $([math]::Round($_.Size / 1GB)) GB" }) -join '; '). Either new, or used whole by another OS: make sure nothing on it is needed before installing there."
}
$withData = $others | Where-Object { $_.PartitionStyle -ne "RAW" -and (Get-DataPartitionCount $_) -gt 0 }
if ($withData) {
    Write-Host "Other disks with partitions on them: $(($withData | ForEach-Object { "#$($_.Number) $($_.FriendlyName)" }) -join '; '). Installing Ubuntu on one of these with 'Use an entire disk' erases it."
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
    # A rerun with the same -ShrinkGB (after a failed download, say) must not take the space a second time.
    $alreadyFree = (Get-Disk -Number $partition.DiskNumber).LargestFreeExtent
    if ($alreadyFree -ge [int64]$ShrinkGB * 1GB) {
        Write-Host "The Windows disk already has $([math]::Round($alreadyFree / 1GB)) GB unallocated, so C: stays as it is. In the installer, put Ubuntu in that free space."
    } else {
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
        Write-Host "Downloading $($newest.Name) (3-4 GB)..."
        try {
            $ProgressPreference = "Continue" # BITS reports progress through the progress bar
            Start-BitsTransfer -Source "$base/$($newest.Name)" -Destination $iso # resumable
        } catch {
            $ProgressPreference = "SilentlyContinue"
            Invoke-WebRequest -UseBasicParsing -Uri "$base/$($newest.Name)" -OutFile $iso
        } finally {
            $ProgressPreference = "SilentlyContinue"
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
   Storage, on a drive of its own: "Use an entire disk" on THAT drive; it erases everything on it.
   Pick it by size and model, which the table above shows for every disk. Before you confirm, the
   installer lists the devices it will format: the Windows drive must not be among them.
   Storage, next to Windows on one disk: the installer shows the free space only with a boot flag.
   At the stick's GRUB menu, highlight "Try or Install Ubuntu Server", press e, and on the
   "linux /casper/vmlinuz" line type subiquity-storage-version=2 just before "---" (Ctrl+X boots).
   Then "Custom storage layout", the disk's "free space" row, "Add GPT Partition": ext4, mounted
   at /. The existing EFI partition becomes /boot/efi by itself. Don't edit it or any Windows
   partition, and don't reformat the disk.
$(if ($encrypted) {
"3. Start Windows from your board's boot menu, not from GRUB. With BitLocker on, booting Windows
   through GRUB changes what the TPM measures, and Windows asks for the recovery password."
} else {
"3. Windows stays in your board's boot menu. To list it in GRUB as well: sudo apt install os-prober,
   set GRUB_DISABLE_OS_PROBER=false in /etc/default/grub, then sudo update-grub."
})
4. Clock: Windows keeps the hardware clock in local time and Ubuntu keeps it in UTC, so one of them
   shows the wrong hour after a switch. Keep Ubuntu on UTC and tell Windows the clock is UTC, in an
   administrator prompt: reg add HKLM\SYSTEM\CurrentControlSet\Control\TimeZoneInformation /v RealTimeIsUniversal /t REG_DWORD /d 1 /f
5. Then follow docs/native_linux.md from "Packages".
"@
if ($warnings.Count -gt 0) {
    Section "Before you reboot"
    $warnings | ForEach-Object { Write-Host "- $_" -ForegroundColor Yellow }
}

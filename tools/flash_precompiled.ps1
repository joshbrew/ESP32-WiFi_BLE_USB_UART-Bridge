#requires -Version 5.1
<#
.SYNOPSIS
Flash an existing Arduino ESP32 build without compiling it.
.EXAMPLE
.\tools\flash_precompiled.ps1 -Variant s3-dispenser -Port COM24
.EXAMPLE
.\tools\flash_precompiled.ps1 -Variant s3-dispenser -BuildDirectory 'C:\firmware\s3' -DryRun
#>
[CmdletBinding()]
param(
    [ValidateSet('s3-dispenser','s3-transport','lolin32-dispenser','lolin32-transport')]
    [string]$Variant,
    [string]$BuildDirectory,
    [string]$Port,
    [ValidateRange(9600,2000000)][int]$Baud = 921600,
    [string]$EsptoolPath,
    [switch]$SkipUnchanged,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'firmware_hash.ps1')
. (Join-Path $PSScriptRoot 'serial_ports.ps1')
$script:ProjectRoot = Split-Path -Parent $PSScriptRoot
$script:Variants = [ordered]@{
    's3-dispenser' = @{ Sketch='ESP32S3_Dispenser_Controller'; Chip='esp32s3'; ChipId=9; BootOffset=0 }
    's3-transport' = @{ Sketch='ESP32S3_Modular_Command_Transport_Controller'; Chip='esp32s3'; ChipId=9; BootOffset=0 }
    'lolin32-dispenser' = @{ Sketch='ESP32_Dispenser_Controller'; Chip='esp32'; ChipId=0; BootOffset=0x1000 }
    'lolin32-transport' = @{ Sketch='ESP32_Modular_Command_Transport_Controller'; Chip='esp32'; ChipId=0; BootOffset=0x1000 }
}

function Read-ImageHeader([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $bytes = New-Object byte[] 24
        if ($stream.Read($bytes,0,24) -ne 24 -or $bytes[0] -ne 0xE9 -or $bytes[1] -lt 1 -or $bytes[1] -gt 16) { throw "Not a complete ESP firmware image: $Path" }
        $sizeCode = $bytes[3] -shr 4
        if ($sizeCode -gt 7) { throw "Unrecognized image flash size: $Path" }
        return [pscustomobject]@{ ChipId=[BitConverter]::ToUInt16($bytes,12); FlashBytes=([long]1MB -shl $sizeCode) }
    } finally { $stream.Dispose() }
}

function Read-PartitionTable([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -gt 4096 -or $bytes.Length -lt 32) { throw 'Partition table must fit its 4 KiB flash sector.' }
    $entries = @()
    for ($i=0; $i+32 -le $bytes.Length; $i+=32) {
        $magic = [BitConverter]::ToUInt16($bytes,$i)
        if ($magic -eq 0xFFFF -or $magic -eq 0xEBEB) { break }
        if ($magic -ne 0x50AA) { throw 'Invalid ESP partition table entry.' }
        $entries += [pscustomobject]@{
            Type=[int]$bytes[$i+2]; Subtype=[int]$bytes[$i+3]
            Offset=[long][BitConverter]::ToUInt32($bytes,$i+4)
            Size=[long][BitConverter]::ToUInt32($bytes,$i+8)
            Label=[Text.Encoding]::ASCII.GetString($bytes,$i+12,16).TrimEnd([char]0)
        }
    }
    if ($entries.Count -eq 0) { throw 'No partitions found.' }
    return $entries
}

function Find-Esptool([string]$ExplicitPath) {
    if ($ExplicitPath) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) { throw "esptool executable not found: $ExplicitPath" }
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }
    $root = Join-Path $env:LOCALAPPDATA 'Arduino15\packages\esp32\tools\esptool_py'
    if (Test-Path -LiteralPath $root) {
        $versions = @(Get-ChildItem -LiteralPath $root -Directory | Sort-Object { try { [version]$_.Name } catch { [version]'0.0' } } -Descending)
        foreach ($version in $versions) {
            $tool = Join-Path $version.FullName 'esptool.exe'
            if (Test-Path -LiteralPath $tool -PathType Leaf) { return $tool }
        }
    }
    $command = Get-Command esptool.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    throw 'Install the esp32 package in Arduino Boards Manager, or supply -EsptoolPath to esptool.exe.'
}

function Find-BootApp([string]$Directory,$Metadata) {
    $local = Join-Path $Directory 'boot_app0.bin'
    if (Test-Path -LiteralPath $local -PathType Leaf) { return $local }
    $roots = @()
    if ($Metadata -and $Metadata.PSObject.Properties['hardwareFolders']) { $roots += @($Metadata.hardwareFolders -split ',') }
    foreach ($root in $roots) {
        $candidate = Join-Path $root 'tools\partitions\boot_app0.bin'
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    $installed = Join-Path $env:LOCALAPPDATA 'Arduino15\packages\esp32\hardware\esp32'
    if (Test-Path -LiteralPath $installed) {
        foreach ($version in @(Get-ChildItem -LiteralPath $installed -Directory | Sort-Object { try { [version]$_.Name } catch { [version]'0.0' } } -Descending)) {
            $candidate = Join-Path $version.FullName 'tools\partitions\boot_app0.bin'
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
        }
    }
    throw 'Missing boot_app0.bin. Keep it with the build, or install the matching Arduino ESP32 core.'
}

function Find-PrecompiledBuild([string]$VariantName,[string]$Root=$script:ProjectRoot,[string]$CacheRoot=(Join-Path $env:LOCALAPPDATA 'arduino\sketches')) {
    $definition = $script:Variants[$VariantName]
    $name = $definition.Sketch + '.ino.bin'
    # An explicitly saved precompiled folder takes precedence over temporary caches.
    $saved = Join-Path (Join-Path $Root 'precompiled') $VariantName
    if (Test-Path -LiteralPath (Join-Path $saved $name) -PathType Leaf) { return $saved }
    $candidates = @()
    $export = Join-Path (Join-Path $Root $definition.Sketch) 'build'
    if (Test-Path -LiteralPath $export) { $candidates += @(Get-ChildItem -LiteralPath $export -Filter $name -File -Recurse -Depth 3) }
    if (Test-Path -LiteralPath $CacheRoot) {
        foreach ($folder in @(Get-ChildItem -LiteralPath $CacheRoot -Directory)) {
            $app = Join-Path $folder.FullName $name
            if (Test-Path -LiteralPath $app -PathType Leaf) { $candidates += Get-Item -LiteralPath $app }
        }
    }
    $complete = @($candidates | Where-Object {
        (Test-Path -LiteralPath (Join-Path $_.DirectoryName ($definition.Sketch+'.ino.bootloader.bin'))) -and
        (Test-Path -LiteralPath (Join-Path $_.DirectoryName ($definition.Sketch+'.ino.partitions.bin')))
    } | Sort-Object LastWriteTimeUtc -Descending)
    if ($complete.Count -eq 0) { throw "No completed build found for $($definition.Sketch). Run controller compile -Variant $VariantName, export in Arduino IDE, or pass -BuildDirectory." }
    return $complete[0].DirectoryName
}

function Get-FlashPlan([string]$VariantName,[string]$Directory,[string]$Repository=$script:ProjectRoot) {
    $definition = $script:Variants[$VariantName]
    if (-not $definition) { throw 'Select a supported controller variant.' }
    $directoryPath = (Resolve-Path -LiteralPath $Directory).Path
    $prefix = $definition.Sketch + '.ino'
    $app = Join-Path $directoryPath ($prefix+'.bin')
    $boot = Join-Path $directoryPath ($prefix+'.bootloader.bin')
    $table = Join-Path $directoryPath ($prefix+'.partitions.bin')
    foreach ($file in @($app,$boot,$table)) { if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing matching binary: $file. Use the complete build/export folder, not just the application .bin." } }
    $appHeader = Read-ImageHeader $app
    $bootHeader = Read-ImageHeader $boot
    if ($appHeader.ChipId -ne $definition.ChipId -or $bootHeader.ChipId -ne $definition.ChipId) { throw "Binary chip mismatch: $VariantName requires $($definition.Chip)." }
    if ($definition.Chip -eq 'esp32s3' -and $bootHeader.FlashBytes -ne 16MB) { throw 'This S3 N16R8 variant needs a binary built for 16 MB flash.' }
    $metadata = $null
    $metadataPath = Join-Path $directoryPath 'build.options.json'
    if (Test-Path -LiteralPath $metadataPath -PathType Leaf) { $metadata = [IO.File]::ReadAllText($metadataPath) | ConvertFrom-Json }
    $fqbn = ''
    if ($metadata -and $metadata.PSObject.Properties['fqbn']) { $fqbn = [string]$metadata.fqbn }
    if ($definition.Chip -eq 'esp32s3' -and $fqbn) {
        if ($fqbn -notmatch '^esp32:esp32:esp32s3(?::|$)' -or $fqbn -notmatch '(?::|,)FlashSize=16M(?:,|$)' -or $fqbn -notmatch '(?::|,)PSRAM=opi(?:,|$)') {
            throw "This cached S3 build does not match N16R8 settings: $fqbn . Select ESP32S3 Dev Module, 16MB flash and OPI PSRAM, then export once. Or choose an existing correctly built folder with -BuildDirectory."
        }
    }
    $partitions = @(Read-PartitionTable $table)
    $apps = @($partitions | Where-Object { $_.Type -eq 0 -and $_.Subtype -eq 0x10 })
    if ($apps.Count -eq 0) { $apps = @($partitions | Where-Object { $_.Type -eq 0 -and $_.Subtype -eq 0 }) }
    if ($apps.Count -ne 1) { throw 'Expected one app0/ota_0 or factory partition.' }
    $application = $apps[0]
    if ((Get-Item -LiteralPath $app).Length -gt $application.Size) { throw 'Application image does not fit its partition.' }
    $segments = @(
        [pscustomobject]@{ Offset=[long]$definition.BootOffset; Path=$boot; Role='bootloader' },
        [pscustomobject]@{ Offset=[long]0x8000; Path=$table; Role='partition table' }
    )
    $ota = @($partitions | Where-Object { $_.Type -eq 1 -and $_.Subtype -eq 0 })
    if ($ota.Count -gt 1) { throw 'Ambiguous OTA metadata partitions.' }
    if ($ota.Count -eq 1) {
        $seed = Find-BootApp $directoryPath $metadata
        if ((Get-Item -LiteralPath $seed).Length -gt $ota[0].Size) { throw 'OTA initialization image does not fit its partition.' }
        if ($application.Subtype -ne 0x10) { throw 'OTA metadata is present but ota_0 is missing.' }
        $segments += [pscustomobject]@{ Offset=[long]$ota[0].Offset; Path=$seed; Role='OTA initialization' }
    }
    $segments += [pscustomobject]@{ Offset=[long]$application.Offset; Path=$app; Role='application' }
    $segments = @($segments | Sort-Object Offset)
    foreach ($partition in $partitions) {
        if ($partition.Offset -lt 0x9000 -or $partition.Size -le 0 -or $partition.Offset+$partition.Size -gt $bootHeader.FlashBytes) { throw "Partition $($partition.Label) is outside the compiled flash layout." }
    }
    $previousEnd = 0L
    foreach ($segment in $segments) {
        $length = (Get-Item -LiteralPath $segment.Path).Length
        $end = $segment.Offset + [long]([math]::Ceiling($length/4096.0)*4096)
        if ($segment.Offset % 4096 -ne 0 -or $length -le 0 -or $segment.Offset -lt $previousEnd -or $end -gt $bootHeader.FlashBytes) { throw 'Flash segments overlap or exceed the flash bounds.' }
        foreach ($partition in $partitions) {
            if ($segment.Offset -lt $partition.Offset+$partition.Size -and $end -gt $partition.Offset) {
                $owned = ($segment.Role -eq 'application' -and $partition -eq $application) -or ($segment.Role -eq 'OTA initialization' -and $partition.Type -eq 1 -and $partition.Subtype -eq 0)
                if (-not $owned) { throw "A flash write would overlap partition $($partition.Label)." }
            }
        }
        $previousEnd = $end
        $segment | Add-Member -NotePropertyName Length -NotePropertyValue $length
        $segment | Add-Member -NotePropertyName SHA256 -NotePropertyValue (Get-ControllerHash $segment.Path)
    }
    $binaryTime = (Get-Item -LiteralPath $app).LastWriteTimeUtc
    $source = Join-Path $Repository $definition.Sketch
    $sourceFiles = @()
    if (Test-Path -LiteralPath $source) {
        $sourceFiles += @(Get-ChildItem -LiteralPath $source -File | Where-Object { $_.Extension -in @('.ino','.h') })
        $src = Join-Path $source 'src'
        if (Test-Path -LiteralPath $src) { $sourceFiles += @(Get-ChildItem -LiteralPath $src -File -Recurse | Where-Object { $_.Extension -in @('.h','.cpp') }) }
    }
    $origin = ''
    if ($metadata -and $metadata.PSObject.Properties['sketchLocation']) { $origin = [string]$metadata.sketchLocation }
    return [pscustomobject]@{ Variant=$VariantName; Sketch=$definition.Sketch; Chip=$definition.Chip; Directory=$directoryPath; BuiltUtc=$binaryTime; Fqbn=$fqbn; Origin=$origin; SourceNewer=(@($sourceFiles | Where-Object { $_.LastWriteTimeUtc -gt $binaryTime }).Count -gt 0); Segments=$segments }
}

function Get-FlashArguments($Plan,[string]$SerialPort,[int]$Speed,[bool]$Modern,[bool]$Skip) {
    if ($SerialPort -notmatch '^COM[1-9][0-9]*$') { throw 'Use a Windows serial port such as COM24.' }
    if ($Modern) {
        $arguments = @('--chip',$Plan.Chip,'--port',$SerialPort,'--baud',[string]$Speed,'--before','default-reset','--after','hard-reset','write-flash','--compress','--flash-mode','keep','--flash-freq','keep','--flash-size','keep')
    } else {
        if ($Skip) { throw '-SkipUnchanged requires esptool 5.3 or newer.' }
        $arguments = @('--chip',$Plan.Chip,'--port',$SerialPort,'--baud',[string]$Speed,'--before','default_reset','--after','hard_reset','write_flash','--compress','--flash_mode','keep','--flash_freq','keep','--flash_size','keep')
    }
    if ($Skip) { $arguments += '--skip-flashed' }
    foreach ($segment in $Plan.Segments) { $arguments += @(('0x{0:x}' -f $segment.Offset),$segment.Path) }
    return $arguments
}

function Invoke-PrecompiledFlash {
    if (-not $Variant) {
        $names = @($script:Variants.Keys)
        Write-Host 'Flash an existing build - no compilation' -ForegroundColor Cyan
        for ($i=0; $i -lt $names.Count; $i++) { Write-Host ('  {0}. {1}' -f ($i+1),$names[$i]) }
        $choice = Read-Host 'Choose firmware (1-4)'
        if ($choice -notmatch '^[1-4]$') { throw 'Choose a number from 1 to 4.' }
        $Variant = $names[[int]$choice-1]
    }
    if (-not $BuildDirectory) { $BuildDirectory = Find-PrecompiledBuild $Variant }
    $plan = Get-FlashPlan $Variant $BuildDirectory
    $tool = Find-Esptool $EsptoolPath
    $toolVersion = $null
    $versionFolder = Split-Path -Leaf (Split-Path -Parent $tool)
    if (-not [version]::TryParse($versionFolder,[ref]$toolVersion)) {
        if ($DryRun) { $toolVersion = [version]'5.0' }
        else {
            $description = (& $tool version 2>&1 | Out-String)
            if ($LASTEXITCODE -ne 0 -or $description -notmatch 'esptool\s+v?(\d+\.\d+(?:\.\d+)?)') { throw 'Cannot identify esptool version.' }
            $toolVersion = [version]$Matches[1]
        }
    }
    if ($SkipUnchanged -and $toolVersion -lt [version]'5.3') { throw '-SkipUnchanged requires esptool 5.3 or newer.' }
    Write-Host "`nFirmware: $($plan.Sketch) [$($plan.Chip)]" -ForegroundColor Cyan
    Write-Host "Build folder: $($plan.Directory)"
    Write-Host ('Binary built (UTC): {0:u}' -f $plan.BuiltUtc)
    if ($plan.Origin) { Write-Host "Compiled from: $($plan.Origin)" }
    if ($plan.Fqbn) { Write-Host "Saved board settings: $($plan.Fqbn)" }
    else { Write-Host 'Saved board settings unavailable: use an export made with the README settings.' -ForegroundColor Yellow }
    if ($plan.SourceNewer) { Write-Host 'Source files are newer than this binary. Later source edits are not included.' -ForegroundColor Yellow }
    foreach ($segment in $plan.Segments) { Write-Host ('  0x{0:x8}  {1,8} bytes  {2}' -f $segment.Offset,$segment.Length,$segment.Role) }
    $selection=$null
    if ($DryRun) { if(-not $Port){$Port='COM1'} }
    else {
        $selection=Select-ControllerPort $Port
        if(-not $selection){Write-Host 'Cancelled. No flash writes.';return}
        $Port=$selection.Port
    }
    $Port = $Port.Trim().ToUpperInvariant()
    $arguments = @(Get-FlashArguments $plan $Port $Baud ($toolVersion.Major -ge 5) $SkipUnchanged.IsPresent)
    if ($DryRun) {
        Write-Host "`nDry run: no serial port opened, no compilation, no flash writes." -ForegroundColor Green
        Write-Host ('& "' + $tool + '" ' + (($arguments | ForEach-Object { "'" + $_.Replace("'","''") + "'" }) -join ' '))
        return
    }
    foreach ($segment in $plan.Segments) { if ((Get-ControllerHash $segment.Path) -ne $segment.SHA256) { throw 'A binary changed after validation. Wait for the build/export to finish and retry.' } }
    if(-not(Confirm-ControllerFlash $selection $plan.Sketch)){Write-Host 'Cancelled. No flash writes.';return}
    $current=@(Get-ControllerPorts | Where-Object Port -eq $selection.Port)
    if($current.Count -ne 1 -or ($selection.HardwareId -and $current[0].HardwareId -ne $selection.HardwareId)){throw 'The selected board disconnected or changed while confirming. Run flash again to select its current port.'}
    foreach ($segment in $plan.Segments) { if ((Get-ControllerHash $segment.Path) -ne $segment.SHA256) { throw 'A binary changed while confirming. Run flash again.' } }
    Write-Host "`nFlashing $Port at $Baud baud. Close Arduino Serial Monitor first." -ForegroundColor Cyan
    & $tool @arguments
    if ($LASTEXITCODE -ne 0) { throw "Flashing failed (esptool exit $LASTEXITCODE). Check the port, close Serial Monitor, or retry with -Baud 460800. For S3 USB recovery, hold BOOT, tap RESET, release BOOT and reselect the port." }
    Write-Host 'Flash complete. The controller is restarting.' -ForegroundColor Green
}

if ($MyInvocation.InvocationName -ne '.') {
    try { Invoke-PrecompiledFlash } catch { Write-Host ("Flash error: " + $_.Exception.Message) -ForegroundColor Red; exit 1 }
}

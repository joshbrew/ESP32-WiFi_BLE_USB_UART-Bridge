#requires -Version 5.1
# Synthetic files and dry runs only: this suite never opens a serial port.
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'tools\flash_precompiled.ps1')
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('flash-script-tests-' + [guid]::NewGuid().ToString('N'))
$originalLocalData = $env:LOCALAPPDATA
New-Item -ItemType Directory -Path $testRoot | Out-Null

function Assert-Equal($Actual,$Expected,[string]$Label) {
    if ($Actual -ne $Expected) { throw "$Label : expected $Expected, got $Actual" }
}
function Assert-Fails([scriptblock]$Work,[string]$Pattern) {
    try { & $Work | Out-Null } catch { if ($_.Exception.Message -notmatch $Pattern) { throw }; return }
    throw "Expected a failure matching $Pattern"
}
function New-Image([string]$Path,[int]$ChipId,[int]$SizeCode,[int]$Length=8192) {
    $bytes = New-Object byte[] $Length
    $bytes[0]=0xE9; $bytes[1]=1; $bytes[2]=2; $bytes[3]=($SizeCode -shl 4) -bor 15
    [BitConverter]::GetBytes([uint16]$ChipId).CopyTo($bytes,12)
    [IO.File]::WriteAllBytes($Path,$bytes)
}
function New-Table([string]$Path,[bool]$HasOta=$true,[uint32]$AppOffset=0x10000,[uint32]$AppSize=0x300000) {
    $bytes = New-Object byte[] 3072
    for ($i=0;$i -lt $bytes.Length;$i++) { $bytes[$i]=255 }
    $rows = @()
    $rows += ,@(1,2,0x9000,0x5000,'nvs')
    if ($HasOta) { $rows += ,@(1,0,0xe000,0x2000,'otadata') }
    $subtype = 0
    if ($HasOta) { $subtype=0x10 }
    $rows += ,@(0,$subtype,$AppOffset,$AppSize,'app0')
    for ($i=0;$i -lt $rows.Count;$i++) {
        $offset=$i*32; $row=$rows[$i]
        for ($j=0;$j -lt 32;$j++) { $bytes[$offset+$j]=0 }
        [BitConverter]::GetBytes([uint16]0x50AA).CopyTo($bytes,$offset)
        $bytes[$offset+2]=[byte]$row[0]; $bytes[$offset+3]=[byte]$row[1]
        [BitConverter]::GetBytes([uint32]$row[2]).CopyTo($bytes,$offset+4)
        [BitConverter]::GetBytes([uint32]$row[3]).CopyTo($bytes,$offset+8)
        [Text.Encoding]::ASCII.GetBytes([string]$row[4]).CopyTo($bytes,$offset+12)
    }
    [IO.File]::WriteAllBytes($Path,$bytes)
}
function New-Fixture([string]$Case,[string]$VariantName) {
    $dir=Join-Path $testRoot $Case
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $definition=$script:Variants[$VariantName]; $prefix=$definition.Sketch+'.ino'
    $size=2; $fqbn='esp32:esp32:lolin32'
    if ($definition.Chip -eq 'esp32s3') { $size=4; $fqbn='esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,FlashMode=qio,USBMode=hwcdc,CDCOnBoot=cdc' }
    New-Image (Join-Path $dir ($prefix+'.bin')) $definition.ChipId $size
    New-Image (Join-Path $dir ($prefix+'.bootloader.bin')) $definition.ChipId $size 4096
    New-Table (Join-Path $dir ($prefix+'.partitions.bin'))
    [IO.File]::WriteAllBytes((Join-Path $dir 'boot_app0.bin'),(New-Object byte[] 8192))
    @{fqbn=$fqbn;sketchLocation=(Join-Path $testRoot $definition.Sketch)} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $dir 'build.options.json') -Encoding UTF8
    return $dir
}

try {
    foreach ($name in $script:Variants.Keys) {
        $dir=New-Fixture ('all-'+$name) $name
        $plan=Get-FlashPlan $name $dir $testRoot
        Assert-Equal $plan.Segments.Count 4 'four matching segments'
        Assert-Equal $plan.Segments[0].Offset $script:Variants[$name].BootOffset 'correct chip bootloader address'
        Assert-Equal $plan.Segments[3].Offset 0x10000 'partition-derived app address'
        $arguments=@(Get-FlashArguments $plan 'COM999' 460800 $true $false)
        foreach ($segment in $plan.Segments) { if ($arguments -notcontains $segment.Path) { throw 'Argument array lost a filename.' } }
        if ($arguments -contains '--erase-all' -or $arguments -contains '--force' -or ($arguments -join ' ') -match 'merged\.bin|arduino-cli|compile') { throw 'Unexpected destructive or compiling command.' }
    }
    $dir=New-Fixture "folder with spaces and ' quote" 's3-dispenser'
    $plan=Get-FlashPlan 's3-dispenser' $dir $testRoot
    $arguments=@(Get-FlashArguments $plan 'COM999' 921600 $true $true)
    if ($arguments -notcontains '--skip-flashed' -or $arguments -notcontains $plan.Segments[3].Path) { throw 'Skip/quoted filename arguments missing.' }
    $old=@(Get-FlashArguments $plan 'COM999' 921600 $false $false)
    if ($old -notcontains 'write_flash' -or $old -notcontains 'default_reset') { throw 'Old esptool syntax missing.' }
    Assert-Fails { Get-FlashArguments $plan 'COM999' 921600 $false $true } '5.3'
    Assert-Fails { Get-FlashArguments $plan 'COM24;bad' 921600 $true $false } 'Windows serial port'

    $bad=New-Fixture 'wrong-chip' 's3-dispenser'
    New-Image (Join-Path $bad 'ESP32S3_Dispenser_Controller.ino.bin') 0 4
    Assert-Fails { Get-FlashPlan 's3-dispenser' $bad $testRoot } 'chip mismatch'
    $bad=New-Fixture 'wrong-psram' 's3-dispenser'
    @{fqbn='esp32:esp32:esp32s3:FlashSize=16M,PSRAM=enabled'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $bad 'build.options.json') -Encoding UTF8
    Assert-Fails { Get-FlashPlan 's3-dispenser' $bad $testRoot } 'OPI PSRAM'
    $bad=New-Fixture 'oversized' 's3-dispenser'
    New-Image (Join-Path $bad 'ESP32S3_Dispenser_Controller.ino.bin') 9 4 (4MB)
    Assert-Fails { Get-FlashPlan 's3-dispenser' $bad $testRoot } 'does not fit'
    $bad=New-Fixture 'nvs-overlap' 's3-dispenser'
    New-Table (Join-Path $bad 'ESP32S3_Dispenser_Controller.ino.partitions.bin') $true 0x9000
    Assert-Fails { Get-FlashPlan 's3-dispenser' $bad $testRoot } 'overlap'
    $bad=New-Fixture 'missing-file' 's3-dispenser'
    Remove-Item -LiteralPath (Join-Path $bad 'ESP32S3_Dispenser_Controller.ino.partitions.bin')
    Assert-Fails { Get-FlashPlan 's3-dispenser' $bad $testRoot } 'Missing matching binary'
    $factory=New-Fixture 'factory' 's3-transport'
    New-Table (Join-Path $factory 'ESP32S3_Modular_Command_Transport_Controller.ino.partitions.bin') $false
    Assert-Equal (Get-FlashPlan 's3-transport' $factory $testRoot).Segments.Count 3 'factory build omits OTA seed'
    $fallback=New-Fixture 'core-seed' 's3-transport'
    $core=Join-Path $testRoot 'core\3.3.10';$coreSeed=Join-Path $core 'tools\partitions'
    New-Item -ItemType Directory -Path $coreSeed -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $fallback 'boot_app0.bin') -Destination $coreSeed
    Remove-Item -LiteralPath (Join-Path $fallback 'boot_app0.bin')
    @{fqbn='esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi';hardwareFolders=$core} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $fallback 'build.options.json') -Encoding UTF8
    $fallbackPlan=Get-FlashPlan 's3-transport' $fallback $testRoot
    Assert-Equal $fallbackPlan.Segments[2].Path (Join-Path $coreSeed 'boot_app0.bin') 'matching core OTA seed fallback'
    $sourceFolder=Join-Path $testRoot 'ESP32S3_Modular_Command_Transport_Controller\src'
    New-Item -ItemType Directory -Path $sourceFolder -Force | Out-Null
    $sourceFile=Join-Path $sourceFolder 'Example.cpp';[IO.File]::WriteAllText($sourceFile,'// source newer than binary')
    (Get-Item -LiteralPath $sourceFile).LastWriteTimeUtc=[datetime]::UtcNow.AddSeconds(10)
    Assert-Equal (Get-FlashPlan 's3-transport' $factory $testRoot).SourceNewer $true 'stale binary is reported'

    $cache=Join-Path $testRoot 'cache'; New-Item -ItemType Directory -Path $cache | Out-Null
    $oldDir=New-Fixture 'cache\older' 's3-dispenser'
    $newDir=New-Fixture 'cache\newer' 's3-dispenser'
    (Get-Item -LiteralPath (Join-Path $oldDir 'ESP32S3_Dispenser_Controller.ino.bin')).LastWriteTimeUtc=[datetime]::UtcNow.AddMinutes(-10)
    Assert-Equal (Find-PrecompiledBuild 's3-dispenser' $testRoot $cache) $newDir 'latest matching build discovery'
    $saved=Join-Path $testRoot 'precompiled\s3-dispenser'
    New-Item -ItemType Directory -Path $saved -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $oldDir 'ESP32S3_Dispenser_Controller.ino.bin') -Destination $saved
    Assert-Equal (Find-PrecompiledBuild 's3-dispenser' $testRoot $cache) $saved 'pinned build takes precedence'

    $env:LOCALAPPDATA=Join-Path $testRoot 'local-app-data'
    $toolDir=Join-Path $env:LOCALAPPDATA 'Arduino15\packages\esp32\tools\esptool_py\5.3.0'
    New-Item -ItemType Directory -Path $toolDir -Force | Out-Null
    $tool=Join-Path $toolDir 'esptool.exe'
    [IO.File]::WriteAllText($tool,'Not executable: dry run must never launch this.')
    Assert-Equal (Find-Esptool '') $tool 'Arduino tool discovery'
    $hostExe=[Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
    $flasher=Join-Path (Split-Path -Parent $PSScriptRoot) 'tools\flash_precompiled.ps1'
    $result=& $hostExe -NoProfile -ExecutionPolicy Bypass -File $flasher -Variant s3-dispenser -BuildDirectory $dir -EsptoolPath $tool -Port COM999 -DryRun 2>&1
    if ($LASTEXITCODE -ne 0 -or ($result -join "`n") -notmatch 'no serial port opened') { throw "Dry run failed or executed the fake tool: $result" }
    $controller=Join-Path (Split-Path -Parent $PSScriptRoot) 'tools\controller.ps1'
    $result=& $hostExe -NoProfile -ExecutionPolicy Bypass -File $controller flash -Variant s3-dispenser -BuildDirectory $dir -EsptoolPath $tool -Port COM999 -DryRun 2>&1
    if ($LASTEXITCODE -ne 0 -or ($result -join "`n") -notmatch 'no serial port opened') { throw "Nested CLI flash dry run failed or executed the fake tool: $result" }
    Write-Output 'PASS all four variants, chip/PSRAM/layout checks, bounds, quoting, old/new tool syntax, cache selection and no-device dry run.'
} finally {
    $env:LOCALAPPDATA=$originalLocalData
    $resolved=[IO.Path]::GetFullPath($testRoot)
    $parent=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
    if (-not $resolved.StartsWith($parent,[StringComparison]::OrdinalIgnoreCase) -or -not ([IO.Path]::GetFileName($resolved)).StartsWith('flash-script-tests-')) { throw 'Unexpected temporary test path.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

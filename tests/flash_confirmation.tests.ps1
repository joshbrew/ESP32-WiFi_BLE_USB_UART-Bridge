#requires -Version 5.1
# Executes only a recording PowerShell stand-in for esptool; never accesses serial.
$ErrorActionPreference='Stop'
$repository=Split-Path -Parent $PSScriptRoot
. (Join-Path $repository 'tools\controller.ps1')
. (Join-Path $repository 'tools\flash_precompiled.ps1')
$testRoot=Join-Path ([IO.Path]::GetTempPath()) ('flash-confirmation-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
function Assert($Condition,[string]$Label){if(-not $Condition){throw $Label}}
function Fails([scriptblock]$Work,[string]$Pattern){try{& $Work | Out-Null}catch{if($_.Exception.Message -match $Pattern){return};throw};throw "Expected failure: $Pattern"}
function New-CompleteBuild([string]$Directory){
    New-Item -ItemType Directory -Path $Directory -Force | Out-Null
    $prefix='ESP32S3_Dispenser_Controller.ino'
    foreach($name in @(($prefix+'.bin'),($prefix+'.bootloader.bin'))){
        $bytes=New-Object byte[] 4096;$bytes[0]=0xe9;$bytes[1]=1;$bytes[3]=0x4f;$bytes[12]=9
        [IO.File]::WriteAllBytes((Join-Path $Directory $name),$bytes)
    }
    $table=New-Object byte[] 3072;for($i=0;$i -lt $table.Length;$i++){$table[$i]=255}
    $rows=@();$rows+=,@(1,2,0x9000,0x5000);$rows+=,@(0,0,0x10000,0x300000)
    for($i=0;$i -lt $rows.Count;$i++){
        $offset=$i*32;for($j=0;$j -lt 32;$j++){$table[$offset+$j]=0}
        $table[$offset]=0xaa;$table[$offset+1]=0x50;$table[$offset+2]=$rows[$i][0];$table[$offset+3]=$rows[$i][1]
        [BitConverter]::GetBytes([uint32]$rows[$i][2]).CopyTo($table,$offset+4);[BitConverter]::GetBytes([uint32]$rows[$i][3]).CopyTo($table,$offset+8)
    }
    [IO.File]::WriteAllBytes((Join-Path $Directory ($prefix+'.partitions.bin')),$table)
    @{fqbn='esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Directory 'build.options.json') -Encoding UTF8
}
try{
    $BuildDirectory=Join-Path $testRoot "binary folder with ' quote";New-CompleteBuild $BuildDirectory
    $toolFolder=Join-Path $testRoot '5.3.0';New-Item -ItemType Directory -Path $toolFolder | Out-Null
    $EsptoolPath=Join-Path $toolFolder 'record-tool.ps1';$record=Join-Path $testRoot 'tool-call.json'
    $global:ControllerTestRecordPath=$record
    [IO.File]::WriteAllText($EsptoolPath,'$args | ConvertTo-Json | Set-Content -LiteralPath $global:ControllerTestRecordPath; $global:LASTEXITCODE=0')
    $Variant='s3-dispenser';$Port='';$Baud=460800;$DryRun=[switch]$false;$SkipUnchanged=[switch]$false
    $script:PortReads=0;$script:ChangeDevice=$false;$script:ChangeBinary=$false;$script:Answer='n'
    function Get-ControllerPorts{
        $script:PortReads++;$id='USB\VID_303A&PID_1001\board-one'
        if($script:ChangeDevice -and $script:PortReads -gt 1){$id='USB\VID_303A&PID_1001\board-two'}
        [pscustomobject]@{Port='COM24';Name='Simulated ESP32-S3';Reason='Test USB metadata';Score=90;HardwareId=$id}
    }
    function Read-Host([string]$Prompt){
        if($script:ChangeBinary){[IO.File]::AppendAllText((Join-Path $BuildDirectory 'ESP32S3_Dispenser_Controller.ino.bin'),'changed')}
        return $script:Answer
    }
    Invoke-PrecompiledFlash
    Assert (-not(Test-Path -LiteralPath $record)) 'Declined auto-detected port must never invoke esptool'
    $Port='COM24';$script:PortReads=0;Invoke-PrecompiledFlash
    Assert (-not(Test-Path -LiteralPath $record)) 'Explicit -Port must still require confirmation'
    $script:Answer='y';$script:PortReads=0;Invoke-PrecompiledFlash
    $arguments=[IO.File]::ReadAllText($record) | ConvertFrom-Json
    Assert ($arguments -contains 'COM24' -and $arguments -contains 'write-flash' -and $arguments -contains (Join-Path $BuildDirectory 'ESP32S3_Dispenser_Controller.ino.bin')) 'Confirmed flash must pass exact validated arguments to the recording tool'
    [IO.File]::Delete($record)
    $script:ChangeDevice=$true;$script:PortReads=0
    Fails {Invoke-PrecompiledFlash} 'disconnected or changed'
    Assert (-not(Test-Path -LiteralPath $record)) 'A device swap after confirmation must prevent writes'
    $script:ChangeDevice=$false;$script:ChangeBinary=$true;$script:PortReads=0
    Fails {Invoke-PrecompiledFlash} 'binary changed while confirming'
    Assert (-not(Test-Path -LiteralPath $record)) 'A binary change after confirmation must prevent writes'

    # Successful compile publication and failure preservation, with synthetic files.
    $environment=[pscustomobject]@{Tools=$testRoot;Core=(Join-Path $testRoot 'core')}
    $export=Join-Path $testRoot 'complete-export';New-CompleteBuild $export
    $output=Join-Path $testRoot 'precompiled\s3-dispenser'
    New-Item -ItemType Directory -Path $output -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $output 'previous-build.txt'),'keep previous set')
    $plan=[pscustomobject]@{Variant='s3-dispenser';Cache=(Join-Path $testRoot 'cache');Export=$export;Output=$output;Fqbn='esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi';Sketch=(Join-Path $repository 'ESP32S3_Dispenser_Controller')}
    Publish-ControllerBuild $plan $environment
    Assert (Test-Path -LiteralPath (Join-Path $output 'ESP32S3_Dispenser_Controller.ino.bin')) 'A complete saved build must be published'
    $backup=@(Get-ChildItem -LiteralPath (Split-Path -Parent $output) -Directory | Where-Object Name -like 's3-dispenser.previous-*')
    Assert ($backup.Count -eq 1 -and (Test-Path -LiteralPath (Join-Path $backup[0].FullName 'previous-build.txt'))) 'Previous saved binaries must be retained'
    $before=(Get-FileHash -LiteralPath (Join-Path $output 'ESP32S3_Dispenser_Controller.ino.bin')).Hash
    [IO.File]::Delete((Join-Path $export 'ESP32S3_Dispenser_Controller.ino.bin'))
    Fails {Publish-ControllerBuild $plan $environment} 'Missing matching binary'
    Assert ((Get-FileHash -LiteralPath (Join-Path $output 'ESP32S3_Dispenser_Controller.ino.bin')).Hash -eq $before) 'A failed/incomplete build must not replace the saved binaries'
    Write-Output 'PASS confirmed/declined flash dispatch, explicit-port confirmation, device/binary change guards and complete-build publication with previous-build preservation. Recording tool only; no serial access.'
}finally{
    Remove-Variable ControllerTestRecordPath -Scope Global -ErrorAction SilentlyContinue
    $full=[IO.Path]::GetFullPath($testRoot);$parent=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
    if(-not $full.StartsWith($parent,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($full) -notmatch '^flash-confirmation-[a-f0-9]{32}$'){throw 'Unexpected test cleanup path.'}
    Remove-Item -LiteralPath $full -Recurse -Force
}

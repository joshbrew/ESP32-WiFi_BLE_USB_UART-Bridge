#requires -Version 5.1
# Fake executables, synthetic images and simulated ports: never compiles or flashes.
$ErrorActionPreference='Stop'
$repository=Split-Path -Parent $PSScriptRoot
. (Join-Path $repository 'tools\controller.ps1')
$testRoot=Join-Path ([IO.Path]::GetTempPath()) ('controller-tests-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
function Assert($Condition,[string]$Label){if(-not $Condition){throw $Label}}
function Fails([scriptblock]$Work,[string]$Pattern){try{& $Work | Out-Null}catch{if($_.Exception.Message -match $Pattern){return};throw};throw "Expected failure: $Pattern"}
function Fake-Environment {
    $tools=Join-Path $testRoot ('tools-'+[guid]::NewGuid().ToString('N'))
    [pscustomobject]@{Tools=$tools;Cli=$null;Config=(Join-Path $tools 'arduino-cli.json');Data=(Join-Path $tools 'data');User=(Join-Path $tools 'sketchbook');Core=(Join-Path $tools 'data\packages\esp32\hardware\esp32\3.3.10')}
}
try{
    # Serial descriptions distinguish USB candidates from Bluetooth pseudo-ports.
    $devices=@(
        @{DeviceID='COM5';Name='Bluetooth (COM5)';PNPDeviceID='BTHENUM\test'},
        @{DeviceID='COM24';Name='Feather (COM24)';PNPDeviceID='USB\VID_239A&PID_800B'},
        @{DeviceID='COM7';Name='ESP32-S3 (COM7)';PNPDeviceID='USB\VID_303A&PID_1001'},
        @{DeviceID='COM9';Name='CP210x (COM9)';PNPDeviceID='USB\VID_10C4&PID_EA60'}
    ) | ForEach-Object {[pscustomobject]$_}
    $ports=@(ConvertTo-ControllerPorts $devices)
    Assert ($ports.Count -eq 3) 'Bluetooth must be excluded'
    Assert (@($ports | Where-Object Reason -Match 'model unverified').Count -eq 2) 'Bridge/Feather descriptors must not claim a verified board model'
    $script:TestPorts=@($ports | Where-Object Port -eq COM24)
    function Get-ControllerPorts {$script:TestPorts}
    $script:Answers=New-Object 'Collections.Generic.Queue[string]'
    function Read-Host([string]$Prompt){if(-not $script:Answers.Count){throw 'Unexpected interactive prompt'};return $script:Answers.Dequeue()}
    Assert ((Select-ControllerPort '').Port -eq 'COM24') 'A unique USB candidate should be suggested'
    $script:Answers.Enqueue('n');Assert (-not(Confirm-ControllerFlash $script:TestPorts[0] 's3-dispenser')) 'No must cancel'
    $script:Answers.Enqueue('');Assert (-not(Confirm-ControllerFlash $script:TestPorts[0] 's3-dispenser')) 'Empty answer must cancel'
    $script:Answers.Enqueue('yes');Assert (Confirm-ControllerFlash $script:TestPorts[0] 's3-dispenser') 'Explicit yes must confirm'
    Fails {Select-ControllerPort COM5} 'not present|Bluetooth'
    $script:TestPorts=$ports;$script:Answers.Enqueue('2');Assert ((Select-ControllerPort '').Port -eq $ports[1].Port) 'Multiple USB candidates require selection'
    $script:TestPorts=@();Fails {Select-ControllerPort ''} 'No usable COM ports'

    # Setup is idempotent and respects exact pinned package versions.
    $script:Operations=New-Object 'Collections.Generic.List[string]'
    function Invoke-ControllerCli($Environment,[string[]]$Arguments){
        $script:Operations.Add(($Arguments -join '|'))
        if($Arguments[0] -eq 'core' -and $Arguments[1] -eq 'update-index'){
            New-Item -ItemType Directory -Path $Environment.Data -Force | Out-Null
            foreach($index in @('package_index.json','package_esp32_index.json')){[IO.File]::WriteAllText((Join-Path $Environment.Data $index),'{}')}
        }
        if($Arguments[0] -eq 'core' -and $Arguments[1] -eq 'install'){New-Item -ItemType Directory -Path $Environment.Core -Force | Out-Null;[IO.File]::WriteAllText((Join-Path $Environment.Core 'platform.txt'),'mock')}
        if($Arguments[0] -eq 'lib' -and $Arguments[1] -eq 'install'){
            $parts=$Arguments[2] -split '@';$folder=Join-Path $Environment.User ('libraries\'+($parts[0] -replace ' ','_'))
            New-Item -ItemType Directory -Path $folder -Force | Out-Null
            [IO.File]::WriteAllText((Join-Path $folder 'library.properties'),"name=$($parts[0])`nversion=$($parts[1])`n")
        }
    }
    # Fake CLI is only used for its version call; all install operations above are mocks.
    $fakeCli=Join-Path $testRoot 'mock-cli.ps1'
    [IO.File]::WriteAllText($fakeCli,'Write-Output ''{"VersionString":"1.5.1"}''; $global:LASTEXITCODE=0')
    Assert ((Get-ControllerHash $fakeCli) -eq (Get-FileHash -LiteralPath $fakeCli -Algorithm SHA256).Hash) 'Portable file hashing must match the standard SHA-256 command'
    function Install-ArduinoCli($Environment){$script:Operations.Add('bootstrap-cli');$Environment.Cli=$fakeCli}
    $environment=Fake-Environment
    Ensure-ControllerEnvironment $environment $true
    Assert ($script:Operations -contains 'core|install|esp32:esp32@3.3.10') 'Setup must install the pinned ESP32 core'
    foreach($library in Get-ControllerLibraries $true){Assert (Test-ControllerLibrary $environment $library) "Missing library $($library.Name)"}
    $before=$script:Operations.Count;Ensure-ControllerEnvironment $environment $true
    Assert ($script:Operations.Count -eq $before) 'Repeated setup must not download/install again'

    foreach($name in $script:ControllerTargets.Keys){
        $plan=Get-ControllerCompilePlan $name $environment usb $false 2 $false $repository
        Assert ($plan.Arguments[0] -eq 'compile') 'Compile plan must use compile'
        Assert ($plan.Arguments -notcontains '--upload' -and $plan.Arguments -notcontains '--port') 'Compile must never upload or need a serial port'
        Assert ($plan.Arguments -notcontains '--clean') 'Incremental build cache must be retained by default'
        if($name.StartsWith('s3-')){Assert ($plan.Fqbn -match 'FlashSize=16M' -and $plan.Fqbn -match 'PSRAM=opi' -and $plan.Fqbn -match 'CPUFreq=240') 'S3 N16R8 settings incorrect'}
    }
    $com=Get-ControllerCompilePlan s3-dispenser $environment com $false 0 $true $repository
    Assert ($com.Fqbn -match 'CDCOnBoot=default' -and $com.Arguments -contains '--clean') 'COM serial route or explicit clean option missing'
    Fails {Get-ControllerCompilePlan lolin32-dispenser $environment usb $true 2 $false $repository} 'S3 variants only'
    Fails {Get-ControllerCompilePlan s3-dispenser $environment usb $true 2 $false $repository} 'LtePrivate.h'
    $lteRepository=Join-Path $testRoot 'lte-fixture'
    $lteSketch=Join-Path $lteRepository 'ESP32S3_Dispenser_Controller';$lteConfig=Join-Path $lteSketch 'src\config'
    New-Item -ItemType Directory -Path $lteConfig -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $lteSketch 'ESP32S3_Dispenser_Controller.ino'),'// synthetic sketch')
    [IO.File]::WriteAllText((Join-Path $lteConfig 'LtePrivate.h'),'// synthetic configuration; no credentials')
    $ltePlan=Get-ControllerCompilePlan s3-dispenser $environment usb $true 2 $false $lteRepository
    Assert ($ltePlan.Cellular -and $ltePlan.Arguments -contains 'compiler.cpp.extra_flags=-DAPP_ENABLE_LTE=1') 'LTE must be enabled in the compile invocation without editing QuickConfig'

    # Dry run must not call setup, compile, downloads, prompts or serial discovery.
    $hostExe=[Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
    $absentTools=Join-Path $testRoot 'never-created'
    $output=& $hostExe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repository 'tools\controller.ps1') compile -Variant s3-dispenser -ToolsDirectory $absentTools -DryRun 2>&1
    Assert ($LASTEXITCODE -eq 0 -and ($output -join "`n") -match 'no installation, compilation or serial access') 'Compile dry run failed'
    Assert (-not(Test-Path -LiteralPath $absentTools)) 'Compile dry run must not create a tools directory'
    Write-Output 'PASS USB/Feather discovery, Bluetooth exclusion, port selection/confirmation, idempotent setup, pinned dependencies, four compile profiles and side-effect-free dry run.'
}finally{
    $full=[IO.Path]::GetFullPath($testRoot);$parent=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
    if(-not $full.StartsWith($parent,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($full) -notmatch '^controller-tests-[a-f0-9]{32}$'){throw 'Unexpected test cleanup path.'}
    Remove-Item -LiteralPath $full -Recurse -Force
}

#requires -Version 5.1
<#
.SYNOPSIS
Setup, inspect, compile, and flash the ESP32 controller variants on Windows.
.EXAMPLE
.\controller.cmd compile -Variant s3-dispenser
.EXAMPLE
.\controller.cmd flash -Variant s3-dispenser
#>
[CmdletBinding()]
param(
    [Parameter(Position=0)][ValidateSet('help','ports','doctor','setup','compile','flash','field-setup','home-setup','modem-provision')][string]$Command='help',
    [ValidateSet('s3-dispenser','s3-transport','lolin32-dispenser','lolin32-transport')][string]$Variant,
    [string]$Port,[string]$BuildDirectory,[string]$ArduinoCliPath,[string]$EsptoolPath,
    [ValidateSet('usb','com')][string]$Serial='usb',
    [ValidateRange(0,64)][int]$Jobs=2,
    [ValidateRange(9600,2000000)][int]$Baud=921600,
    [switch]$Lte,[switch]$Clean,[switch]$SkipUnchanged,[switch]$DryRun,
    [string]$ToolsDirectory=(Join-Path $env:LOCALAPPDATA 'ESP32ControllerTools')
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'firmware_hash.ps1')
$script:ControllerRoot=Split-Path -Parent $PSScriptRoot
$script:CoreVersion='3.3.10'
$script:CliVersion='1.5.1'
$script:CliSha256='fabe42e0eb04d00e776a66178299ff95a46c623dbc260f997e58fd514853dd40'
$script:PackageUrl='https://espressif.github.io/arduino-esp32/package_esp32_index.json'
$script:ControllerTargets=@{
    's3-dispenser'='ESP32S3_Dispenser_Controller';'s3-transport'='ESP32S3_Modular_Command_Transport_Controller'
    'lolin32-dispenser'='ESP32_Dispenser_Controller';'lolin32-transport'='ESP32_Modular_Command_Transport_Controller'
}
. (Join-Path $PSScriptRoot 'serial_ports.ps1')

function Find-ArduinoCli([string]$Explicit,[string]$ToolsRoot){
    if($Explicit){if(-not(Test-Path -LiteralPath $Explicit -PathType Leaf)){throw 'The supplied Arduino CLI executable does not exist.'};return [IO.Path]::GetFullPath($Explicit)}
    $found=Get-Command arduino-cli.exe -ErrorAction SilentlyContinue
    if($found){return $found.Source}
    foreach($candidate in @((Join-Path $ToolsRoot "bin\arduino-cli-$script:CliVersion\arduino-cli.exe"),(Join-Path $env:LOCALAPPDATA 'Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'),(Join-Path $env:ProgramFiles 'Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'))){if(Test-Path -LiteralPath $candidate -PathType Leaf){return $candidate}}
    return $null
}
function Get-ControllerEnvironment([string]$ToolsRoot,[string]$ExplicitCli){
    $tools=[IO.Path]::GetFullPath($ToolsRoot)
    $shared=Join-Path $env:LOCALAPPDATA 'Arduino15'
    $data=Join-Path $tools 'data'
    if(Test-Path -LiteralPath (Join-Path $shared "packages\esp32\hardware\esp32\$script:CoreVersion\platform.txt")){$data=$shared}
    [pscustomobject]@{Tools=$tools;Cli=(Find-ArduinoCli $ExplicitCli $tools);Config=(Join-Path $tools 'arduino-cli.json');Data=$data;User=(Join-Path $tools 'sketchbook');Core=(Join-Path $data "packages\esp32\hardware\esp32\$script:CoreVersion")}
}
function Invoke-ControllerCli($Environment,[string[]]$Arguments){
    & $Environment.Cli @Arguments --config-file $Environment.Config --additional-urls $script:PackageUrl
    if($LASTEXITCODE -ne 0){throw "Arduino CLI command failed (exit $LASTEXITCODE). Check the message above; nothing has been flashed."}
}
function Remove-OwnedTemporary([string]$Path,[string]$Root){
    $full=[IO.Path]::GetFullPath($Path);$parent=[IO.Path]::GetFullPath($Root).TrimEnd('\')+'\'
    if(-not $full.StartsWith($parent,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($full) -notmatch '^(download|export|stage)-[a-f0-9]{32}$'){throw 'Unexpected temporary cleanup path.'}
    if(Test-Path -LiteralPath $full){Remove-Item -LiteralPath $full -Recurse -Force}
}
function Install-ArduinoCli($Environment){
    if(-not [Environment]::Is64BitOperatingSystem){throw 'The automatic installer needs 64-bit Windows. Install Arduino CLI manually and pass -ArduinoCliPath.'}
    $temp=Join-Path $Environment.Tools ('download-'+[guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temp -Force | Out-Null
    try{
        [Net.ServicePointManager]::SecurityProtocol=[Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $zip=Join-Path $temp 'arduino-cli.zip'
        Write-Host "Downloading official Arduino CLI $script:CliVersion (SHA-256 verified)..." -ForegroundColor Cyan
        Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/arduino/arduino-cli/releases/download/v$script:CliVersion/arduino-cli_$($script:CliVersion)_Windows_64bit.zip" -OutFile $zip
        if((Get-ControllerHash $zip) -ne $script:CliSha256){throw 'Arduino CLI download checksum mismatch. The archive will not be executed.'}
        $unpacked=Join-Path $temp 'unpacked';Expand-Archive -LiteralPath $zip -DestinationPath $unpacked
        $exe=Join-Path $unpacked 'arduino-cli.exe';if(-not(Test-Path -LiteralPath $exe)){throw 'Arduino CLI archive is incomplete.'}
        $destination=Join-Path $Environment.Tools "bin\arduino-cli-$script:CliVersion"
        New-Item -ItemType Directory -Path $destination -Force | Out-Null
        Copy-Item -LiteralPath $exe -Destination $destination -Force
        $Environment.Cli=Join-Path $destination 'arduino-cli.exe'
    }finally{Remove-OwnedTemporary $temp $Environment.Tools}
}
function Get-ControllerLibraries([bool]$Cellular){
    $libraries=@([pscustomobject]@{Name='Async TCP';Version='3.5.0'},[pscustomobject]@{Name='ESP Async WebServer';Version='3.12.1'})
    if($Cellular){$libraries+=@([pscustomobject]@{Name='TinyGSM';Version='0.12.0'},[pscustomobject]@{Name='ArduinoMqttClient';Version='0.1.8'},[pscustomobject]@{Name='ArduinoJson';Version='7.3.2'})}
    return $libraries
}
function Test-ControllerLibrary($Environment,$Library){
    $root=Join-Path $Environment.User 'libraries';if(-not(Test-Path -LiteralPath $root)){return $false}
    foreach($dir in Get-ChildItem -LiteralPath $root -Directory){
        $file=Join-Path $dir.FullName 'library.properties';if(-not(Test-Path -LiteralPath $file)){continue}
        $text=[IO.File]::ReadAllText($file)
        if($text -match ('(?m)^name='+[regex]::Escape($Library.Name)+'\s*$') -and $text -match ('(?m)^version='+[regex]::Escape($Library.Version)+'\s*$')){return $true}
    }
    return $false
}
function Ensure-ControllerEnvironment($Environment,[bool]$Cellular,[switch]$ToolsOnly){
    if(-not $Environment.Cli){Install-ArduinoCli $Environment}
    $versionText=(& $Environment.Cli version --json | Out-String)
    if($LASTEXITCODE -ne 0){throw 'Arduino CLI could not run.'}
    $version=$versionText | ConvertFrom-Json
    if([version]$version.VersionString -lt [version]'1.2.0'){throw 'Arduino CLI 1.2 or newer is required. Supply a current -ArduinoCliPath.'}
    New-Item -ItemType Directory -Path $Environment.Tools,$Environment.User -Force | Out-Null
    @{directories=@{data=$Environment.Data;downloads=(Join-Path $Environment.Tools 'downloads');user=$Environment.User}} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Environment.Config -Encoding UTF8
    $indexesMissing=-not(Test-Path -LiteralPath (Join-Path $Environment.Data 'package_index.json')) -or -not(Test-Path -LiteralPath (Join-Path $Environment.Data 'package_esp32_index.json'))
    if($indexesMissing){Invoke-ControllerCli $Environment @('core','update-index')}
    if(-not(Test-Path -LiteralPath (Join-Path $Environment.Core 'platform.txt'))){
        Write-Host "Installing ESP32 board package $script:CoreVersion; this first download can be large." -ForegroundColor Cyan
        if(-not $indexesMissing){Invoke-ControllerCli $Environment @('core','update-index')}
        Invoke-ControllerCli $Environment @('core','install',"esp32:esp32@$script:CoreVersion")
    }
    if(-not $ToolsOnly){
        $missing=@(Get-ControllerLibraries $Cellular | Where-Object {-not(Test-ControllerLibrary $Environment $_)})
        if($missing.Count){Invoke-ControllerCli $Environment @('lib','update-index')}
        foreach($library in $missing){Invoke-ControllerCli $Environment @('lib','install',"$($library.Name)@$($library.Version)",'--no-deps')}
    }
    Write-Host 'CLI tools and requested packages are ready.' -ForegroundColor Green
}
function Get-ControllerCompilePlan([string]$Name,$Environment,[string]$SerialRoute,[bool]$Cellular,[int]$ParallelJobs,[bool]$Fresh,[string]$Repository=$script:ControllerRoot){
    if(-not $script:ControllerTargets.ContainsKey($Name)){throw 'Compile needs -Variant; run controller help for the four names.'}
    $s3=$Name.StartsWith('s3-')
    if($Cellular -and -not $s3){throw 'LTE is supported by the S3 variants only.'}
    if($s3){$cdc='cdc';if($SerialRoute -eq 'com'){$cdc='default'};$fqbn="esp32:esp32:esp32s3:CPUFreq=240,FlashMode=qio,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=$cdc"}
    else{$fqbn='esp32:esp32:lolin32:CPUFreq=240,PartitionScheme=min_spiffs'}
    $sketch=Join-Path $Repository $script:ControllerTargets[$Name]
    if(-not(Test-Path -LiteralPath (Join-Path $sketch ($script:ControllerTargets[$Name]+'.ino')))){throw 'Sketch not found. Run this tool from a complete repository checkout.'}
    $sourceLte=$false
    if($s3){foreach($relative in @('QuickConfig.h','src\config\AppConfig.h')){$file=Join-Path $sketch $relative;if((Test-Path -LiteralPath $file) -and [IO.File]::ReadAllText($file) -match '(?m)^\s*#define\s+(?:APP_ENABLE_LTE|DRONE_CFG_ENABLE_LTE)\s+1(?:\s|$)'){$sourceLte=$true}}}
    $enabled=$Cellular -or $sourceLte
    if($enabled -and -not(Test-Path -LiteralPath (Join-Path $sketch 'src\config\LtePrivate.h'))){throw 'LTE needs src\config\LtePrivate.h in the selected sketch. Follow docs/LTE_FIELD_ACCESS.md before compiling with LTE.'}
    $profile="$SerialRoute-source";if($Cellular){$profile="$SerialRoute-lte"}
    $cache=Join-Path $Environment.Tools "build\$Name\$profile"
    $export=Join-Path $Environment.Tools ('export-'+[guid]::NewGuid().ToString('N'))
    $arguments=@('compile','--fqbn',$fqbn,'--jobs',[string]$ParallelJobs,'--build-path',$cache,'--output-dir',$export)
    if($Fresh){$arguments+='--clean'}
    if($Cellular){$arguments+=@('--build-property','compiler.cpp.extra_flags=-DAPP_ENABLE_LTE=1')}
    $arguments+=$sketch
    [pscustomobject]@{Variant=$Name;Sketch=$sketch;Fqbn=$fqbn;Cellular=$enabled;Arguments=$arguments;Cache=$cache;Export=$export;Output=(Join-Path $Repository "precompiled\$Name")}
}
function Find-ControllerEsptool($Environment){
    foreach($root in @($Environment.Data,(Join-Path $env:LOCALAPPDATA 'Arduino15'))){
        $folder=Join-Path $root 'packages\esp32\tools\esptool_py'
        if(Test-Path -LiteralPath $folder){foreach($version in Get-ChildItem -LiteralPath $folder -Directory | Sort-Object {try{[version]$_.Name}catch{[version]'0.0'}} -Descending){$tool=Join-Path $version.FullName 'esptool.exe';if(Test-Path -LiteralPath $tool){return $tool}}}
    }
    $tool=Get-Command esptool.exe -ErrorAction SilentlyContinue;if($tool){return $tool.Source};return $null
}
function Publish-ControllerBuild($Plan,$Environment){
    # Validate the complete new set before replacing the selected saved build.
    $prefix=$script:ControllerTargets[$Plan.Variant]+'.ino'
    $metadata=Join-Path $Plan.Cache 'build.options.json'
    if(Test-Path -LiteralPath $metadata){Copy-Item -LiteralPath $metadata -Destination $Plan.Export -Force}
    else{@{fqbn=$Plan.Fqbn;sketchLocation=$Plan.Sketch;hardwareFolders=$Environment.Core} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Plan.Export 'build.options.json') -Encoding UTF8}
    $seed=Join-Path $Environment.Core 'tools\partitions\boot_app0.bin'
    if(Test-Path -LiteralPath $seed){Copy-Item -LiteralPath $seed -Destination $Plan.Export -Force}
    $flasher=Join-Path $PSScriptRoot 'flash_precompiled.ps1'
    . $flasher
    $validated=Get-FlashPlan $Plan.Variant $Plan.Export $script:ControllerRoot
    $parent=Split-Path -Parent $Plan.Output
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    $stage=Join-Path $parent ('stage-'+[guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage | Out-Null
    try{
        foreach($file in @($validated.Segments.Path)+(Join-Path $Plan.Export 'build.options.json')){Copy-Item -LiteralPath $file -Destination $stage -Force}
        @{variant=$Plan.Variant;fqbn=$Plan.Fqbn;core=$script:CoreVersion;compiledUtc=[datetime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage 'controller-build.json') -Encoding UTF8
        $target=[IO.Path]::GetFullPath($Plan.Output);$base=[IO.Path]::GetFullPath($parent).TrimEnd('\')+'\'
        if(-not $target.StartsWith($base,[StringComparison]::OrdinalIgnoreCase) -or (Split-Path -Leaf $target) -ne $Plan.Variant){throw 'Unexpected saved build path.'}
        $backup=$null
        if(Test-Path -LiteralPath $target){$backup=$target+'.previous-'+[guid]::NewGuid().ToString('N');Move-Item -LiteralPath $target -Destination $backup}
        try{Move-Item -LiteralPath $stage -Destination $target}catch{if($backup -and -not(Test-Path -LiteralPath $target)){Move-Item -LiteralPath $backup -Destination $target};throw}
        Write-Host "Saved complete binary set: $target" -ForegroundColor Green
        if($backup){Write-Host "Previous set retained: $backup"}
    }finally{Remove-OwnedTemporary $stage $parent}
}
function Show-ControllerHelp {
    Write-Output @'
ESP32 Controller CLI (Windows PowerShell 5.1+)
  controller ports
  controller doctor
  controller setup [-Lte]
  controller field-setup [-Variant s3-dispenser] [-DryRun]
  controller home-setup [-Variant s3-dispenser] [-DryRun]
  controller modem-provision [-Variant s3-dispenser] [-Port COM24] [-DryRun]
  controller compile -Variant s3-dispenser [-Serial usb|com] [-Lte] [-Jobs 2] [-Clean] [-DryRun]
  controller flash -Variant s3-dispenser [-Port COM24] [-BuildDirectory path] [-Baud 921600] [-SkipUnchanged] [-DryRun]

Variants: s3-dispenser, s3-transport, lolin32-dispenser, lolin32-transport
Compile installs missing tools/packages, then saves a complete binary set. It never uploads.
Flash uses an existing binary set, detects USB serial candidates, and always asks before writing.
setup installs pinned dependencies without compiling. doctor/ports/help are read-only.
Use -ArduinoCliPath, -EsptoolPath or -ToolsDirectory to override tool discovery.
'@
}
function Invoke-ControllerCommand {
    if($Command -eq 'help'){Show-ControllerHelp;return}
    if($Command -in @('field-setup','home-setup','modem-provision')){
        if($Variant -and -not $Variant.StartsWith('s3-')){throw 'Field LTE setup requires an S3 variant.'}
        $params=@{ToolsDirectory=$ToolsDirectory;DryRun=$DryRun};if($Variant){$params.Variant=$Variant}
        $scriptName='field_setup.ps1';if($Command -eq 'home-setup'){$scriptName='home_setup.ps1'};if($Command -eq 'modem-provision'){$scriptName='modem_provision.ps1';if($Port){$params.Port=$Port}}
        $global:LASTEXITCODE=0
        & (Join-Path $PSScriptRoot $scriptName) @params
        if($LASTEXITCODE -ne 0){throw 'Field setup did not complete. Saved settings can be reused on the next run.'};return
    }
    if($Command -eq 'ports'){$ports=@(Get-ControllerPorts);if(-not $ports.Count){Write-Output 'No usable board COM ports found.'}else{$ports | Format-Table Port,Name,Reason -AutoSize};return}
    $environment=Get-ControllerEnvironment $ToolsDirectory $ArduinoCliPath
    if($Command -eq 'doctor'){
        Write-Output "Arduino CLI: $(if($environment.Cli){$environment.Cli}else{'missing (setup can install)'})"
        Write-Output "ESP32 core $script:CoreVersion : $(if(Test-Path -LiteralPath (Join-Path $environment.Core 'platform.txt')){'ready'}else{'missing'})"
        Write-Output "CLI data: $($environment.Data)"
        Write-Output "Private libraries: $($environment.User)\libraries"
        foreach($library in Get-ControllerLibraries $Lte.IsPresent){Write-Output "$($library.Name) $($library.Version): $(if(Test-ControllerLibrary $environment $library){'ready'}else{'setup needed'})"}
        Write-Output "esptool: $(Find-ControllerEsptool $environment)";return
    }
    if($Command -eq 'setup'){if($DryRun){Write-Output 'Dry run: setup would install missing CLI/core/libraries; no changes made.';return};Ensure-ControllerEnvironment $environment $Lte.IsPresent;return}
    if($Command -eq 'compile'){
        $plan=Get-ControllerCompilePlan $Variant $environment $Serial $Lte.IsPresent $Jobs $Clean.IsPresent
        Write-Host "Compile: $Variant`nBoard settings: $($plan.Fqbn)`nOutput: $($plan.Output)" -ForegroundColor Cyan
        if($DryRun){Write-Output ('Dry run: no installation, compilation or serial access. CLI arguments: '+($plan.Arguments -join ' '));return}
        Ensure-ControllerEnvironment $environment $plan.Cellular
        try{Invoke-ControllerCli $environment $plan.Arguments;Publish-ControllerBuild $plan $environment}finally{Remove-OwnedTemporary $plan.Export $environment.Tools}
        Write-Host 'Compile complete. Use controller flash for repeat uploads without compiling.' -ForegroundColor Green;return
    }
    if($Command -eq 'flash'){
        if(-not $EsptoolPath){$EsptoolPath=Find-ControllerEsptool $environment}
        if(-not $EsptoolPath -and -not $DryRun){Ensure-ControllerEnvironment $environment $false -ToolsOnly;$EsptoolPath=Find-ControllerEsptool $environment}
        $arguments=@{Baud=$Baud;SkipUnchanged=$SkipUnchanged;DryRun=$DryRun}
        foreach($entry in @{Variant=$Variant;Port=$Port;BuildDirectory=$BuildDirectory;EsptoolPath=$EsptoolPath}.GetEnumerator()){if($entry.Value){$arguments[$entry.Key]=$entry.Value}}
        $global:LASTEXITCODE=0
        & (Join-Path $PSScriptRoot 'flash_precompiled.ps1') @arguments
        if($LASTEXITCODE -ne 0){throw 'Flash command failed. See the message above.'}
    }
}
if($MyInvocation.InvocationName -ne '.'){
    try{Invoke-ControllerCommand}catch{Write-Host ('Controller error: '+$_.Exception.Message) -ForegroundColor Red;exit 1}
}

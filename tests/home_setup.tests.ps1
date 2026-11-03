#requires -Version 5.1
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
. (Join-Path $root 'tools\home_setup.ps1')
function Assert($value,[string]$message){if(-not $value){throw $message}}
$temporary=Join-Path ([IO.Path]::GetTempPath()) ('home-setup-tests-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary -Force | Out-Null
try{
    foreach($bad in @('127.0.0.1','10.0.0.1','192.168.1.50','172.16.1.1','100.64.0.1','100.127.255.254','169.254.1.1','224.0.0.1','not-an-ip')){Assert (-not(Test-HomePublicIPv4 $bad)) ('Private/CGNAT address accepted: '+$bad)}
    Assert (Test-HomePublicIPv4 '8.8.8.8') 'Public IPv4 should be accepted'
    $plan=Get-HomeForwardingText '8.8.8.8' 'control.example.com' 'mqtt.example.com' '192.168.1.50' '192.168.1.1'
    Assert ($plan -match '80\s+external -> 192.168.1.50 port 80' -and $plan -match '8883 external -> 192.168.1.50 port 8883' -and $plan -match 'CGNAT' -and $plan -match 'Wi-Fi OFF') 'Router plan needs real destination, ports and off-LAN test'
    # Reuse only the certificate fixture function; do not execute modem/wizard tests.
    $tokens=$null;$errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'field_setup.tests.ps1'),[ref]$tokens,[ref]$errors)
    $fixture=$ast.Find({param($node)$node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'New-PairingFixture'},$true)
    Assert ($null -ne $fixture) 'Certificate fixture missing'
    . ([scriptblock]::Create($fixture.Extent.Text))
    $pair=New-PairingFixture
    $script:Operations=New-Object 'Collections.Generic.List[object]'
    function Invoke-HomeDocker([string[]]$Arguments,[switch]$Capture){
        $script:Operations.Add(@($Arguments))
        $global:LASTEXITCODE=0
        if($Arguments -contains 'hash-password'){return ('$2b$12$'+('a'*53))}
        if($Arguments -contains '--mount'){
            $mount=$Arguments[[Array]::IndexOf($Arguments,'--mount')+1]
            $work=$mount -replace '^type=bind,source=(.*),target=/work$','$1'
            if($Arguments -contains 'home-prepare'){
                New-Item -ItemType Directory -Path (Join-Path $work 'secrets') | Out-Null
                Write-HomeText (Join-Path $work 'bootstrap.json') '{"webPassword":"private-browser","devicePassword":"private-device","relayPassword":"PRIVATE_RELAY","internalKey":"PRIVATE_PROXY"}'
                Write-HomeText (Join-Path $work 'secrets\ca.crt') $pair.caPem
            }
            if($Arguments -contains 'export_settings.py'){
                Write-HomeText (Join-Path $work 'secrets\device-settings.json') ($pair | ConvertTo-Json -Depth 8)
            }
        }
    }
    $homeDir=Join-Path $temporary 'private home';New-Item -ItemType Directory -Path $homeDir | Out-Null
    Protect-HomeDirectory $homeDir;Protect-HomeDirectory $homeDir
    $deployment=New-HomeDeployment $homeDir 'control.example.com' 'mqtt.example.com' 's3-controller' 's3-dispenser'
    Assert ($deployment -eq (Join-Path $homeDir 'server')) 'Deployment path must not contain Docker output'
    $relay=Join-Path $deployment 'ESP32S3_Modular_Command_Transport_Controller\relay'
    $envText=[IO.File]::ReadAllText((Join-Path $relay '.env'))
    Assert ($envText -match 'WEB_SOURCE=../../ESP32S3_Dispenser_Controller/web') 'Dispenser host must serve dispenser UI'
    Assert (-not(Test-Path -LiteralPath (Join-Path $relay 'bootstrap.json'))) 'Temporary plaintext bootstrap copy must be removed'
    Assert (Test-Path -LiteralPath (Join-Path $relay 'home_prepare.py')) 'Public package must include home bootstrap'
    Assert (@($script:Operations | Where-Object {$_ -contains 'mosquitto_passwd'}).Count -eq 2) 'Both separate MQTT identities need hashes'
    Assert (@($script:Operations | Where-Object {($_ -join ' ') -match 'docker.sock|2375|--privileged'}).Count -eq 0) 'Bootstrap must not mount Docker daemon or use privileged containers'
    $before=[IO.File]::ReadAllText((Join-Path $relay '.env'));$count=$script:Operations.Count
    $again=New-HomeDeployment $homeDir 'control.example.com' 'mqtt.example.com' 's3-controller' 's3-dispenser'
    Assert ($again -eq $deployment -and $script:Operations.Count -eq $count -and [IO.File]::ReadAllText((Join-Path $relay '.env')) -eq $before) 'Rerun must reuse credentials without any Docker regeneration'
    # Drive the complete wizard with only boundary calls mocked.
    function Ensure-HomeDocker {$script:Operations.Add(@('ensure-docker'))}
    function Invoke-HomeAdmin([string]$Task){$script:Operations.Add(@('admin',$Task))}
    function Read-Host([string]$Prompt){if($Prompt -match '^Can your router'){return 'y'};return 'n'}
    function Get-NetIPConfiguration {return [pscustomobject]@{IPv4DefaultGateway=[pscustomobject]@{NextHop='192.168.1.1'};IPv4Address=@([pscustomobject]@{IPAddress='192.168.1.50'})}}
    function Invoke-FieldNative([string]$Executable,[string[]]$Arguments,[switch]$Capture){$script:Operations.Add(@($Executable)+$Arguments);$global:LASTEXITCODE=0}
    $Variant='s3-dispenser';$WebDomain='control.example.com';$MqttDomain='mqtt.example.com';$Domain='example.com';$PublicAddress='8.8.8.8';$Apn='test.apn';$SkipModem=$true;$ToolsDirectory=Join-Path $temporary 'tools'
    # Use the already-created simulated deployment, matching the real pairing.
    function New-HomeDeployment {return $deployment}
    $script:RealWriteFirmware=(Get-Command Write-FieldFirmwareSettings).ScriptBlock
    $script:TestRepository=Join-Path $temporary 'fake-repository'
    New-Item -ItemType Directory -Path (Join-Path $script:TestRepository 'ESP32S3_Dispenser_Controller\src\config') -Force | Out-Null
    function Write-FieldFirmwareSettings($Profile,[string]$Tools){& $script:RealWriteFirmware $Profile $Tools $script:TestRepository}
    Invoke-HomeSetup
    Assert (@($script:Operations | Where-Object {($_ -join ' ') -match 'controller.ps1 setup -Lte'}).Count -eq 1) 'Home setup should install Arduino LTE dependencies'
    Assert (@($script:Operations | Where-Object {($_ -join ' ') -match 'controller.ps1 (compile|flash)'}).Count -eq 0) 'Home wizard must never compile or flash'
    Assert (@($script:Operations | Where-Object {$_ -contains '--wait'}).Count -eq 1) 'Host startup should wait for relay/broker readiness'
    $profile=Read-FieldProfile $ToolsDirectory $Variant
    Assert ($profile.hosting -eq 'home' -and $profile.publicAddress -eq $PublicAddress) 'Home profile must retain public IP separately from local host'
    $header=Join-Path $script:TestRepository 'ESP32S3_Dispenser_Controller\src\config\LtePrivate.h'
    $headerText=[IO.File]::ReadAllText($header)
    Assert ($headerText -match '#define LTE_WIFI_CA_PEM "-----BEGIN CERTIFICATE-----\\n' -and $headerText -match 'END CERTIFICATE-----\\n"') 'Wi-Fi CA must be embedded as a valid escaped PEM string'
    $report=Join-Path $ToolsDirectory 'field\s3-dispenser-credentials.txt'
    Assert (Test-Path -LiteralPath $report) 'Final private report missing'
    Assert (Test-Path -LiteralPath (Join-Path $ToolsDirectory 'home\s3-dispenser\ROUTER-SETUP.txt')) 'Final router checklist missing'
    Write-Output 'PASS home address checks, router plan, public-only packaging, private ACL reuse, isolated Docker bootstrap, preserved credentials, dispenser UI choice and complete mocked wizard. No installs, network, Docker, serial, compile or flash.'
}finally{
    $full=[IO.Path]::GetFullPath($temporary);$base=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
    if(-not $full.StartsWith($base,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($full) -notmatch '^home-setup-tests-[a-f0-9]{32}$'){throw 'Unexpected test cleanup path.'}
    Remove-Item -LiteralPath $full -Recurse -Force
}

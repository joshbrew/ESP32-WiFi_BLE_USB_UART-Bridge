#requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('s3-dispenser','s3-transport')][string]$Variant='s3-dispenser',
    [string]$Domain,[string]$WebDomain,[string]$MqttDomain,[string]$PublicAddress,
    [string]$DeviceId='s3-controller',[string]$Apn,[string]$ApnUser='', [string]$ApnPassword='',
    [switch]$DryRun,[switch]$SkipModem,
    [string]$ToolsDirectory=(Join-Path $env:LOCALAPPDATA 'ESP32ControllerTools')
)
$ErrorActionPreference='Stop'
$script:HomeExplicit=@($PSBoundParameters.Keys)
. (Join-Path $PSScriptRoot 'field_common.ps1')
. (Join-Path $PSScriptRoot 'field_dns.ps1')
. (Join-Path $PSScriptRoot 'field_package.ps1')
function Read-HomeAnswer([string]$Prompt,[string]$Default){
    $suffix='';if($Default){$suffix=" [$Default]"};$answer=Read-Host ($Prompt+$suffix)
    if(-not $answer){return $Default};return $answer.Trim()
}
function Test-HomePublicIPv4([string]$Value){
    $ip=$null
    if(-not[Net.IPAddress]::TryParse($Value,[ref]$ip) -or $ip.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork){return $false}
    $b=$ip.GetAddressBytes()
    return -not($b[0] -in @(0,10,127) -or $b[0] -ge 224 -or ($b[0]-eq 100 -and $b[1]-ge 64 -and $b[1]-le 127) -or ($b[0]-eq 169 -and $b[1]-eq 254) -or ($b[0]-eq 172 -and $b[1]-ge 16 -and $b[1]-le 31) -or ($b[0]-eq 192 -and $b[1]-eq 168))
}
function Invoke-HomeAdmin([string]$Task){
    $installer=Join-Path $PSScriptRoot 'install_home_prerequisites.ps1'
    $shell=Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $process=Start-Process -FilePath $shell -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$installer+'"'),'-Task',$Task) -Verb RunAs -WindowStyle Hidden -Wait -PassThru
    if($process.ExitCode -eq 3010){throw 'Windows features were enabled. Restart Windows, then run home setup again. No automatic restart was requested.'}
    if($process.ExitCode -ne 0){throw 'Home prerequisite setup failed. Check Windows virtualization/WSL and administrator access.'}
}
function Invoke-HomeDocker([string[]]$Arguments,[switch]$Capture){
    if($Capture){$result=(& $script:HomeDocker @Arguments | Out-String).Trim();if($LASTEXITCODE -ne 0){throw 'Docker command failed; check Docker Desktop.'};return $result}
    & $script:HomeDocker @Arguments
    if($LASTEXITCODE -ne 0){throw 'Docker command failed. Existing credentials were preserved; rerun setup after fixing Docker Desktop.'}
}
function Ensure-HomeDocker {
    $desktop=Join-Path $env:ProgramFiles 'Docker\Docker\Docker Desktop.exe'
    $docker=Get-Command docker.exe -ErrorAction SilentlyContinue
    if(-not $docker -and -not(Test-Path -LiteralPath $desktop)){
        Invoke-HomeAdmin 'Wsl'
        if(-not(Get-Command winget.exe -ErrorAction SilentlyContinue)){throw 'Install Microsoft App Installer (winget), then rerun. https://aka.ms/getwinget'}
        Write-Host 'Installing Docker Desktop from the Windows package manager. Complete any installer/license prompts.'
        & winget.exe install --exact --id Docker.DockerDesktop --source winget --accept-source-agreements
        if($LASTEXITCODE -ne 0){throw 'Docker Desktop installation needs attention. Restart Windows if requested, then rerun home setup.'}
    }
    $script:HomeDocker=Join-Path $env:ProgramFiles 'Docker\Docker\resources\bin\docker.exe'
    if($docker){$script:HomeDocker=$docker.Source}
    if(-not(Test-Path -LiteralPath $script:HomeDocker)){throw 'Docker CLI was not found. Complete Docker Desktop installation and rerun.'}
    if(-not(Test-HomeDockerReady) -and (Test-Path -LiteralPath $desktop)){Start-Process -FilePath $desktop -WindowStyle Hidden | Out-Null}
    while($true){
        if(Test-HomeDockerReady){break}
        if((Read-Host 'Finish Docker Desktop first-run setup, select Linux containers / WSL 2, then Enter to retry (Q exits)') -match '^(?i:q)$'){throw 'Docker is not ready; run home setup again when it is running.'}
    }
    $context=Invoke-HomeDocker @('context','inspect') -Capture | ConvertFrom-Json
    if($env:DOCKER_HOST -or $context[0].Endpoints.docker.Host -notmatch '^npipe://'){throw 'Home hosting requires the local Docker Desktop context. Clear DOCKER_HOST and select desktop-linux; remote Docker contexts are not used.'}
    Invoke-HomeDocker @('compose','version')
}
function Test-HomeDockerReady {
    # PS 5.1 turns redirected native stderr into errors when the daemon is down.
    $saved=$ErrorActionPreference
    try{$ErrorActionPreference='Continue';$kind=(& $script:HomeDocker info --format '{{.OSType}}' 2>$null | Out-String).Trim();return ($LASTEXITCODE -eq 0 -and $kind -eq 'linux')}
    finally{$ErrorActionPreference=$saved}
}
function Protect-HomeDirectory([string]$Path){
    $expected=@([Security.Principal.WindowsIdentity]::GetCurrent().User.Value,'S-1-5-18')
    $current=Get-Acl -LiteralPath $Path
    $rules=@($current.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier]))
    $unexpected=@($rules | Where-Object {$expected -notcontains $_.IdentityReference.Value -or $_.AccessControlType -ne 'Allow' -or $_.FileSystemRights -ne [Security.AccessControl.FileSystemRights]::FullControl -or $_.IsInherited -or $_.InheritanceFlags -ne 'ContainerInherit,ObjectInherit'})
    if($current.AreAccessRulesProtected -and $rules.Count -eq $expected.Count -and -not $unexpected.Count){return}
    $acl=New-Object Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true,$false)
    foreach($sid in @([Security.Principal.WindowsIdentity]::GetCurrent().User,(New-Object Security.Principal.SecurityIdentifier 'S-1-5-18'))){
        $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid,'FullControl','ContainerInherit,ObjectInherit','None','Allow')))
    }
    Set-Acl -LiteralPath $Path -AclObject $acl
}
function Write-HomeText([string]$Path,[string]$Text){[IO.File]::WriteAllText($Path,$Text.Replace("`r`n","`n"),(New-Object Text.UTF8Encoding $false))}
function New-HomeDeployment([string]$HomeRoot,[string]$Web,[string]$Mqtt,[string]$Device,[string]$Name){
    $destination=Join-Path $HomeRoot 'server'
    if(Test-Path -LiteralPath $destination){return $destination}
    $pending=Join-Path $HomeRoot ('pending-'+[guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $pending -Force | Out-Null
    try{
        $archive=Join-Path $pending 'public-source.tgz'
        New-FieldServerArchive $archive
        Invoke-FieldNative 'tar.exe' @('-xzf',$archive,'-C',$pending)
        Remove-Item -LiteralPath $archive -Force
        $relay=Join-Path $pending 'ESP32S3_Modular_Command_Transport_Controller\relay'
        # Linux tools run in a disposable container. No Python/OpenSSL install on Windows,
        # no Docker socket in the container, no credentials in its output.
        $mount='type=bind,source='+$relay+',target=/work'
        Invoke-HomeDocker @('run','--rm','--mount',$mount,'-w','/work','python:3.12-alpine','sh','-c','apk add --no-cache openssl >/dev/null && python home_prepare.py /work "$@"','home-prepare',$Web,$Mqtt,$Device,$Name)
        $bootstrap=Join-Path $relay 'bootstrap.json'
        $keys=[IO.File]::ReadAllText($bootstrap) | ConvertFrom-Json
        $hash=Invoke-HomeDocker @('run','--rm','caddy:2-alpine','caddy','hash-password','--plaintext',$keys.webPassword) -Capture
        if($hash -notmatch '^\$2[aby]\$[0-9]{2}\$[./a-zA-Z0-9]{53}$'){throw 'Unexpected browser password hash; configuration was not installed.'}
        $secretMount='type=bind,source='+(Join-Path $relay 'secrets')+',target=/work'
        Invoke-HomeDocker @('run','--rm','--mount',$secretMount,'eclipse-mosquitto:2','mosquitto_passwd','-b','-c','/work/passwords',$Device,$keys.devicePassword)
        Invoke-HomeDocker @('run','--rm','--mount',$secretMount,'eclipse-mosquitto:2','mosquitto_passwd','-b','/work/passwords','relay',$keys.relayPassword)
        $source='../web';if($Name -eq 's3-dispenser'){$source='../../ESP32S3_Dispenser_Controller/web'}
        $lines=@(('WEB_DOMAIN='+$Web),('MQTT_DOMAIN='+$Mqtt),'WEB_USER=field',("WEB_PASSWORD_HASH='"+$hash+"'"),('INTERNAL_RELAY_KEY='+$keys.internalKey),('RELAY_MQTT_PASSWORD='+$keys.relayPassword),('DEVICE_ID='+$Device),('WEB_SOURCE='+$source),('FIRMWARE_VARIANT='+$Name))
        Write-HomeText (Join-Path $relay '.env') (($lines -join "`n")+"`n")
        $settings=@(('Broker: '+$Mqtt+':8883'),('Device ID and MQTT user: '+$Device),('MQTT password: '+$keys.devicePassword),'Provision secrets/ca.crt as relay-ca.',('Browser: https://'+$Web),'Login: field',('Browser password: '+$keys.webPassword))
        Write-HomeText (Join-Path $relay 'secrets\device-settings.txt') (($settings -join "`n")+"`n")
        Invoke-HomeDocker @('run','--rm','--mount',$mount,'-w','/work','python:3.12-alpine','python','export_settings.py','/work')
        Remove-Item -LiteralPath $bootstrap -Force
        # Verify both absolute paths remain inside this particular home directory.
        $base=[IO.Path]::GetFullPath($HomeRoot).TrimEnd('\')+'\'
        foreach($path in @($pending,$destination)){if(-not([IO.Path]::GetFullPath($path)).StartsWith($base,[StringComparison]::OrdinalIgnoreCase)){throw 'Unexpected home deployment path.'}}
        Move-Item -LiteralPath $pending -Destination $destination
        return $destination
    }finally{
        $full=[IO.Path]::GetFullPath($pending);$base=[IO.Path]::GetFullPath($HomeRoot).TrimEnd('\')+'\'
        if(-not $full.StartsWith($base,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($full) -notmatch '^pending-[a-f0-9]{32}$'){throw 'Unexpected staging cleanup path.'}
        if(Test-Path -LiteralPath $full){Remove-Item -LiteralPath $full -Recurse -Force}
    }
}
function Get-HomeForwardingText([string]$Address,[string]$Web,[string]$Mqtt,[string]$Lan,[string]$Gateway){
    return @"
HOME ROUTER CHECKLIST
Router admin address: $Gateway
Reserve this laptop's LAN address in DHCP: $Lan
Forward TCP 80   external -> $Lan port 80   (HTTPS certificate issuance/renewal)
Forward TCP 443  external -> $Lan port 443  (browser login and live console)
Forward TCP 8883 external -> $Lan port 8883 (MQTT/TLS from the LTE modem)
Use TCP for all three. Do not forward 1883, 8080, Docker or SSH ports.
DNS-only A records: $Web and $Mqtt -> $Address
Remove conflicting AAAA records unless IPv6 routing/firewall is configured too.
Compare router WAN IPv4 with $Address. A different/private/100.64-127.x.x WAN
usually means double NAT or ISP CGNAT. Double NAT needs forwarding on both routers;
CGNAT needs a public IPv4 from your ISP, or use the VPS hosting option.
Test from phone cellular data with Wi-Fi OFF, not from this home network.
If remote works but home Wi-Fi cannot open the public name, enable NAT loopback
on the router or add LAN DNS overrides for BOTH names -> $Lan (same TLS names).
Keep this laptop awake, powered and connected; keep Docker Desktop running.
Start Docker Desktop when you sign in. Containers restart automatically once Docker starts.
If public IPv4 changes, update BOTH DNS records (router DDNS or rerun home setup).
"@
}
function Invoke-HomeSetup {
    if($DryRun){
        Write-Output 'DRY RUN: Windows home laptop -> install/reuse WSL 2 and Docker Desktop; generate/reuse private broker and browser credentials; publish TCP 80/443/8883; add Private-network firewall rules; show router DHCP/port-forward/CGNAT checklist; DNS-only records to HOME PUBLIC IPv4; start broker/web app; generate LTE + Wi-Fi CA settings; optional modem provisioning. No firmware compilation or flashing.'
        Write-Output 'No prompts, file writes, network requests, installs, firewall changes, Docker commands or hardware access.';return
    }
    Write-Host 'ESP32 field access hosted on this Windows laptop' -ForegroundColor Cyan
    if($ToolsDirectory -match '[,\r\n]'){throw 'For Docker bind mounts, choose a ToolsDirectory without commas or line breaks.'}
    Write-Host 'Requires Windows 10/11 with WSL 2 virtualization and a reachable home public IPv4. Router forwarding is a manual step; no router passwords are collected.'
    $previous=Read-FieldProfile $ToolsDirectory $Variant
    if($previous){
        if(-not $WebDomain){$WebDomain=$previous.webDomain};if(-not $MqttDomain){$MqttDomain=$previous.mqttDomain};if(-not $Apn){$Apn=$previous.apn}
        if($script:HomeExplicit -notcontains 'DeviceId'){$DeviceId=$previous.pairing.deviceId}
        if($script:HomeExplicit -notcontains 'ApnUser'){$ApnUser=$previous.apnUser};if($script:HomeExplicit -notcontains 'ApnPassword'){$ApnPassword=$previous.apnPassword}
    }
    if(-not $WebDomain -and -not $MqttDomain){if(-not $Domain){$Domain=Read-HomeAnswer 'Your domain (for example yourdomain.com)' ''};$WebDomain='control.'+$Domain;$MqttDomain='mqtt.'+$Domain}
    if(-not $WebDomain){$WebDomain=Read-HomeAnswer 'Console hostname' ''};if(-not $MqttDomain){$MqttDomain=Read-HomeAnswer 'MQTT hostname' ''}
    if(-not $PublicAddress){
        $detected='';try{$detected=([string](Invoke-RestMethod -Uri 'https://api.ipify.org' -TimeoutSec 10)).Trim()}catch{}
        $PublicAddress=Read-HomeAnswer 'Home PUBLIC IPv4 (confirm against router WAN address)' $detected
    }
    if(-not $Apn){$Apn=Read-HomeAnswer 'SIM provider APN' ''}
    if(-not(Test-FieldDomain $WebDomain) -or -not(Test-FieldDomain $MqttDomain) -or -not(Test-HomePublicIPv4 $PublicAddress) -or $DeviceId -notmatch '^[a-zA-Z0-9_-]{1,40}$' -or $DeviceId -eq 'relay' -or $Apn -notmatch '^[a-zA-Z0-9._-]{1,100}$'){throw 'Check the DNS hostnames, home PUBLIC IPv4, device ID and SIM APN. Private LAN IPs cannot be reached from LTE.'}
    $network=@(Get-NetIPConfiguration | Where-Object {$_.IPv4DefaultGateway -and $_.IPv4Address})
    $lan='YOUR-LAPTOP-LAN-IP';$gateway='your router settings page'
    if($network.Count){$lan=[string]$network[0].IPv4Address[0].IPAddress;$gateway=[string]$network[0].IPv4DefaultGateway.NextHop}
    $plan=Get-HomeForwardingText $PublicAddress $WebDomain $MqttDomain $lan $gateway
    Write-Host $plan -ForegroundColor Cyan
    if((Read-Host 'Can your router receive inbound connections at that public IPv4? Check WAN/CGNAT above. Continue? [y/N]') -notmatch '^(?i:y|yes)$'){Write-Host 'No deployment started. Use Setup Field Access.cmd for VPS hosting if your ISP cannot provide inbound access.';return}
    Ensure-HomeDocker
    $records=@(Get-FieldDnsRecords $WebDomain $MqttDomain $PublicAddress);Show-FieldDnsPlan $records $ToolsDirectory ($Variant+'-home')
    if((Read-Host 'Create/update these DNS records automatically in Cloudflare? [y/N]') -match '^(?i:y|yes)$'){
        if(-not $Domain){$Domain=Read-HomeAnswer 'Cloudflare zone/domain' ''}
        Write-Host 'Use a token scoped to this zone: Zone Read + DNS Edit. It is not saved.'
        $secure=Read-Host 'Cloudflare API token' -AsSecureString;$ptr=[Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
        try{$token=[Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr);Set-FieldCloudflareDns $Domain $records $token}finally{[Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr);$token=$null}
    }
    $homeRoot=Join-Path $ToolsDirectory ('home\'+$Variant)
    New-Item -ItemType Directory -Path $homeRoot -Force | Out-Null;Protect-HomeDirectory $homeRoot
    $deployment=New-HomeDeployment $homeRoot $WebDomain $MqttDomain $DeviceId $Variant
    $relay=Join-Path $deployment 'ESP32S3_Modular_Command_Transport_Controller\relay'
    $pair=Get-FieldPairing ([IO.File]::ReadAllText((Join-Path $relay 'secrets\device-settings.json')) | ConvertFrom-Json)
    if($pair.variant -ne $Variant -or $pair.deviceId -ne $DeviceId -or $pair.mqttHost -ne $MqttDomain -or ([uri]$pair.webUrl).Host -ne $WebDomain){throw 'Saved home deployment has different hostnames/device. Existing credentials were preserved; rerun using the original names or choose a separate ToolsDirectory.'}
    Invoke-HomeAdmin 'Firewall'
    Write-Host 'Firewall rules cover Private networks. In Windows network settings, mark your trusted home connection Private.'
    Invoke-HomeDocker @('compose','--project-name',('esp32-home-'+$Variant),'--project-directory',$relay,'-f',(Join-Path $relay 'compose.yaml'),'up','-d','--build','--wait','--wait-timeout','90')
    $profile=[pscustomobject]@{variant=$Variant;server='localhost';sshUser='';sshPort=22;domain=$Domain;webDomain=$WebDomain;mqttDomain=$MqttDomain;apn=$Apn;apnUser=$ApnUser;apnPassword=$ApnPassword;pairing=$pair;modemBaud=$null;modemProvisioned=$false;hosting='home';publicAddress=$PublicAddress}
    if($previous -and $previous.pairing.caSha256 -eq $pair.caSha256 -and $previous.pairing.deviceId -eq $pair.deviceId){$profile.modemBaud=$previous.modemBaud;$profile.modemProvisioned=$previous.modemProvisioned}
    Save-FieldProfile $profile $ToolsDirectory;Write-FieldFirmwareSettings $profile $ToolsDirectory
    Write-HomeText (Join-Path $homeRoot 'ROUTER-SETUP.txt') $plan
    Invoke-FieldNative (Join-Path $PSScriptRoot 'controller.ps1') @('setup','-Lte','-ToolsDirectory',$ToolsDirectory)
    if(-not $SkipModem -and (Read-Host 'Configure modem CA/clock through a confirmed UART adapter now? [Y/n]') -notmatch '^(?i:n|no)$'){
        & (Join-Path $PSScriptRoot 'modem_provision.ps1') -Variant $Variant -ToolsDirectory $ToolsDirectory
    }
    $report=Join-Path $ToolsDirectory ('field\'+$Variant+'-credentials.txt')
    Write-Host ([IO.File]::ReadAllText($report)) -ForegroundColor Cyan
    Write-Host $plan
    Write-Host "Private credentials: $report"
    Write-Host "Router checklist: $(Join-Path $homeRoot 'ROUTER-SETUP.txt')"
    Write-Host "Host files/keys: $relay (back up privately; broker certificate expires in one year)."
    Write-Host 'Finish DNS and router forwarding before testing on phone cellular data. Container startup alone does not prove external reachability.'
    Write-Host "No firmware was compiled/flashed. When ready: controller compile -Variant $Variant -Lte"
}
if($MyInvocation.InvocationName -ne '.'){
    try{Invoke-HomeSetup}catch{Write-Host ('Home setup error: '+$_.Exception.Message) -ForegroundColor Red;exit 1}
}

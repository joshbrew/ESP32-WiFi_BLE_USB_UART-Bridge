#requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('s3-dispenser','s3-transport')][string]$Variant='s3-dispenser',
    [string]$Server,[string]$SshUser,[ValidateRange(1,65535)][int]$SshPort=22,
    [string]$Domain,[string]$WebDomain,[string]$MqttDomain,[string]$DeviceId='s3-controller',
    [string]$Apn,[string]$ApnUser='', [string]$ApnPassword='',
    [switch]$DryRun,[switch]$SkipModem,[switch]$BuildAndFlash,
    [string]$ToolsDirectory=(Join-Path $env:LOCALAPPDATA 'ESP32ControllerTools')
)
$ErrorActionPreference='Stop'
$script:FieldExplicit=@($PSBoundParameters.Keys)
. (Join-Path $PSScriptRoot 'field_common.ps1')
. (Join-Path $PSScriptRoot 'field_dns.ps1')
function Read-FieldAnswer([string]$Prompt,[string]$Default){
    $suffix='';if($Default){$suffix=" [$Default]"}
    $answer=Read-Host ($Prompt+$suffix)
    if(-not $answer){return $Default};return $answer.Trim()
}
function Resolve-FieldAddress([string]$Name){@([Net.Dns]::GetHostAddresses($Name) | ForEach-Object IPAddressToString)}
function Invoke-FieldController([string[]]$Arguments){
    $global:LASTEXITCODE=0
    & (Join-Path $PSScriptRoot 'controller.ps1') @Arguments
}
. (Join-Path $PSScriptRoot 'field_package.ps1')
function Get-FieldRemoteCommands([string]$Archive,[string]$Web,[string]$Mqtt,[string]$Device,[string]$Name,[bool]$AsRoot=$false){
    if($Archive -notmatch '^/tmp/esp32-field-[a-f0-9]{32}\.tgz$' -or -not(Test-FieldDomain $Web) -or -not(Test-FieldDomain $Mqtt) -or $Device -notmatch '^[a-zA-Z0-9_-]{1,40}$' -or $Device -eq 'relay' -or $Name -notin @('s3-dispenser','s3-transport')){throw 'Invalid remote setup arguments.'}
    $install='/opt/esp32-controller/ESP32S3_Modular_Command_Transport_Controller/relay/install.sh'
    $prefix='sudo -n ';if($AsRoot){$prefix=''}
    $parts=@(($prefix+'bash'),(ConvertTo-FieldShell $install),'--web-domain',(ConvertTo-FieldShell $Web),'--mqtt-domain',(ConvertTo-FieldShell $Mqtt),'--device-id',(ConvertTo-FieldShell $Device),'--variant',(ConvertTo-FieldShell $Name))
    $deploy=$prefix+'mkdir -p /opt/esp32-controller && '+$prefix+'tar -xzf '+(ConvertTo-FieldShell $Archive)+' -C /opt/esp32-controller && '+($parts -join ' ')
    [pscustomobject]@{Deploy=$deploy;Pairing=($prefix+'cat /opt/esp32-controller/ESP32S3_Modular_Command_Transport_Controller/relay/secrets/device-settings.json')}
}
function Invoke-FieldSetup {
    if($DryRun){
        Write-Output 'DRY RUN: generate DNS record table/zone file (optional Cloudflare API); package current relay + UI sources; SSH upload to your Ubuntu VPS; install missing Docker Engine, Compose, OpenSSL, Python and curl; reuse/generate credentials and CA; start services; retrieve private pairing over SSH; generate LtePrivate.h and private LTE credentials report; install Arduino LTE dependencies; offer modem provisioning and compile/flash.'
        Write-Output 'No prompts, files, network calls, package installs, serial access, compilation or deployment in this dry run.';return
    }
    Write-Host 'ESP32-S3 field access setup' -ForegroundColor Cyan
    Write-Host 'Use an Ubuntu VPS with SSH access (root or passwordless sudo). Point two DNS-only hostnames at it and allow TCP 80,443,8883 in its cloud firewall.'
    $previous=Read-FieldProfile $ToolsDirectory $Variant
    if($previous){
        if(-not $Server){$Server=$previous.server};if(-not $Domain){$Domain=$previous.domain};if(-not $WebDomain){$WebDomain=$previous.webDomain};if(-not $MqttDomain){$MqttDomain=$previous.mqttDomain};if(-not $Apn){$Apn=$previous.apn}
        if($script:FieldExplicit -notcontains 'SshUser'){$SshUser=$previous.sshUser};if($script:FieldExplicit -notcontains 'SshPort'){$SshPort=$previous.sshPort}
        if($script:FieldExplicit -notcontains 'DeviceId'){$DeviceId=$previous.pairing.deviceId}
        if($script:FieldExplicit -notcontains 'ApnUser'){$ApnUser=$previous.apnUser};if($script:FieldExplicit -notcontains 'ApnPassword'){$ApnPassword=$previous.apnPassword}
    }
    if(-not $Server){$Server=Read-FieldAnswer 'VPS public IP or hostname' ([string]$previous.server)}
    if(-not $SshUser){$SshUser=Read-FieldAnswer 'SSH username' 'ubuntu'}
    if(-not $WebDomain -and -not $MqttDomain){
        if(-not $Domain){$Domain=Read-FieldAnswer 'Your domain (for example yourdomain.com)' ''}
        $WebDomain='control.'+$Domain;$MqttDomain='mqtt.'+$Domain
    }
    if(-not $WebDomain){$WebDomain=Read-FieldAnswer 'Console hostname' ''}
    if(-not $MqttDomain){$MqttDomain=Read-FieldAnswer 'MQTT hostname' ''}
    if(-not $Apn){$Apn=Read-FieldAnswer 'SIM provider APN' ([string]$previous.apn)}
    if($Server -notmatch '^[a-zA-Z0-9][a-zA-Z0-9.-]{0,252}$' -or $Server -match '\.\.' -or $SshUser -notmatch '^[a-z_][a-zA-Z0-9_-]{0,31}$' -or -not(Test-FieldDomain $WebDomain) -or -not(Test-FieldDomain $MqttDomain) -or $DeviceId -notmatch '^[a-zA-Z0-9_-]{1,40}$' -or $DeviceId -eq 'relay' -or $Apn -notmatch '^[a-zA-Z0-9._-]{1,100}$'){throw 'Check the server, SSH user, DNS hostnames, device ID and SIM APN. Use plain names, not URLs.'}
    Ensure-FieldWindowsTools
    $addresses=@(Resolve-FieldAddress $Server)
    $ipv4=@($addresses | Where-Object {$_ -match '^\d+\.\d+\.\d+\.\d+$'})
    if(-not $ipv4.Count){throw 'Use the public IPv4 address or an SSH hostname with an A record.'}
    $records=@(Get-FieldDnsRecords $WebDomain $MqttDomain $ipv4[0])
    Show-FieldDnsPlan $records $ToolsDirectory $Variant
    if(-not(Test-FieldDnsReady $records)){
        if((Read-Host 'Create/update these records automatically in Cloudflare? [y/N]') -match '^(?i:y|yes)$'){
            if(-not $Domain){$Domain=Read-FieldAnswer 'Cloudflare zone/domain name' ''}
            Write-Host 'Use a token scoped to this zone with Zone Read and DNS Edit. The token is used for DNS only and is not saved.'
            $secure=Read-Host 'Cloudflare API token' -AsSecureString
            $pointer=[Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
            try{$token=[Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer);Set-FieldCloudflareDns $Domain $records $token}
            finally{[Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer);$token=$null}
            Write-Host 'Waiting up to one minute for DNS to become visible...'
            for($attempt=0;$attempt -lt 12 -and -not(Test-FieldDnsReady $records);$attempt++){Start-Sleep -Seconds 5}
        }
        while(-not(Test-FieldDnsReady $records)){
            if((Read-Host 'Add the displayed records (or import the zone file), then press Enter to recheck; Q saves the instructions and exits') -match '^(?i:q)$'){Write-Host 'DNS instructions saved. Rerun setup when DNS is ready.';return}
        }
    }
    $target=$SshUser+'@'+$Server
    $ssh=@('-p',[string]$SshPort,'-o','StrictHostKeyChecking=ask',$target)
    Write-Host 'Checking SSH and sudo access. OpenSSH may ask for your first host-key approval or login.'
    $preflight='sudo -n true';if($SshUser -ceq 'root'){$preflight='true'}
    Invoke-FieldNative 'ssh.exe' ($ssh+@($preflight))
    $temp=Join-Path $ToolsDirectory ('field-run-'+[guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temp -Force | Out-Null
    $remote='/tmp/esp32-field-'+[guid]::NewGuid().ToString('N')+'.tgz'
    try{
        $archive=Join-Path $temp 'relay.tgz';New-FieldServerArchive $archive
        Invoke-FieldNative 'scp.exe' @('-P',[string]$SshPort,'-o','StrictHostKeyChecking=ask',$archive,($target+':'+$remote))
        $commands=Get-FieldRemoteCommands $remote $WebDomain $MqttDomain $DeviceId $Variant ($SshUser -ceq 'root')
        Invoke-FieldNative 'ssh.exe' ($ssh+@($commands.Deploy))
        $json=Invoke-FieldNative 'ssh.exe' ($ssh+@($commands.Pairing)) -Capture
        if($json.Length -gt 32768){throw 'Pairing report exceeded the expected size.'}
        $pair=Get-FieldPairing ($json | ConvertFrom-Json)
        if($pair.variant -ne $Variant -or $pair.deviceId -ne $DeviceId -or $pair.mqttHost -ne $MqttDomain -or ([uri]$pair.webUrl).Host -ne $WebDomain){throw 'Returned pairing settings do not match the requested deployment.'}
        $profile=[pscustomobject]@{variant=$Variant;server=$Server;sshUser=$SshUser;sshPort=$SshPort;domain=$Domain;webDomain=$WebDomain;mqttDomain=$MqttDomain;apn=$Apn;apnUser=$ApnUser;apnPassword=$ApnPassword;pairing=$pair;modemBaud=$null;modemProvisioned=$false}
        # A repeated setup preserves successful provisioning only for the same CA/device.
        if($previous -and $previous.pairing.caSha256 -eq $pair.caSha256 -and $previous.pairing.deviceId -eq $pair.deviceId){$profile.modemBaud=$previous.modemBaud;$profile.modemProvisioned=$previous.modemProvisioned}
        Save-FieldProfile $profile $ToolsDirectory
        Write-FieldFirmwareSettings $profile $ToolsDirectory
        Invoke-FieldController @('setup','-Lte','-ToolsDirectory',$ToolsDirectory)
        if($LASTEXITCODE -ne 0){throw 'Arduino dependency setup failed. Server/pairing settings were saved; rerun setup to resume.'}
        if(-not $SkipModem -and ((Read-Host 'Connect the modem adapter and configure its CA/clock now? [Y/n]') -notmatch '^(?i:n|no)$')){
            & (Join-Path $PSScriptRoot 'modem_provision.ps1') -Variant $Variant -ToolsDirectory $ToolsDirectory
            if($LASTEXITCODE -ne 0){Write-Host 'Modem provisioning remains pending. Rerun controller modem-provision after checking wiring.' -ForegroundColor Yellow}
        }
        $report=Join-Path $ToolsDirectory ('field\'+$Variant+'-credentials.txt')
        Write-Host ("`n"+[IO.File]::ReadAllText($report)) -ForegroundColor Cyan
        Write-Host "Private credentials report: $report"
        Write-Host 'The profile is also saved encrypted for this Windows account. Reconnect the shield UART to ESP32 GPIO17/18 after adapter provisioning.'
        if($BuildAndFlash -or ((Read-Host 'Compile LTE firmware and continue to confirmed flashing now? [y/N]') -match '^(?i:y|yes)$')){
            Invoke-FieldController @('compile','-Variant',$Variant,'-Lte','-ToolsDirectory',$ToolsDirectory)
            if($LASTEXITCODE -ne 0){throw 'Compile failed. The previous saved binary set was preserved.'}
            Invoke-FieldController @('flash','-Variant',$Variant,'-ToolsDirectory',$ToolsDirectory)
            if($LASTEXITCODE -ne 0){throw 'Flashing did not complete. Use controller flash to retry the saved build.'}
        }else{Write-Host "When ready: controller compile -Variant $Variant -Lte ; then controller flash -Variant $Variant"}
    }finally{
        # Only remove our own public-source archive, never remote settings or keys.
        try{Invoke-FieldNative 'ssh.exe' ($ssh+@(('rm -f -- '+(ConvertTo-FieldShell $remote))))}catch{Write-Verbose 'Public-source upload archive remains in remote /tmp.'}
        $full=[IO.Path]::GetFullPath($temp);$base=[IO.Path]::GetFullPath($ToolsDirectory).TrimEnd('\')+'\'
        if(-not $full.StartsWith($base,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($full) -notmatch '^field-run-[a-f0-9]{32}$'){throw 'Unexpected setup cleanup path.'}
        if(Test-Path -LiteralPath $full){Remove-Item -LiteralPath $full -Recurse -Force}
    }
}
if($MyInvocation.InvocationName -ne '.'){
    try{Invoke-FieldSetup}catch{Write-Host ('Field setup error: '+$_.Exception.Message) -ForegroundColor Red;exit 1}
}

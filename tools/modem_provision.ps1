#requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('s3-dispenser','s3-transport')][string]$Variant='s3-dispenser',
    [string]$Port,[switch]$DryRun,
    [string]$ToolsDirectory=(Join-Path $env:LOCALAPPDATA 'ESP32ControllerTools')
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'field_common.ps1')
. (Join-Path $PSScriptRoot 'serial_ports.ps1')
function Wait-FieldModem($Connection,[switch]$Prompt,[int]$TimeoutMs=10000){
    $clock=[Diagnostics.Stopwatch]::StartNew();$text=''
    while($clock.ElapsedMilliseconds -lt $TimeoutMs){
        $text+=[string]$Connection.ReadExisting()
        if($text.Length -gt 16384){throw 'Modem response exceeded the limit.'}
        if($text -match '(?m)^\s*(?:ERROR|\+CME ERROR[^\r\n]*)\s*$'){return [pscustomobject]@{Ok=$false;Text=$text}}
        if(($Prompt -and $text -match '(?m)^\s*>') -or (-not $Prompt -and $text -match '(?m)^\s*OK\s*$')){return [pscustomobject]@{Ok=$true;Text=$text}}
        Start-Sleep -Milliseconds 15
    }
    throw 'Modem response timed out. Check power, SOFT UART routing, adapter wiring and ESP32 UART disconnection.'
}
function Send-FieldModem($Connection,[string]$Command,[switch]$AllowError,[int]$TimeoutMs=10000){
    $Connection.Write($Command+"`r")
    $reply=Wait-FieldModem $Connection -TimeoutMs $TimeoutMs
    if(-not $reply.Ok -and -not $AllowError){throw 'The modem rejected a provisioning command. Check module firmware and the saved setup instructions.'}
    return $reply
}
function Provision-FieldModem($Connection,$Pairing,[datetime]$Utc=[DateTime]::UtcNow){
    $pair=Get-FieldPairing $Pairing
    $null=Send-FieldModem $Connection 'AT'
    $model=Send-FieldModem $Connection 'AT+CGMM'
    if($model.Text -notmatch '(?im)^\s*SARA-R4[A-Z0-9-]*\s*$'){throw 'This is not an identified SARA-R4 modem. No certificate or clock settings were written.'}
    $null=Send-FieldModem $Connection 'ATE0'
    $der=[Convert]::FromBase64String((($pair.caPem -replace '-----BEGIN CERTIFICATE-----|-----END CERTIFICATE-----','') -replace '\s',''))
    $md5=[Security.Cryptography.MD5]::Create()
    try{$expected=([BitConverter]::ToString($md5.ComputeHash($der))).Replace('-','').ToLowerInvariant()}finally{$md5.Dispose()}
    # u-blox uses an MD5 identifier for stored DER; TLS peer verification stays enabled.
    $stored=Send-FieldModem $Connection 'AT+USECMNG=4,0,"relay-ca"' -AllowError
    if(-not $stored.Ok -or $stored.Text -notmatch [regex]::Escape($expected)){
        $bytes=[Text.Encoding]::UTF8.GetBytes($pair.caPem)
        $Connection.Write(('AT+USECMNG=0,0,"relay-ca",'+$bytes.Length+"`r"))
        $prompt=Wait-FieldModem $Connection -Prompt
        if(-not $prompt.Ok){throw 'Modem did not accept the certificate transfer.'}
        $Connection.Write($bytes,0,$bytes.Length)
        $result=Wait-FieldModem $Connection
        if(-not $result.Ok){throw 'Modem certificate import failed.'}
        $stored=Send-FieldModem $Connection 'AT+USECMNG=4,0,"relay-ca"'
        if($stored.Text -notmatch [regex]::Escape($expected)){throw 'Stored modem CA fingerprint did not match. Setup remains incomplete.'}
    }
    $null=Send-FieldModem $Connection ('AT+CCLK="'+$Utc.ToUniversalTime().ToString('yy/MM/dd,HH:mm:ss')+'+00"')
    $check=Send-FieldModem $Connection 'AT+CCLK?'
    if($check.Text -notmatch '\+CCLK:\s*"(\d{2}/\d{2}/\d{2},\d{2}:\d{2}:\d{2})([+-]\d{2})"'){throw 'Could not verify the modem clock.'}
    $read=[datetime]::ParseExact($Matches[1],'yy/MM/dd,HH:mm:ss',[Globalization.CultureInfo]::InvariantCulture)
    $read=$read.AddMinutes(-15*[int]$Matches[2])
    if([math]::Abs(($read-$Utc.ToUniversalTime()).TotalSeconds) -gt 300){throw 'Modem clock verification failed.'}
    foreach($command in @('AT+USECPRF=0','AT+USECPRF=0,0,3','AT+USECPRF=0,1,3','AT+USECPRF=0,3,"relay-ca"',('AT+USECPRF=0,4,"'+$pair.mqttHost+'"'))){$null=Send-FieldModem $Connection $command}
    Write-Host 'Modem CA, clock and verified TLS profile are ready.' -ForegroundColor Green
}
function Invoke-ModemProvision {
    if($DryRun){Write-Output 'DRY RUN: select/confirm modem adapter; identify SARA-R4; detect 115200/9600 baud; compare/import raw CA bytes; verify CA, UTC clock and TLS profile. No serial access or changes.';return}
    $profile=Read-FieldProfile $ToolsDirectory $Variant
    if(-not $profile){throw 'Run Setup Field Access.cmd first to obtain matching server settings.'}
    $pair=Get-FieldPairing $profile.pairing
    Write-Host 'Use a 3.3V USB-UART adapter: TX -> shield D9, RX <- D8, common GND; SOFT switch; IOREF 3.3V. Disconnect ESP32 UART while provisioning. Power the shield separately.'
    $selection=Select-ControllerPort $Port
    if(-not $selection){return}
    if((Read-Host "Configure the SARA-R4 on $($selection.Port) ($($selection.Name))? [y/N]") -notmatch '^(?i:y|yes)$'){Write-Host 'Modem setup skipped.';return}
    $connection=$null;$speed=0
    foreach($baud in @(115200,9600)){
        $candidate=New-Object IO.Ports.SerialPort($selection.Port,$baud,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
        $candidate.Handshake=[IO.Ports.Handshake]::None;$candidate.DtrEnable=$false;$candidate.RtsEnable=$false;$candidate.WriteTimeout=5000
        try{$candidate.Open();$candidate.DiscardInBuffer();$null=Send-FieldModem $candidate 'AT' -TimeoutMs 1500;$connection=$candidate;$speed=$baud;break}
        catch{$candidate.Dispose()}
    }
    if(-not $connection){throw 'No responding modem found at 115200 or 9600. Check the selected adapter, modem power and UART wiring.'}
    try{
        Provision-FieldModem $connection $pair
        $profile.modemBaud=$speed;$profile.modemProvisioned=$true
        Save-FieldProfile $profile $ToolsDirectory;Write-FieldFirmwareSettings $profile $ToolsDirectory
    }finally{$connection.Dispose()}
    Write-Host "Detected $speed baud; matching firmware setting saved. Reconnect ESP32 GPIO17 <- D8 and GPIO18 -> D9."
}
if($MyInvocation.InvocationName -ne '.'){
    try{Invoke-ModemProvision}catch{Write-Host ('Modem setup error: '+$_.Exception.Message) -ForegroundColor Red;exit 1}
}

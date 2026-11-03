#requires -Version 5.1
$script:FieldRoot=Split-Path -Parent $PSScriptRoot
function Ensure-FieldWindowsTools {
    if(-not(Get-Command ssh.exe -ErrorAction SilentlyContinue) -or -not(Get-Command scp.exe -ErrorAction SilentlyContinue)){
        Write-Host 'Installing the Windows OpenSSH Client. Windows may request administrator approval.'
        $installer=Join-Path $PSScriptRoot 'install_windows_tools.ps1'
        $shell=Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
        $process=Start-Process -FilePath $shell -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$installer+'"')) -Verb RunAs -WindowStyle Hidden -Wait -PassThru
        if($process.ExitCode -eq 3010){throw 'Restart Windows to finish OpenSSH installation, then rerun field setup.'}
        if($process.ExitCode -ne 0){throw 'Windows OpenSSH installation did not complete.'}
        $env:PATH+=';'+(Join-Path $env:WINDIR 'System32\OpenSSH')
    }
    foreach($name in @('ssh.exe','scp.exe','tar.exe')){if(-not(Get-Command $name -ErrorAction SilentlyContinue)){throw "Required Windows 10/11 tool is unavailable: $name"}}
}
function Test-FieldDomain([string]$Name){
    if($Name.Length -gt 253 -or $Name -notmatch '^[a-zA-Z0-9][a-zA-Z0-9.-]*\.[a-zA-Z0-9.-]*[a-zA-Z0-9]$'){return $false}
    foreach($label in $Name.Split('.')){if($label.Length -eq 0 -or $label.Length -gt 63 -or $label -match '^-|-$'){return $false}};return $true
}
function Protect-FieldFile([string]$Path){
    $identities=@([Security.Principal.WindowsIdentity]::GetCurrent().User,(New-Object Security.Principal.SecurityIdentifier 'S-1-5-18'))
    $expected=@($identities | ForEach-Object Value | Select-Object -Unique)
    $current=Get-Acl -LiteralPath $Path
    $rules=@($current.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier]))
    $unexpected=@($rules | Where-Object {$expected -notcontains $_.IdentityReference.Value -or $_.AccessControlType -ne 'Allow' -or $_.FileSystemRights -ne [Security.AccessControl.FileSystemRights]::FullControl -or $_.IsInherited})
    if($current.AreAccessRulesProtected -and $rules.Count -eq $expected.Count -and -not $unexpected.Count){return}
    $acl=New-Object Security.AccessControl.FileSecurity
    $acl.SetAccessRuleProtection($true,$false)
    foreach($sid in $identities){
        $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid,'FullControl','Allow')))
    }
    Set-Acl -LiteralPath $Path -AclObject $acl
}
function Save-FieldProtected([byte[]]$Bytes,[string]$Path){
    Add-Type -AssemblyName System.Security
    $cipher=[Security.Cryptography.ProtectedData]::Protect($Bytes,[Text.Encoding]::UTF8.GetBytes('esp32-field-v1'),[Security.Cryptography.DataProtectionScope]::CurrentUser)
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    [IO.File]::WriteAllBytes($Path,$cipher);Protect-FieldFile $Path
}
function Read-FieldProtected([string]$Path){
    Add-Type -AssemblyName System.Security
    $plain=[Security.Cryptography.ProtectedData]::Unprotect([IO.File]::ReadAllBytes($Path),[Text.Encoding]::UTF8.GetBytes('esp32-field-v1'),[Security.Cryptography.DataProtectionScope]::CurrentUser)
    return [Text.Encoding]::UTF8.GetString($plain)
}
function Get-FieldPairing($Data){
    if($Data.version -ne 1 -or $Data.deviceId -notmatch '^[a-zA-Z0-9_-]{1,40}$' -or $Data.deviceId -eq 'relay' -or $Data.mqttUser -ne $Data.deviceId -or $Data.mqttPort -ne 8883 -or -not(Test-FieldDomain $Data.mqttHost) -or $Data.caName -ne 'relay-ca' -or $Data.variant -notin @('s3-dispenser','s3-transport')){throw 'Invalid or unsupported server pairing settings.'}
    foreach($password in @($Data.mqttPassword,$Data.webPassword)){if(-not $password -or $password.Length -gt 256 -or $password -match '[\r\n\x00]'){throw 'Invalid pairing password field.'}}
    $url=$null
    if(-not[uri]::TryCreate([string]$Data.webUrl,[UriKind]::Absolute,[ref]$url) -or $url.Scheme -ne 'https' -or $url.Port -ne 443 -or $url.AbsolutePath -ne '/' -or $url.UserInfo -or $url.Query -or $url.Fragment -or -not(Test-FieldDomain $url.Host)){throw 'Pairing requires an HTTPS console hostname.'}
    if($Data.caPem.Length -gt 8192 -or $Data.caPem -notmatch '^\s*-----BEGIN CERTIFICATE-----\s*([A-Za-z0-9+/=\r\n]+)\s*-----END CERTIFICATE-----\s*$'){throw 'Invalid public CA certificate in pairing settings.'}
    try{
        $der=[Convert]::FromBase64String(($Matches[1] -replace '\s',''))
        $cert=New-Object Security.Cryptography.X509Certificates.X509Certificate2 -ArgumentList (,$der)
        $sha=[Security.Cryptography.SHA256]::Create()
        try{$fingerprint=([BitConverter]::ToString($sha.ComputeHash($der))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
        if($fingerprint -ne $Data.caSha256 -or $Data.caSha256 -notmatch '^[a-fA-F0-9]{64}$'){throw 'CA fingerprint mismatch'}
        $ca=@($cert.Extensions | Where-Object {$_.Oid.Value -eq '2.5.29.19'})
        if($ca.Count -ne 1 -or -not $ca[0].CertificateAuthority){throw 'Not a CA certificate'}
        if([DateTime]::UtcNow -lt $cert.NotBefore.ToUniversalTime() -or [DateTime]::UtcNow -gt $cert.NotAfter.ToUniversalTime()){throw 'CA date invalid'}
    }catch{throw 'CA validation failed. Check the pairing file and computer clock.'}finally{if($cert){$cert.Dispose()}}
    return $Data
}
function Get-FieldProfilePath([string]$Tools,[string]$Name){Join-Path $Tools "field\$Name.protected"}
function Save-FieldProfile($Profile,[string]$Tools){
    $json=$Profile | ConvertTo-Json -Depth 12
    Save-FieldProtected ([Text.Encoding]::UTF8.GetBytes($json)) (Get-FieldProfilePath $Tools $Profile.variant)
}
function Read-FieldProfile([string]$Tools,[string]$Name){
    $path=Get-FieldProfilePath $Tools $Name
    if(Test-Path -LiteralPath $path){return (Read-FieldProtected $path | ConvertFrom-Json)}
    return $null
}
function ConvertTo-FieldCppString([string]$Value){
    if($Value -match '[\r\n\x00]'){throw 'Settings cannot contain line breaks or null bytes.'}
    return '"'+$Value.Replace('\','\\').Replace('"','\"').Replace("`t",'\t')+'"'
}
function Write-FieldFirmwareSettings($Profile,[string]$Tools,[string]$Repository=$script:FieldRoot){
    $pair=Get-FieldPairing $Profile.pairing
    if($Profile.variant -ne $pair.variant -or $Profile.apn -notmatch '^[a-zA-Z0-9._-]{1,100}$'){throw 'Pairing variant or SIM APN is invalid.'}
    $folder='ESP32S3_Dispenser_Controller';if($Profile.variant -eq 's3-transport'){$folder='ESP32S3_Modular_Command_Transport_Controller'}
    $path=Join-Path $Repository "$folder\src\config\LtePrivate.h"
    $values=[ordered]@{LTE_APN=$Profile.apn;LTE_APN_USER=[string]$Profile.apnUser;LTE_APN_PASSWORD=[string]$Profile.apnPassword;LTE_BROKER_HOST=$pair.mqttHost;LTE_DEVICE_ID=$pair.deviceId;LTE_MQTT_USER=$pair.mqttUser;LTE_MQTT_PASSWORD=$pair.mqttPassword;LTE_CA_NAME=$pair.caName}
    $text="// Generated by ESP32 field setup. This file contains private settings.`n"
    if(Test-Path -LiteralPath $path){$text=[IO.File]::ReadAllText($path)}
    $original=$text
    foreach($key in $values.Keys){
        $line='#define '+$key+' '+(ConvertTo-FieldCppString $values[$key])
        $pattern='(?m)^\s*#\s*define\s+'+[regex]::Escape($key)+'\b[^\r\n]*'
        if([regex]::IsMatch($text,$pattern)){$text=[regex]::Replace($text,$pattern,[Text.RegularExpressions.MatchEvaluator]{param($match)$line})}
        else{$text=$text.TrimEnd()+"`n"+$line+"`n"}
    }
    # PEM is public data. A single escaped string keeps repeated setup idempotent.
    $pem=$pair.caPem.Replace("`r",'').Replace('\','\\').Replace('"','\"').Replace("`n",'\n')
    $caLine='#define LTE_WIFI_CA_PEM "'+$pem+'"'
    $caPattern='(?m)^\s*#\s*define\s+LTE_WIFI_CA_PEM\b[^\r\n]*'
    if([regex]::IsMatch($text,$caPattern)){$text=[regex]::Replace($text,$caPattern,[Text.RegularExpressions.MatchEvaluator]{param($match)$caLine})}
    else{$text=$text.TrimEnd()+"`n"+$caLine+"`n"}
    if($Profile.modemBaud){
        if($Profile.modemBaud -notin @(9600,115200)){throw 'Unsupported detected modem baud rate.'}
        $line='#define LTE_UART_BAUD '+$Profile.modemBaud
        if($text -match '(?m)^\s*#\s*define\s+LTE_UART_BAUD\b'){$text=[regex]::Replace($text,'(?m)^\s*#\s*define\s+LTE_UART_BAUD\b[^\r\n]*',$line)}else{$text+=$line+"`n"}
    }
    if((Test-Path -LiteralPath $path) -and $original -ne $text){
        $backup=Join-Path $Tools ('field\backups\'+$Profile.variant+'-'+[guid]::NewGuid().ToString('N')+'.protected')
        Save-FieldProtected ([Text.Encoding]::UTF8.GetBytes($original)) $backup
    }
    if(-not(Test-Path -LiteralPath $path) -or $original -ne $text){[IO.File]::WriteAllText($path,$text,(New-Object Text.UTF8Encoding $false));Protect-FieldFile $path}
    $caPath=Join-Path $Tools ('field\'+$Profile.variant+'-ca.crt')
    New-Item -ItemType Directory -Path (Split-Path -Parent $caPath) -Force | Out-Null
    [IO.File]::WriteAllText($caPath,$pair.caPem,(New-Object Text.UTF8Encoding $false))
    $report=Join-Path $Tools ('field\'+$Profile.variant+'-credentials.txt')
    $baud='115200 (default; run modem provisioning to verify)';if($Profile.modemBaud){$baud=[string]$Profile.modemBaud}
    $lines=@('PRIVATE: ESP32 / LTE CLIENT SETTINGS',('APN: '+$Profile.apn),('APN user: '+$Profile.apnUser),('APN password: '+$Profile.apnPassword),('MQTT host: '+$pair.mqttHost),'MQTT port: 8883','Transport: MQTT over TLS 1.2',('Device ID: '+$pair.deviceId),('MQTT username: '+$pair.mqttUser),('MQTT password: '+$pair.mqttPassword),('CA store name: '+$pair.caName),('Public CA file: '+$caPath),('CA SHA-256: '+$pair.caSha256),('Modem UART baud: '+$baud),('Firmware settings file: '+$path),'','BROWSER LOGIN (separate from MQTT)',('Console URL: '+$pair.webUrl),('Browser username: '+$pair.webUser),('Browser password: '+$pair.webPassword))
    [IO.File]::WriteAllText($report,($lines -join "`r`n")+"`r`n",(New-Object Text.UTF8Encoding $false));Protect-FieldFile $report
    Write-Host "Firmware LTE settings ready: $path" -ForegroundColor Green
}
function ConvertTo-FieldShell([string]$Value){return "'"+$Value.Replace("'","'"+'"'+"'"+'"'+"'")+"'"}
function Invoke-FieldNative([string]$Executable,[string[]]$Arguments,[switch]$Capture){
    if($Capture){$output=(& $Executable @Arguments | Out-String);if($LASTEXITCODE -ne 0){throw 'Remote command failed. Check SSH access, sudo permissions and the message above.'};return $output}
    & $Executable @Arguments
    if($LASTEXITCODE -ne 0){throw 'Command failed. Check the message above; setup can be rerun.'}
}

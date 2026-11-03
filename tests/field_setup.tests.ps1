#requires -Version 5.1
# No SSH, package installs, serial access, firmware compile or hardware writes.
$ErrorActionPreference='Stop'
$repository=Split-Path -Parent $PSScriptRoot
. (Join-Path $repository 'tools\field_setup.ps1')
. (Join-Path $repository 'tools\modem_provision.ps1')
$testRoot=Join-Path ([IO.Path]::GetTempPath()) ('field-setup-tests-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
function Assert($Condition,[string]$Label){if(-not $Condition){throw $Label}}
function Fails([scriptblock]$Work,[string]$Pattern){try{& $Work | Out-Null}catch{if($_.Exception.Message -match $Pattern){return};throw};throw "Expected failure: $Pattern"}
function New-PairingFixture {
    $rsa=New-Object Security.Cryptography.RSACryptoServiceProvider 2048;$rsa.PersistKeyInCsp=$false
    $request=New-Object Security.Cryptography.X509Certificates.CertificateRequest('CN=Field Setup Test',$rsa,[Security.Cryptography.HashAlgorithmName]::SHA256,[Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $request.CertificateExtensions.Add((New-Object Security.Cryptography.X509Certificates.X509BasicConstraintsExtension($true,$false,0,$true)))
    $certificate=$request.CreateSelfSigned([DateTimeOffset]::UtcNow.AddMinutes(-2),[DateTimeOffset]::UtcNow.AddYears(1))
    $der=$certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert)
    $sha=[Security.Cryptography.SHA256]::Create();$hash=([BitConverter]::ToString($sha.ComputeHash($der))).Replace('-','').ToLowerInvariant();$sha.Dispose();$certificate.Dispose();$rsa.Dispose()
    $pem="-----BEGIN CERTIFICATE-----`n"+[Convert]::ToBase64String($der,[Base64FormattingOptions]::InsertLineBreaks).Replace("`r`n","`n")+"`n-----END CERTIFICATE-----`n"
    [pscustomobject]@{version=1;deviceId='s3-controller';mqttHost='mqtt.example.com';mqttPort=8883;mqttUser='s3-controller';mqttPassword='private-test-mqtt-password';caName='relay-ca';caPem=$pem;caSha256=$hash;webUrl='https://control.example.com';webUser='field';webPassword='private-test-browser-password';variant='s3-dispenser'}
}
function New-FakeModem($Pair){
    $der=[Convert]::FromBase64String((($Pair.caPem -replace '-----BEGIN CERTIFICATE-----|-----END CERTIFICATE-----','') -replace '\s',''))
    $md5=[Security.Cryptography.MD5]::Create();$expected=([BitConverter]::ToString($md5.ComputeHash($der))).Replace('-','').ToLowerInvariant();$md5.Dispose()
    $fake=[pscustomobject]@{Queue=(New-Object 'Collections.Generic.Queue[string]');Commands=(New-Object 'Collections.Generic.List[string]');Raw=(New-Object 'Collections.Generic.List[object]');Expected=$expected;Stored='';Model='SARA-R410M-02B';Clock='';RejectTls=$false;BadFingerprint=$false}
    $fake | Add-Member ScriptMethod ReadExisting {if($this.Queue.Count){return $this.Queue.Dequeue()};return ''}
    $fake | Add-Member ScriptMethod Write {
        param($value,$offset,$count)
        if($value -is [byte[]]){
            $copy=New-Object byte[] $count;[Array]::Copy($value,$offset,$copy,0,$count);$this.Raw.Add($copy);$this.Stored=$this.Expected
            if($this.BadFingerprint){$this.Stored='0'*32}
            $this.Queue.Enqueue("`r`nOK`r`n");return
        }
        $command=([string]$value).Trim();$this.Commands.Add($command)
        $reply="`r`nOK`r`n"
        if($command -eq 'AT+CGMM'){$reply="`r`n$($this.Model)`r`nOK`r`n"}
        elseif($command -eq 'AT+USECMNG=4,0,"relay-ca"'){
            $reply="`r`nERROR`r`n";if($this.Stored){$reply="`r`n+USECMNG: 4,0,`"relay-ca`",`"$($this.Stored)`"`r`nOK`r`n"}
        }elseif($command -like 'AT+USECMNG=0,*'){$reply="`r`n>"}
        elseif($command -like 'AT+CCLK=*'){$this.Clock=$command.Substring(8)}
        elseif($command -eq 'AT+CCLK?'){$reply="`r`n+CCLK: $($this.Clock)`r`nOK`r`n"}
        elseif($this.RejectTls -and $command -eq 'AT+USECPRF=0,0,3'){$reply="`r`nERROR`r`n"}
        # Fragment every response to exercise prompt/body boundaries.
        $split=[math]::Max(1,[int]($reply.Length/2));$this.Queue.Enqueue($reply.Substring(0,$split));$this.Queue.Enqueue($reply.Substring($split))
    }
    return $fake
}
try{
    $pair=New-PairingFixture;$null=Get-FieldPairing $pair
    $bad=$pair | ConvertTo-Json | ConvertFrom-Json;$bad.caSha256='0'*64;Fails {Get-FieldPairing $bad} 'CA validation'
    $bad=$pair | ConvertTo-Json | ConvertFrom-Json;$bad.webUrl='http://control.example.com';Fails {Get-FieldPairing $bad} 'HTTPS'
    Assert (-not(Test-FieldDomain 'example.com;touch')) 'Domain validation must reject shell injection'
    Fails {Get-FieldRemoteCommands '/tmp/bad;touch' 'control.example.com' 'mqtt.example.com' 's3-controller' 's3-dispenser'} 'Invalid remote'
    $remote=Get-FieldRemoteCommands ('/tmp/esp32-field-'+('a'*32)+'.tgz') 'control.example.com' 'mqtt.example.com' 's3-controller' 's3-dispenser'
    Assert ($remote.Deploy -match 'sudo -n bash' -and $remote.Deploy -match 'install.sh') 'Remote command must run the dependency installer'
    $rootRemote=Get-FieldRemoteCommands ('/tmp/esp32-field-'+('a'*32)+'.tgz') 'control.example.com' 'mqtt.example.com' 's3-controller' 's3-dispenser' $true
    Assert ($rootRemote.Deploy -notmatch 'sudo' -and $rootRemote.Pairing -notmatch 'sudo') 'Root SSH must work without a sudo package'

    $profile=[pscustomobject]@{variant='s3-dispenser';apn='test.apn';apnUser='';apnPassword='';pairing=$pair;modemBaud=9600;modemProvisioned=$false}
    Save-FieldProfile $profile $testRoot
    $protected=Get-FieldProfilePath $testRoot $profile.variant
    Assert ([Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($protected)) -notmatch 'private-test-mqtt-password|BEGIN CERTIFICATE') 'Pairing must not be stored as plaintext'
    $restored=Read-FieldProfile $testRoot $profile.variant
    Assert ($restored.pairing.mqttPassword -eq $pair.mqttPassword) 'Protected credentials must round-trip'
    $fixture=Join-Path $testRoot 'repository';$config=Join-Path $fixture 'ESP32S3_Dispenser_Controller\src\config'
    New-Item -ItemType Directory -Path $config -Force | Out-Null
    $header=Join-Path $config 'LtePrivate.h';[IO.File]::WriteAllText($header,'#define OTHER_SETTING 123')
    Write-FieldFirmwareSettings $profile $testRoot $fixture
    $text=[IO.File]::ReadAllText($header)
    Assert ($text -match 'OTHER_SETTING 123' -and $text -match 'LTE_UART_BAUD 9600' -and $text -match 'LTE_BROKER_HOST "mqtt.example.com"') 'Generated settings must preserve unrelated macros and include detected baud'
    Assert ((Get-Acl -LiteralPath $header).AreAccessRulesProtected) 'Private header must have restricted inherited access'
    $report=Join-Path $testRoot 'field\s3-dispenser-credentials.txt'
    $reportText=[IO.File]::ReadAllText($report)
    Assert ($reportText -match 'MQTT username: s3-controller' -and $reportText -match 'MQTT password: private-test-mqtt-password' -and $reportText -match 'MQTT port: 8883' -and $reportText -match 'CA store name: relay-ca') 'LTE credential report must contain all module connection values'
    Assert ($reportText.IndexOf('BROWSER LOGIN') -gt $reportText.IndexOf('MQTT password:') -and (Get-Acl -LiteralPath $report).AreAccessRulesProtected) 'Browser login must be separate and the report restricted to the account'
    $backups=@(Get-ChildItem -LiteralPath (Join-Path $testRoot 'field\backups') -File)
    Write-FieldFirmwareSettings $profile $testRoot $fixture
    Assert (@(Get-ChildItem -LiteralPath (Join-Path $testRoot 'field\backups') -File).Count -eq $backups.Count) 'Identical rerun must not create another secret backup'
    $bytes=[Text.Encoding]::UTF8.GetBytes($pair.caPem)
    $fake=New-FakeModem $pair;Provision-FieldModem $fake $pair
    Assert ($fake.Raw.Count -eq 1 -and [Convert]::ToBase64String($fake.Raw[0]) -eq [Convert]::ToBase64String($bytes)) 'Certificate transfer must preserve exact UTF-8 bytes'
    Assert ($fake.Commands -contains ('AT+USECMNG=0,0,"relay-ca",'+$bytes.Length)) 'Transfer byte count must be exact'
    Provision-FieldModem $fake $pair
    Assert ($fake.Raw.Count -eq 1) 'An identical stored CA must not be uploaded again'
    $wrong=New-FakeModem $pair;$wrong.Model='ESP32-S3';Fails {Provision-FieldModem $wrong $pair} 'not an identified SARA-R4'
    Assert ($wrong.Raw.Count -eq 0 -and $wrong.Commands.Count -eq 2) 'Wrong serial device must receive identification only'
    $badStore=New-FakeModem $pair;$badStore.BadFingerprint=$true;Fails {Provision-FieldModem $badStore $pair} 'fingerprint did not match'
    $badTls=New-FakeModem $pair;$badTls.RejectTls=$true;Fails {Provision-FieldModem $badTls $pair} 'rejected a provisioning command'
    Assert ($badTls.Commands -notcontains 'AT+USECPRF=0,0,0') 'TLS verification must never be downgraded'
    $quiet=New-FakeModem $pair;Fails {Wait-FieldModem $quiet -TimeoutMs 25} 'timed out'
    $archive=Join-Path $testRoot 'server.tgz';New-FieldServerArchive $archive $repository
    $listing=(& tar.exe -tzf $archive | Out-String)
    Assert ($LASTEXITCODE -eq 0 -and $listing -match 'relay/install.sh' -and $listing -match 'relay/export_settings.py') 'Deployment archive must contain the automated installer'
    Assert ($listing -notmatch 'secrets/|\.env|LtePrivate|node_modules|\.protected') 'Deployment archive must exclude private data and dependency caches'
    $dnsRecords=@(Get-FieldDnsRecords 'control.example.com' 'mqtt.example.com' '192.0.2.10')
    Show-FieldDnsPlan $dnsRecords $testRoot 's3-dispenser'
    Assert ([IO.File]::ReadAllText((Join-Path $testRoot 'field\s3-dispenser-dns.zone')) -match 'mqtt.example.com. 300 IN A 192.0.2.10') 'DNS zone export must contain the exact public A records'
    $script:FakeDns=@{};$script:DnsWrites=0
    function Invoke-FieldDnsApi([string]$Method,[string]$Path,[string]$Token,$Body){
        if($Path.StartsWith('zones?')){return [pscustomobject]@{success=$true;result=@([pscustomobject]@{name='example.com';id=('a'*32)})}}
        if($Method -eq 'GET'){
            $name=[uri]::UnescapeDataString(($Path -split 'name=')[1]);$items=@();if($script:FakeDns.ContainsKey($name)){$items=@($script:FakeDns[$name])}
            return [pscustomobject]@{success=$true;result=$items}
        }
        Assert ($Method -in @('PUT','POST') -and $Body.type -eq 'A' -and $Body.proxied -eq $false) 'DNS automation must create/update only DNS-only A records'
        $script:DnsWrites++;$script:FakeDns[$Body.name]=[pscustomobject]@{id=('b'*32);content=$Body.content;proxied=$false;ttl=$Body.ttl}
        return [pscustomobject]@{success=$true;result=@()}
    }
    Set-FieldCloudflareDns 'example.com' $dnsRecords 'not-a-real-token'
    Assert ($script:DnsWrites -eq 2) 'Initial DNS setup must create both records'
    Set-FieldCloudflareDns 'example.com' $dnsRecords 'not-a-real-token'
    Assert ($script:DnsWrites -eq 2) 'Matching DNS reruns must not rewrite records'
    $script:FakeDns['mqtt.example.com'].proxied=$true
    Set-FieldCloudflareDns 'example.com' $dnsRecords 'not-a-real-token'
    Assert ($script:DnsWrites -eq 3) 'MQTT DNS automation must disable an existing HTTP proxy flag'
    Fails {Set-FieldCloudflareDns 'different.example' $dnsRecords 'not-a-real-token'} 'outside the selected zone'
    # Exercise the complete wizard with stand-ins at all OS/network boundaries.
    $script:FieldRoot=Join-Path $testRoot 'wizard-repository'
    New-Item -ItemType Directory -Path (Join-Path $script:FieldRoot 'ESP32S3_Dispenser_Controller\src\config') -Force | Out-Null
    $ToolsDirectory=Join-Path $testRoot 'wizard-tools';$Variant='s3-dispenser';$Server='192.0.2.10';$SshUser='ubuntu';$SshPort=22
    $WebDomain='control.example.com';$MqttDomain='mqtt.example.com';$DeviceId='s3-controller';$Apn='test.apn';$SkipModem=[switch]$true;$BuildAndFlash=[switch]$false;$DryRun=[switch]$false
    $script:WizardOperations=New-Object 'Collections.Generic.List[string]'
    function Resolve-FieldAddress([string]$Name){return '192.0.2.10'}
    function Ensure-FieldWindowsTools {$script:WizardOperations.Add('windows-tools')}
    function New-FieldServerArchive([string]$Destination){[IO.File]::WriteAllText($Destination,'mock-public-archive')}
    function Invoke-FieldNative([string]$Executable,[string[]]$Arguments,[switch]$Capture){$script:WizardOperations.Add($Executable+' '+($Arguments -join ' '));$global:LASTEXITCODE=0;if($Capture){return ($pair | ConvertTo-Json -Depth 8)}}
    function Invoke-FieldController([string[]]$Arguments){$script:WizardOperations.Add('controller '+($Arguments -join ' '));$global:LASTEXITCODE=0}
    function Read-Host([string]$Prompt){return 'n'}
    Invoke-FieldSetup
    Assert (@($script:WizardOperations | Where-Object {$_ -match 'controller setup -Lte'}).Count -eq 1) 'Wizard must install local Arduino LTE dependencies'
    Assert (@($script:WizardOperations | Where-Object {$_ -match 'install.sh'}).Count -eq 1) 'Wizard must deploy through the server dependency installer'
    Assert (@($script:WizardOperations | Where-Object {$_ -match 'controller compile|controller flash'}).Count -eq 0) 'Declining build must never compile or flash'
    $saved=Read-FieldProfile $ToolsDirectory $Variant
    Assert ($saved.pairing.caSha256 -eq $pair.caSha256 -and $saved.server -eq $Server) 'Wizard must save matching protected settings for resume'
    $ToolsDirectory=Join-Path $testRoot 'single-domain-tools';$Domain='example.com';$WebDomain='';$MqttDomain=''
    Invoke-FieldSetup
    $single=Read-FieldProfile $ToolsDirectory $Variant
    Assert ($single.webDomain -eq 'control.example.com' -and $single.mqttDomain -eq 'mqtt.example.com') 'One domain must derive both hostnames without extra hostname prompts'
    Write-Output 'PASS protected pairing/header backups, CA validation, raw modem transfer/readback, wrong-device refusal, clock/TLS checks, idempotent CA setup and public-only deployment archive. No serial/network/compiler use.'
}finally{
    $full=[IO.Path]::GetFullPath($testRoot);$parent=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
    if(-not $full.StartsWith($parent,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($full) -notmatch '^field-setup-tests-[a-f0-9]{32}$'){throw 'Unexpected test cleanup path.'}
    Remove-Item -LiteralPath $full -Recurse -Force
}

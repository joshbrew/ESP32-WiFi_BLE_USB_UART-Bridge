#requires -Version 5.1
function Get-FieldDnsRecords([string]$Web,[string]$Mqtt,[string]$Address){
    $parsed=$null
    if(-not(Test-FieldDomain $Web) -or -not(Test-FieldDomain $Mqtt) -or -not[Net.IPAddress]::TryParse($Address,[ref]$parsed) -or $parsed.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork){throw 'DNS setup requires two hostnames and the host public IPv4 address.'}
    foreach($name in @($Web,$Mqtt) | Select-Object -Unique){[pscustomobject]@{Type='A';Name=$name;Content=$Address;TTL=300;Proxy='DNS only'}}
}
function Show-FieldDnsPlan($Records,[string]$Tools,[string]$Name){
    Write-Host "`nAdd these DNS records at your domain provider:" -ForegroundColor Cyan
    $Records | Format-Table Type,Name,Content,TTL,Proxy -AutoSize | Out-Host
    $path=Join-Path $Tools "field\$Name-dns.zone"
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    $lines=@('; ESP32 field access: A records only; proxy disabled; TTL 300 seconds.')
    foreach($record in $Records){$lines+=$record.Name+'. 300 IN A '+$record.Content+' ; cf_tags=cf-proxied:false'}
    [IO.File]::WriteAllText($path,($lines -join "`n")+"`n",(New-Object Text.UTF8Encoding $false))
    Write-Host "Importable DNS zone file: $path"
    Write-Host 'Use the full Name when your provider accepts it; otherwise use control/mqtt within your domain. Correct conflicting AAAA records if this host has no matching IPv6 address.'
}
function Invoke-FieldDnsApi([string]$Method,[string]$Path,[string]$Token,$Body){
    [Net.ServicePointManager]::SecurityProtocol=[Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $parameters=@{Uri=('https://api.cloudflare.com/client/v4/'+$Path);Method=$Method;Headers=@{Authorization=('Bearer '+$Token)};ErrorAction='Stop'}
    if($Body){$parameters.ContentType='application/json';$parameters.Body=($Body | ConvertTo-Json -Compress)}
    try{$response=Invoke-RestMethod @parameters}catch{throw 'Cloudflare DNS request failed. Check the domain and token permissions (Zone Read + DNS Edit for this zone).'}
    if($response.success -ne $true){throw 'Cloudflare rejected the DNS request. Existing records were not deleted.'}
    return $response
}
function Set-FieldCloudflareDns([string]$Domain,$Records,[string]$Token){
    if(-not(Test-FieldDomain $Domain) -or -not $Token){throw 'Cloudflare needs your domain and a scoped API token.'}
    foreach($record in $Records){if($record.Name -ne $Domain -and -not $record.Name.EndsWith('.'+$Domain,[StringComparison]::OrdinalIgnoreCase)){throw 'Requested DNS name is outside the selected zone.'}}
    $response=Invoke-FieldDnsApi 'GET' ('zones?name='+[uri]::EscapeDataString($Domain)+'&status=active') $Token $null
    $zones=@($response.result | Where-Object {$_.name -eq $Domain})
    if($zones.Count -ne 1 -or $zones[0].id -notmatch '^[a-f0-9]{32}$'){throw 'Cloudflare zone not found. Use the registered zone name and a token scoped to it.'}
    $zone=$zones[0].id
    foreach($record in $Records){
        $path='zones/'+$zone+'/dns_records'
        $found=Invoke-FieldDnsApi 'GET' ($path+'?type=A&name='+[uri]::EscapeDataString($record.Name)) $Token $null
        $existing=@($found.result)
        if($existing.Count -gt 1){throw 'Multiple A records exist for a requested hostname. Resolve those records or choose unused control/mqtt hostnames.'}
        $body=@{type='A';name=$record.Name;content=$record.Content;ttl=300;proxied=$false}
        if($existing.Count -eq 1){
            if($existing[0].content -eq $record.Content -and $existing[0].proxied -eq $false){continue}
            if($existing[0].id -notmatch '^[a-f0-9]{32}$'){throw 'Invalid Cloudflare record identifier.'}
            $null=Invoke-FieldDnsApi 'PUT' ($path+'/'+$existing[0].id) $Token $body
        }else{$null=Invoke-FieldDnsApi 'POST' $path $Token $body}
    }
    Write-Host 'Cloudflare DNS-only A records are ready. DNS caches may take a few minutes to update.' -ForegroundColor Green
}
function Test-FieldDnsReady($Records){
    foreach($record in $Records){
        try{$addresses=@(Resolve-FieldAddress $record.Name)}catch{return $false}
        if($addresses -notcontains $record.Content){return $false}
    };return $true
}

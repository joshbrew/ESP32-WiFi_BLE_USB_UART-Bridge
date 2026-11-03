function New-FieldServerArchive([string]$Destination,[string]$Repository=$script:FieldRoot){
    $generic='ESP32S3_Modular_Command_Transport_Controller';$dispenser='ESP32S3_Dispenser_Controller'
    $files=@("$generic/.dockerignore")
    foreach($folder in @($generic,$dispenser)){foreach($name in @('index.html','app.css','app.js')){$files+="$folder/web/$name"}}
    foreach($name in @('server.mjs','core.mjs','broker-health.mjs','healthcheck.mjs','package.json','package-lock.json','Dockerfile','compose.yaml','Caddyfile','mosquitto.conf','setup.sh','install.sh','export_settings.py','home_prepare.py')){$files+="$generic/relay/$name"}
    $stage=Join-Path (Split-Path -Parent $Destination) ('package-'+[guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    try{
        foreach($file in $files){
            $source=Join-Path $Repository $file;$target=Join-Path $stage $file
            if(-not(Test-Path -LiteralPath $source -PathType Leaf)){throw "Missing deployment source: $file"}
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            # Normalize text for Linux; copy only this public-source whitelist.
            [IO.File]::WriteAllText($target,[IO.File]::ReadAllText($source).Replace("`r`n","`n"),(New-Object Text.UTF8Encoding $false))
        }
        Invoke-FieldNative 'tar.exe' (@('-czf',$Destination,'-C',$stage)+$files)
    }finally{
        $resolved=[IO.Path]::GetFullPath($stage);$parent=[IO.Path]::GetFullPath((Split-Path -Parent $Destination)).TrimEnd('\')+'\'
        if(-not $resolved.StartsWith($parent,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolved) -notmatch '^package-[a-f0-9]{32}$'){throw 'Unexpected package cleanup path.'}
        if(Test-Path -LiteralPath $resolved){Remove-Item -LiteralPath $resolved -Recurse -Force}
    }
}

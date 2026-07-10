[CmdletBinding()]
param(
    [string]$ArduinoLibraries = (Join-Path $env:USERPROFILE 'Documents\Arduino\libraries')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$packages = @(
    @{
        Name = 'Async TCP'
        Version = '3.4.10'
        Url = 'https://github.com/ESP32Async/AsyncTCP/archive/refs/tags/v3.4.10.zip'
        Destination = 'AsyncTCP'
        Header = 'src\AsyncTCP.h'
        VersionFile = 'src\AsyncTCPVersion.h'
        VersionPattern = 'ASYNCTCP_VERSION_PATCH 10'
        OldFolders = @('AsyncTCP', 'Async_TCP', 'Async-TCP')
    },
    @{
        Name = 'ESP Async WebServer'
        Version = '3.11.2'
        Url = 'https://github.com/ESP32Async/ESPAsyncWebServer/archive/refs/tags/v3.11.2.zip'
        Destination = 'ESP_Async_WebServer'
        Header = 'src\ESPAsyncWebServer.h'
        VersionFile = 'src\AsyncWebServerVersion.h'
        VersionPattern = 'ASYNCWEBSERVER_VERSION_PATCH 2'
        OldFolders = @('ESP_Async_WebServer', 'ESPAsyncWebServer', 'ESP-Async-WebServer')
    }
)

Write-Host ''
Write-Host 'ESP32 async library repair' -ForegroundColor Cyan
Write-Host 'Close Arduino IDE before continuing.' -ForegroundColor Yellow
Write-Host "Library folder: $ArduinoLibraries"
Write-Host ''

New-Item -ItemType Directory -Force -Path $ArduinoLibraries | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) "esp32_async_fix_$stamp"
New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null

try {
    foreach ($package in $packages) {
        Write-Host "Installing $($package.Name) $($package.Version)..." -ForegroundColor Cyan

        foreach ($folderName in $package.OldFolders) {
            $existing = Join-Path $ArduinoLibraries $folderName
            if (Test-Path $existing) {
                $backup = "$existing.backup_$stamp"
                Write-Host "  Backing up $existing"
                Move-Item -Force $existing $backup
            }
        }

        $zipPath = Join-Path $tempRoot "$($package.Destination).zip"
        $extractPath = Join-Path $tempRoot "$($package.Destination)_extract"
        Invoke-WebRequest -UseBasicParsing -Uri $package.Url -OutFile $zipPath
        Expand-Archive -Force -Path $zipPath -DestinationPath $extractPath

        $sourceDir = Get-ChildItem -Path $extractPath -Directory | Select-Object -First 1
        if ($null -eq $sourceDir) {
            throw "Downloaded archive for $($package.Name) did not contain a library folder."
        }

        $destination = Join-Path $ArduinoLibraries $package.Destination
        Copy-Item -Recurse -Force $sourceDir.FullName $destination

        $header = Join-Path $destination $package.Header
        $versionFile = Join-Path $destination $package.VersionFile
        if (!(Test-Path $header) -or !(Test-Path $versionFile)) {
            throw "$($package.Name) installation is incomplete at $destination"
        }

        $versionText = Get-Content -Raw $versionFile
        if ($versionText -notmatch [regex]::Escape($package.VersionPattern)) {
            throw "$($package.Name) version verification failed at $versionFile"
        }

        Write-Host "  Installed at $destination" -ForegroundColor Green
    }

    $asyncTcpHeader = Join-Path $ArduinoLibraries 'AsyncTCP\src\AsyncTCP.h'
    $headerText = Get-Content -Raw $asyncTcpHeader
    if ($headerText -notmatch 'uint8_t\s+status\(\)\s+const\s*;') {
        throw 'AsyncTCP verification failed: AsyncServer::status() is not const.'
    }

    Write-Host ''
    Write-Host 'Libraries installed and verified.' -ForegroundColor Green
    Write-Host 'Restart Arduino IDE, select the LOLIN32 board, and compile again.' -ForegroundColor Green
    Write-Host 'Old installations were retained as .backup_<timestamp> folders.' -ForegroundColor DarkGray
}
finally {
    if (Test-Path $tempRoot) {
        Remove-Item -Recurse -Force $tempRoot
    }
}

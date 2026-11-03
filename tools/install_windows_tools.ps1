#requires -Version 5.1
#requires -RunAsAdministrator
$ErrorActionPreference='Stop'
try{
    $capability=Get-WindowsCapability -Online -Name 'OpenSSH.Client~~~~0.0.1.0'
    if($capability.State -ne 'Installed'){
        $result=Add-WindowsCapability -Online -Name 'OpenSSH.Client~~~~0.0.1.0'
        if($result.RestartNeeded){Write-Host 'Windows needs a restart to finish OpenSSH installation. Restart, then rerun Setup Field Access.cmd.';exit 3010}
    }
    exit 0
}catch{Write-Host 'Windows OpenSSH installation failed. Check Windows Update access and rerun setup.';exit 1}

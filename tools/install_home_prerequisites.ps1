#requires -Version 5.1
#requires -RunAsAdministrator
[CmdletBinding()]
param([ValidateSet('Wsl','Firewall')][string]$Task='Wsl')
$ErrorActionPreference='Stop'
try {
    if($Task -eq 'Wsl'){
        $restart=$false
        foreach($feature in @('Microsoft-Windows-Subsystem-Linux','VirtualMachinePlatform')){
            if((Get-WindowsOptionalFeature -Online -FeatureName $feature).State -ne 'Enabled'){
                $result=Enable-WindowsOptionalFeature -Online -FeatureName $feature -All -NoRestart
                $restart=$true
            }
        }
        if($restart){exit 3010}
        & wsl.exe --update
        if($LASTEXITCODE -ne 0){throw 'WSL update failed. Check Windows Update and hardware virtualization.'}
    }else{
        foreach($port in @(80,443,8883)){
            $name='ESP32FieldHome-TCP-'+$port
            # Only manage our named rules; do not disable Windows Firewall.
            if(Get-NetFirewallRule -Name $name -ErrorAction SilentlyContinue){Remove-NetFirewallRule -Name $name}
            New-NetFirewallRule -Name $name -DisplayName ('ESP32 home console TCP '+$port) -Direction Inbound -Action Allow -Protocol TCP -LocalPort $port -Profile Private | Out-Null
        }
    }
}catch{Write-Error $_;exit 1}

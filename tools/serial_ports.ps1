#requires -Version 5.1
# Enumerates device metadata only; discovery never opens or resets a serial port.
function ConvertTo-ControllerPorts($Devices) {
    $seen=@{}
    foreach($device in $Devices){
        $port=[string]$device.DeviceID
        if($port -notmatch '^COM[1-9][0-9]*$' -or $seen.ContainsKey($port.ToUpperInvariant())){continue}
        $port=$port.ToUpperInvariant();$seen[$port]=$true
        $name=[string]$device.Name;$id=[string]$device.PNPDeviceID
        if($id -match '^BTH' -or $name -match 'Bluetooth'){continue}
        $score=0;$reason='Unidentified serial port'
        if($id -match 'VID_303A'){$score=90;$reason='Espressif USB device'}
        elseif($name -match 'Feather'){$score=85;$reason='Feather label; exact board model unverified'}
        elseif($id -match 'VID_239A'){$score=80;$reason='Adafruit USB device; exact board model unverified'}
        elseif($id -match 'VID_10C4|VID_1A86|VID_0403'){$score=60;$reason='USB serial bridge; attached board model unverified'}
        elseif($id -match '^USB'){$score=20;$reason='USB serial device; attached board model unverified'}
        [pscustomobject]@{Port=$port;Name=$name;Reason=$reason;Score=$score;HardwareId=$id}
    }
}
function Get-ControllerPorts {
    $devices=@();$names=@([IO.Ports.SerialPort]::GetPortNames());$bluetooth=@()
    # Registry metadata works even when Windows Management Instrumentation is
    # unavailable to the shell. Filter ghost device records against live COM names.
    try{
        $map=Get-ItemProperty -LiteralPath 'HKLM:\HARDWARE\DEVICEMAP\SERIALCOMM' -ErrorAction Stop
        $bluetooth=@($map.PSObject.Properties | Where-Object Name -Match 'Bth|Bluetooth' | ForEach-Object Value)
    }catch{Write-Verbose 'Serial device map unavailable.'}
    foreach($bus in @('USB','FTDIBUS')){
        $root="HKLM:\SYSTEM\CurrentControlSet\Enum\$bus"
        foreach($model in @(Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue)){
            foreach($instance in @(Get-ChildItem -LiteralPath $model.PSPath -ErrorAction SilentlyContinue)){
                $parameters=Get-ItemProperty -LiteralPath ($instance.PSPath+'\Device Parameters') -ErrorAction SilentlyContinue
                if(-not $parameters -or $names -notcontains $parameters.PortName){continue}
                $description=Get-ItemProperty -LiteralPath $instance.PSPath -ErrorAction SilentlyContinue
                $label=[string]$description.FriendlyName;if(-not $label){$label="USB serial device ($($parameters.PortName))"}
                $devices+=[pscustomobject]@{DeviceID=$parameters.PortName;Name=$label;PNPDeviceID="$bus\$($model.PSChildName)\$($instance.PSChildName)"}
            }
        }
    }
    if(-not $devices.Count){try{$devices=@(Get-CimInstance Win32_SerialPort -OperationTimeoutSec 5 -ErrorAction Stop)}catch{Write-Verbose 'Windows device descriptions unavailable; using serial port names.'}}
    $known=@($devices | ForEach-Object {$_.DeviceID})
    foreach($name in $names){
        if($known -notcontains $name -and $bluetooth -notcontains $name){$devices+=[pscustomobject]@{DeviceID=$name;Name='Serial port (description unavailable)';PNPDeviceID=''}}
    }
    @(ConvertTo-ControllerPorts $devices | Sort-Object @{Expression='Score';Descending=$true},@{Expression={ [int]($_.Port -replace '\D','') }})
}
function Select-ControllerPort([string]$RequestedPort) {
    if($RequestedPort -and $RequestedPort.Trim() -notmatch '^COM[1-9][0-9]*$'){throw 'Use a Windows serial port such as COM24.'}
    $ports=@(Get-ControllerPorts)
    if($ports.Count -eq 0){throw 'No usable COM ports found. Connect the board with a data cable, check its USB driver, or use BOOT/RESET for S3 recovery.'}
    if($RequestedPort){
        $match=@($ports | Where-Object Port -eq $RequestedPort.Trim().ToUpperInvariant())
        if($match.Count -ne 1){throw "Port $RequestedPort is not present or is a Bluetooth port. Run controller ports and choose the connected board."}
        return $match[0]
    }
    $candidates=@($ports | Where-Object Score -gt 0)
    if($candidates.Count -eq 1){Write-Host "Found $($candidates[0].Port): $($candidates[0].Name)" -ForegroundColor Cyan;return $candidates[0]}
    Write-Host 'Select the connected board (USB descriptions do not prove its model):'
    for($i=0;$i -lt $ports.Count;$i++){Write-Host ('  {0}. {1} - {2} [{3}]' -f ($i+1),$ports[$i].Port,$ports[$i].Name,$ports[$i].Reason)}
    $choice=Read-Host 'Port number, or Q to cancel'
    if($choice -match '^(?i:q)$'){return $null}
    if($choice -notmatch '^\d+$' -or [int]$choice -lt 1 -or [int]$choice -gt $ports.Count){throw 'Choose a listed port number.'}
    return $ports[[int]$choice-1]
}
function Confirm-ControllerFlash($Selection,[string]$Sketch) {
    Write-Host "`nFirmware: $Sketch"
    Write-Host "Port: $($Selection.Port) - $($Selection.Name)"
    Write-Host "Identification: $($Selection.Reason)"
    return (Read-Host 'Is this the correct board and port? Flash now? [y/N]') -match '^(?i:y|yes)$'
}

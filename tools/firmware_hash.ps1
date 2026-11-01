#requires -Version 5.1
# Avoid module auto-loading differences in nested Windows PowerShell launchers.
function Get-ControllerHash([string]$LiteralPath){
    $stream=[IO.File]::OpenRead($LiteralPath)
    $sha=[Security.Cryptography.SHA256]::Create()
    try{return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','')}
    finally{$sha.Dispose();$stream.Dispose()}
}

[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\Nero\bin'),
    [ValidateSet('User', 'Machine', 'Process')]
    [string]$PathScope = 'User'
)
$ErrorActionPreference = 'Stop'
if ($PathScope -eq 'Machine') {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Machine removal requires an Administrator PowerShell.'
    }
    if (-not $PSBoundParameters.ContainsKey('InstallDir')) {
        $InstallDir = Join-Path $env:ProgramFiles 'Nero\bin'
    }
}
$destination = [IO.Path]::GetFullPath($InstallDir)
$target = Join-Path $destination 'nero.exe'
if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Force }
function Remove-PathEntry([string]$current, [string]$entry) {
    return (@($current -split ';' | Where-Object {
        $_ -and $_.TrimEnd('\') -ine $entry.TrimEnd('\')
    }) -join ';')
}
if ($PathScope -ne 'Process') {
    $current = [Environment]::GetEnvironmentVariable('Path', $PathScope)
    [Environment]::SetEnvironmentVariable('Path', (Remove-PathEntry $current $destination), $PathScope)
}
$env:Path = Remove-PathEntry $env:Path $destination
Write-Host "Removed $target (user history and logs retained)."

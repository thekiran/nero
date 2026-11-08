[CmdletBinding()]
param(
    [string]$BinaryPath = (Join-Path $PSScriptRoot 'bin\nero.exe'),
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\Nero\bin'),
    [ValidateSet('User', 'Machine', 'Process')]
    [string]$PathScope = 'User'
)
$ErrorActionPreference = 'Stop'
$source = (Resolve-Path -LiteralPath $BinaryPath).Path
$destination = [System.IO.Path]::GetFullPath($InstallDir)
if ($PathScope -eq 'Machine') {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Machine installation requires an Administrator PowerShell.'
    }
    # Use the protected Program Files location for machine-wide installation.
    if (-not $PSBoundParameters.ContainsKey('InstallDir')) {
        $destination = Join-Path $env:ProgramFiles 'Nero\bin'
    }
}
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$target = Join-Path $destination 'nero.exe'
if ($source -ne $target) { Copy-Item -LiteralPath $source -Destination $target -Force }
function Add-PathEntry([string]$current, [string]$entry) {
    $parts = @($current -split ';' | Where-Object { $_ })
    if ($parts.TrimEnd('\') -notcontains $entry.TrimEnd('\')) { $parts += $entry }
    return ($parts -join ';')
}
if ($PathScope -ne 'Process') {
    $current = [Environment]::GetEnvironmentVariable('Path', $PathScope)
    [Environment]::SetEnvironmentVariable('Path', (Add-PathEntry $current $destination), $PathScope)
    . (Join-Path $PSScriptRoot 'windows-env.ps1')
    Send-NeroEnvironmentChange
}
$env:Path = Add-PathEntry $env:Path $destination
Write-Host "Installed $target"
Write-Host 'Run nero from any directory. Open a new terminal for the saved PATH.'
Write-Host 'For elevated operations, open Windows Terminal as Administrator.'

# Default user install must resolve in fresh PowerShell and CMD processes from
# an unrelated directory. Runs only in disposable native Windows CI hosts.
param([Parameter(Mandatory=$true)][string]$BinaryPath)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$installer = Join-Path $root 'scripts\install.ps1'
$uninstaller = Join-Path $root 'scripts\uninstall.ps1'
$binary = (Resolve-Path -LiteralPath $BinaryPath).Path
$target = Join-Path $env:LOCALAPPDATA 'Programs\Nero\bin\nero.exe'
if (Test-Path -LiteralPath $target) { throw 'Disposable test host already has a user NERO installation' }
$previousUserPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$previousProcessPath = $env:Path
$temp = Join-Path $env:TEMP ('nero global install ' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
$env:NERO_GLOBAL_TARGET = $target
$env:NERO_LOG_FILE = ':memory:'
$env:NERO_HISTORY_FILE = ':memory:'
$env:NERO_ASCII = '1'
$env:NO_COLOR = '1'
try {
    & $installer -BinaryPath $binary
    if (-not $?) { throw 'Default user installation failed' }
    Push-Location $temp
    try {
        Set-Content -LiteralPath 'commands.txt' -Value @('version', 'exit') -Encoding ASCII
        $code = @'
$ErrorActionPreference = 'Stop'
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
$actual = (Get-Command nero -ErrorAction Stop).Source
if ([IO.Path]::GetFullPath($actual) -ine [IO.Path]::GetFullPath($env:NERO_GLOBAL_TARGET)) { throw 'Wrong global nero resolved' }
'version','exit' | & nero
if ($LASTEXITCODE -ne 0) { throw 'Fresh PowerShell could not run nero' }
& "$env:SystemRoot\System32\cmd.exe" /d /c 'nero < commands.txt'
if ($LASTEXITCODE -ne 0) { throw 'Fresh CMD could not run nero' }
'NERO_GLOBAL_INSTALL_OK'
'@
        $shell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
        $output = & $shell -NoProfile -NonInteractive -Command $code
        if ($LASTEXITCODE -ne 0 -or $output -notcontains 'NERO_GLOBAL_INSTALL_OK') {
            throw 'Global command resolution failed in fresh terminal processes'
        }
        $version = if ($env:NERO_EXPECTED_VERSION) { $env:NERO_EXPECTED_VERSION } else { 'development' }
        if (($output -join "`n") -notmatch [regex]::Escape("version $version")) { throw 'Global CLI version mismatch' }
        Write-Output 'PASS default user PATH installation: fresh PowerShell and CMD, unrelated directory'
    } finally { Pop-Location }
} finally {
    try { & $uninstaller } finally {
        [Environment]::SetEnvironmentVariable('Path', $previousUserPath, 'User')
        $env:Path = $previousProcessPath
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
if (Test-Path -LiteralPath $target) { throw 'Installed binary remains after uninstall' }

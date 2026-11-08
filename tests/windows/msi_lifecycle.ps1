# Exercise Windows Installer itself on a disposable administrator CI runner.
param(
    [Parameter(Mandatory=$true)][string]$MsiPath,
    [Parameter(Mandatory=$true)][string]$UpgradeMsiPath,
    [Parameter(Mandatory=$true)][string]$ExpectedVersion
)
$ErrorActionPreference = 'Stop'
$msi = (Resolve-Path -LiteralPath $MsiPath).Path
$upgrade = (Resolve-Path -LiteralPath $UpgradeMsiPath).Path
$target = Join-Path $env:ProgramFiles 'Nero\bin\nero.exe'
$directory = [IO.Path]::GetDirectoryName($target).TrimEnd('\')
if (Test-Path -LiteralPath $target) { throw 'Disposable runner already has a machine NERO installation' }
$previousPath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
$temp = Join-Path $env:TEMP ('nero msi test ' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$logs = Join-Path $root '.build\msi-test-logs'
New-Item -ItemType Directory -Path $logs -Force | Out-Null
function Invoke-Msi([string]$operation, [string]$package, [string]$log, [switch]$Cleanup) {
    $arguments = "$operation `"$package`" /qn /norestart /l*v `"$log`""
    $process = Start-Process -FilePath "$env:SystemRoot\System32\msiexec.exe" -ArgumentList $arguments -Wait -PassThru
    $allowed = if ($Cleanup) { @(0, 1605, 1614, 3010) } else { @(0, 3010) }
    if ($process.ExitCode -notin $allowed) {
        Get-Content -LiteralPath $log -Tail 60
        throw "Windows Installer failed: $($process.ExitCode)"
    }
}
$env:NERO_GLOBAL_TARGET = $target
$env:NERO_LOG_FILE = ':memory:'
$env:NERO_HISTORY_FILE = ':memory:'
$env:NO_COLOR = '1'
$env:NERO_ASCII = '1'
$env:NERO_EXPECTED_VERSION = $ExpectedVersion
try {
    foreach ($package in @($msi, $upgrade)) {
        Invoke-Msi '/i' $package (Join-Path $logs ('install-' + [Guid]::NewGuid().ToString('N') + '.log'))
        if (-not (Test-Path -LiteralPath $target)) { throw 'MSI did not install nero.exe' }
        $path = [Environment]::GetEnvironmentVariable('Path', 'Machine')
        if (@($path -split ';' | Where-Object { $_.TrimEnd('\') -ieq $directory }).Count -ne 1) {
            throw 'MSI did not register exactly one system PATH entry'
        }
        Push-Location $temp
        try {
            Set-Content -LiteralPath commands.txt -Value @('version','exit') -Encoding ASCII
            $code = @'
$ErrorActionPreference = 'Stop'
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
$resolved = (Get-Command nero -ErrorAction Stop).Source
if ([IO.Path]::GetFullPath($resolved) -ine [IO.Path]::GetFullPath($env:NERO_GLOBAL_TARGET)) { throw 'Wrong MSI-installed command resolved' }
'version','exit' | & nero
if ($LASTEXITCODE -ne 0) { throw 'MSI-installed CLI failed in PowerShell' }
& "$env:SystemRoot\System32\cmd.exe" /d /c 'nero < commands.txt'
if ($LASTEXITCODE -ne 0) { throw 'MSI-installed CLI failed in CMD' }
'MSI_COMMAND_OK'
'@
            $output = & "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -Command $code
            if ($LASTEXITCODE -ne 0 -or $output -notcontains 'MSI_COMMAND_OK' -or
                ($output -join "`n") -notmatch [regex]::Escape("version $ExpectedVersion")) {
                throw 'Fresh terminal could not resolve the MSI-installed command'
            }
        } finally { Pop-Location }
        Write-Output 'PASS MSI install/upgrade and command resolution from unrelated directory'
    }
    Invoke-Msi '/x' $upgrade (Join-Path $logs 'uninstall.log')
    if (Test-Path -LiteralPath $target) { throw 'MSI uninstall left nero.exe behind' }
    $path = [Environment]::GetEnvironmentVariable('Path','Machine')
    if (@($path -split ';' | Where-Object { $_.TrimEnd('\') -ieq $directory }).Count) {
        throw 'MSI uninstall left its system PATH entry behind'
    }
    $before = ($previousPath -split ';' | Where-Object { $_ }) -join ';'
    $after = ($path -split ';' | Where-Object { $_ }) -join ';'
    if ($before -cne $after) {
        throw 'MSI changed unrelated system PATH entries'
    }
    Write-Output 'PASS MSI removal and unrelated PATH preservation'
} finally {
    Invoke-Msi '/x' $upgrade (Join-Path $logs 'cleanup-upgrade.log') -Cleanup
    Invoke-Msi '/x' $msi (Join-Path $logs 'cleanup-original.log') -Cleanup
    Remove-Item -LiteralPath $temp -Recurse -Force
}

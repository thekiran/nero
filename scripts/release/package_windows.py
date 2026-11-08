"""Package the real, tested native Windows x86-64 CLI, with system DLLs only."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import struct
import subprocess
import tempfile
import zipfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--version', required=True)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--binary', required=True)
    parser.add_argument('--output', default='.build/release')
    args = parser.parse_args()
    if not re.fullmatch(r'v\d+\.\d+\.\d+(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?', args.version):
        parser.error('invalid version')
    if not re.fullmatch(r'[0-9a-f]{40}', args.commit):
        parser.error('commit must be a full SHA-1')
    if platform.system() != 'Windows' or platform.machine().lower() not in ('amd64', 'x86_64'):
        parser.error('must run on native Windows x86-64')
    binary = Path(args.binary)
    data = binary.read_bytes()
    if data[:2] != b'MZ':
        parser.error('not a Windows executable')
    offset = struct.unpack_from('<I', data, 0x3c)[0]
    if data[offset:offset + 4] != b'PE\0\0' or struct.unpack_from('<H', data, offset + 4)[0] != 0x8664:
        parser.error('not an x86-64 PE executable')
    imports = subprocess.check_output(['objdump', '-p', str(binary)], text=True)
    dlls = re.findall(r'DLL Name:\s+(\S+)', imports)
    system_dlls = {'kernel32.dll', 'msvcrt.dll', 'ucrtbase.dll', 'ws2_32.dll',
                   'iphlpapi.dll', 'advapi32.dll', 'user32.dll', 'shell32.dll',
                   'ole32.dll', 'bcrypt.dll', 'ntdll.dll'}
    if not dlls or any(d.lower() not in system_dlls and not d.lower().startswith('api-ms-win-') for d in dlls):
        parser.error(f'non-system DLL dependency in package: {dlls}')
    root = f'nero-{args.version}-windows-x86_64'
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    archive = output / f'{root}.zip'
    metadata = {'version': args.version, 'commit': args.commit,
                'platform': 'windows-x86_64', 'build': platform.platform(),
                'binary_sha256': hashlib.sha256(data).hexdigest(), 'runtime_dependencies': dlls,
                'raw_packet_engine': 'Unsupported', 'code_signed': False}
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as zip_file:
        zip_file.write(binary, f'{root}/bin/nero.exe')
        for source, destination in [('LICENSE.txt', 'LICENSE.txt'), ('README.md', 'README.md'), ('docs/releases.md', 'INSTALL.md'), ('scripts/install.ps1', 'install.ps1'), ('scripts/uninstall.ps1', 'uninstall.ps1')]:
            zip_file.write(source, f'{root}/{destination}')
        zip_file.writestr(f'{root}/BUILD-INFO.json', json.dumps(metadata, indent=2) + '\n')
    with zipfile.ZipFile(archive) as zip_file:
        if zip_file.testzip() is not None or zip_file.read(f'{root}/bin/nero.exe') != data:
            raise RuntimeError('ZIP contents verification failed')
        # Execute outside the build tree with a system-only PATH: MSYS2 DLLs
        # cannot accidentally hide a missing runtime dependency.
        with tempfile.TemporaryDirectory() as temp:
            zip_file.extractall(temp)
            import os
            env = dict(os.environ, PATH=str(Path(os.environ['SystemRoot']) / 'System32'),
                       NO_COLOR='1', NERO_ASCII='1', TERM='dumb')
            result = subprocess.run([str(Path(temp) / root / 'bin/nero.exe')],
                                    input=b'version\nexit\n', capture_output=True,
                                    timeout=15, env=env, cwd=temp)
            if result.returncode != 0 or f'version {args.version}'.encode() not in result.stdout:
                raise RuntimeError(f'extracted standalone CLI failed: {result.returncode}: {result.stderr!r}')
            # Test command installation from an unrelated working directory.
            import shutil
            powershell = shutil.which('powershell.exe')
            if not powershell:
                raise RuntimeError('PowerShell is required to verify the installer')
            installer = Path(temp) / root / 'install.ps1'
            installed = Path(temp) / 'installed-bin'
            # Environment variables avoid interpolating filesystem paths as code.
            input_path = Path(temp) / 'commands.txt'
            input_path.write_bytes(b'version\nexit\n')
            stdout_path, stderr_path = Path(temp) / 'installed.stdout', Path(temp) / 'installed.stderr'
            install_env = dict(env, NERO_INSTALL_SCRIPT=str(installer),
                               NERO_UNINSTALL_SCRIPT=str(Path(temp) / root / 'uninstall.ps1'),
                               NERO_INSTALL_DIR=str(installed),
                               NERO_COMMAND_INPUT=str(input_path), NERO_COMMAND_OUTPUT=str(stdout_path),
                               NERO_COMMAND_ERROR=str(stderr_path))
            command = ("& $env:NERO_INSTALL_SCRIPT -InstallDir $env:NERO_INSTALL_DIR -PathScope Process; "
                       "if (-not $?) { exit 1 }; "
                       "$cmd = Get-Command nero -ErrorAction Stop; "
                       "$expected = [IO.Path]::GetFullPath((Join-Path $env:NERO_INSTALL_DIR 'nero.exe')); "
                       "if ([IO.Path]::GetFullPath($cmd.Source) -ne $expected) { throw 'Wrong nero command resolved' }; "
                       "& (Join-Path $env:SystemRoot 'System32\\cmd.exe') /d /c "
                       "'nero < commands.txt > installed.stdout 2> installed.stderr'; "
                       "if ($LASTEXITCODE -ne 0) { throw ('Installed CLI failed: ' + $LASTEXITCODE) }; "
                       "& $env:NERO_INSTALL_SCRIPT -InstallDir $env:NERO_INSTALL_DIR -PathScope Process; "
                       "if (-not $?) { throw 'Reinstall failed' }; "
                       "$matches = @($env:Path -split ';' | Where-Object { $_.TrimEnd('\\') -ieq $env:NERO_INSTALL_DIR.TrimEnd('\\') }); "
                       "if ($matches.Count -ne 1) { throw 'Duplicate installed PATH entry' }; "
                       "$sentinel = Join-Path $env:NERO_INSTALL_DIR 'keep-me.txt'; "
                       "Set-Content -LiteralPath $sentinel -Value 'preserve'; "
                       "& $env:NERO_UNINSTALL_SCRIPT -InstallDir $env:NERO_INSTALL_DIR -PathScope Process; "
                       "if (-not $?) { throw 'Uninstall failed' }; "
                       "if (Test-Path -LiteralPath $expected) { throw 'Executable remains after removal' }; "
                       "if (-not (Test-Path -LiteralPath $sentinel)) { throw 'Unrelated file removed' }; "
                       "if (@($env:Path -split ';' | Where-Object { $_.TrimEnd('\\') -ieq $env:NERO_INSTALL_DIR.TrimEnd('\\') }).Count) { throw 'Installed PATH entry remains' }; "
                       "Write-Output 'NERO_COMMAND_INSTALL_OK'")
            check = subprocess.run([powershell, '-NoProfile', '-NonInteractive', '-Command', command],
                                   capture_output=True, timeout=30, env=install_env, cwd=temp)
            if (check.returncode != 0 or b'NERO_COMMAND_INSTALL_OK' not in check.stdout
                    or not stdout_path.exists() or f'version {args.version}'.encode() not in stdout_path.read_bytes()):
                raise RuntimeError(f'installed nero command failed ({check.returncode}): stdout={check.stdout!r}, stderr={check.stderr!r}, cli_stdout={stdout_path.read_bytes() if stdout_path.exists() else None!r}, cli_stderr={stderr_path.read_bytes() if stderr_path.exists() else None!r}')
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_name(archive.name + '.sha256').write_text(f'{digest}  {archive.name}\n')
    print(archive)


if __name__ == '__main__':
    main()

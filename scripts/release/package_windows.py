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
        for source, destination in [('LICENSE.txt', 'LICENSE.txt'), ('README.md', 'README.md'), ('docs/releases.md', 'INSTALL.md')]:
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
            if result.returncode != 0 or b'version 1.0' not in result.stdout:
                raise RuntimeError(f'extracted standalone CLI failed: {result.returncode}: {result.stderr!r}')
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_name(archive.name + '.sha256').write_text(f'{digest}  {archive.name}\n')
    print(archive)


if __name__ == '__main__':
    main()

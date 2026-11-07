"""Package the native Make executable; never substitute an offline test binary."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import tarfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--version', required=True)
    parser.add_argument('--platform', required=True, choices=['macos-arm64', 'macos-x86_64', 'linux-arm64', 'linux-x86_64'])
    parser.add_argument('--commit', required=True)
    parser.add_argument('--output', default='.build/release')
    args = parser.parse_args()
    if not re.fullmatch(r'v\d+\.\d+\.\d+(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?', args.version):
        parser.error('version must be vMAJOR.MINOR.PATCH, optionally with a prerelease suffix')
    if not re.fullmatch(r'[0-9a-f]{40}', args.commit):
        parser.error('commit must be a full SHA-1')
    host_os = {'Darwin': 'macos', 'Linux': 'linux'}.get(platform.system())
    host_arch = {'aarch64': 'arm64', 'arm64': 'arm64', 'x86_64': 'x86_64'}.get(platform.machine())
    if args.platform != f'{host_os}-{host_arch}':
        parser.error('package platform must match the native runner')
    binary = Path('nero')
    description = subprocess.check_output(['file', str(binary)], text=True).strip()
    expected = 'arm64' if host_os == 'macos' and host_arch == 'arm64' else ('aarch64' if host_arch == 'arm64' else 'x86-64')
    if host_os == 'macos' and host_arch == 'x86_64':
        expected = 'x86_64'
    if expected not in description or ('Mach-O' if host_os == 'macos' else 'ELF') not in description:
        parser.error(f'unexpected executable architecture: {description}')
    dependency_command = ['otool', '-L', str(binary)] if host_os == 'macos' else ['ldd', './nero']
    dependencies = subprocess.check_output(dependency_command, text=True)
    if 'not found' in dependencies:
        parser.error('executable has unresolved runtime dependencies')
    if host_os == 'macos' and any(p in dependencies for p in ['/opt/homebrew/', '/usr/local/', '/Users/']):
        parser.error('macOS package must not depend on local third-party libraries')
    root = f'nero-{args.version}-{args.platform}'
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    archive = output / f'{root}.tar.gz'
    metadata = {
        'version': args.version, 'commit': args.commit, 'platform': args.platform,
        'build': platform.platform(), 'binary': description,
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        'runtime_dependencies': dependencies,
    }
    with tarfile.open(archive, 'w:gz') as tar:
        for source, destination in [('nero', 'bin/nero'), ('LICENSE.txt', 'LICENSE.txt'), ('README.md', 'README.md'), ('docs/releases.md', 'INSTALL.md')]:
            tar.add(source, arcname=f'{root}/{destination}', recursive=False)
        data = (json.dumps(metadata, indent=2) + '\n').encode()
        info = tarfile.TarInfo(f'{root}/BUILD-INFO.json')
        info.size, info.mode = len(data), 0o644
        tar.addfile(info, io.BytesIO(data))
    # Verify the actual archive contents and executable bit after packing.
    with tarfile.open(archive) as tar:
        member = tar.getmember(f'{root}/bin/nero')
        if not member.mode & 0o111 or tar.extractfile(member).read() != binary.read_bytes():
            raise RuntimeError('archive executable verification failed')
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_name(archive.name + '.sha256').write_text(f'{digest}  {archive.name}\n')
    print(archive)


if __name__ == '__main__':
    main()

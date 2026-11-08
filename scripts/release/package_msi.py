"""Create a native Windows Installer wizard with managed PATH and upgrades."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--product-version', help='MSI version override for disposable upgrade tests')
    parser.add_argument('--output', type=Path, default=Path('.build/release'))
    args = parser.parse_args()
    match = re.fullmatch(r'v(\d+\.\d+\.\d+)(?:-[0-9A-Za-z.]+)?', args.version)
    if not match or not re.fullmatch(r'[0-9a-f]{40}', args.commit) or platform.system() != 'Windows' or platform.machine().lower() not in ('amd64', 'x86_64'):
        parser.error('requires a release tag and native Windows x86-64 host')
    version = args.product_version or match[1]
    if not re.fullmatch(r'\d+\.\d+\.\d+', version): parser.error('invalid MSI product version')
    parts = list(map(int, version.split('.')))
    if parts[0] > 255 or parts[1] > 255 or parts[2] > 65535: parser.error('MSI version out of range')
    binary = args.binary.resolve(strict=True)
    data = binary.read_bytes()
    if len(data) < 64 or data[:2] != b'MZ': parser.error('expected a native Windows executable')
    offset = struct.unpack_from('<I', data, 0x3c)[0]
    if offset + 6 > len(data) or data[offset:offset + 4] != b'PE\0\0' or struct.unpack_from('<H', data, offset + 4)[0] != 0x8664:
        parser.error('expected an x86-64 PE executable')
    env = dict(os.environ, NO_COLOR='1', NERO_ASCII='1', NERO_LOG_FILE=':memory:', NERO_HISTORY_FILE=':memory:')
    check = subprocess.run([str(binary)], input=b'version\nexit\n', capture_output=True, env=env, timeout=15)
    if check.returncode or f'version {args.version}'.encode() not in check.stdout:
        parser.error('binary release version mismatch')
    candidates = [Path(os.environ.get('WIX', 'C:/Program Files (x86)/WiX Toolset v3.14')) / 'bin',
                  Path('C:/Program Files (x86)/WiX Toolset v3.14/bin')]
    wix = next((p for p in candidates if (p / 'candle.exe').is_file() and (p / 'light.exe').is_file()), None)
    if wix is None: parser.error('WiX Toolset v3.14 build tools are required')
    args.output.mkdir(parents=True, exist_ok=True)
    output = (args.output / f'nero-{args.version}-windows-x86_64.msi').resolve()
    with tempfile.TemporaryDirectory(prefix='nero-msi-') as tmp:
        work = Path(tmp)
        license_text = (ROOT / 'LICENSE.txt').read_text()
        escaped = ''.join(('\\' + c if c in '\\{}' else '\\par\n' if c == '\n'
                           else '\\u' + str(ord(c) if ord(c) < 32768 else ord(c) - 65536) + '?'
                           if ord(c) > 127 else c) for c in license_text)
        license_file = work / 'License.rtf'
        license_file.write_text('{\\rtf1\\ansi\\deff0 {\\fonttbl {\\f0 Segoe UI;}}\\f0\\fs20 ' + escaped + '}')
        metadata = work / 'BUILD-INFO.json'
        metadata.write_text(json.dumps({'version': args.version, 'installer_version': version,
            'commit': args.commit, 'platform': 'windows-x86_64', 'signed': False,
            'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}, indent=2) + '\n')
        obj = work / 'nero.wixobj'
        subprocess.run([str(wix / 'candle.exe'), '-nologo', '-arch', 'x64',
                        '-dProductVersion=' + version, '-dBinarySource=' + str(binary),
                        '-dLicenseSource=' + str(license_file),
                        '-dLicenseTextSource=' + str(ROOT / 'LICENSE.txt'),
                        '-dBuildInfoSource=' + str(metadata), '-out', str(obj),
                        str(ROOT / 'packaging/windows/nero.wxs')], check=True)
        subprocess.run([str(wix / 'light.exe'), '-nologo', '-ext', 'WixUIExtension',
                        '-out', str(output), str(obj)], check=True)
    # WiX also emits a debug database; do not distribute it as an installer.
    output.with_suffix('.wixpdb').unlink(missing_ok=True)
    output.with_name(output.name + '.sha256').write_text(
        f'{hashlib.sha256(output.read_bytes()).hexdigest()}  {output.name}\n')
    print(output)


if __name__ == '__main__': main()

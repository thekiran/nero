"""Native graphical macOS Installer package for the NERO command."""
import argparse
import hashlib
import html
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--output', type=Path, default=Path('.build/release'))
    args = parser.parse_args()
    match = re.fullmatch(r'v(\d+\.\d+\.\d+)(?:-[0-9A-Za-z.]+)?', args.version)
    if not match or not re.fullmatch(r'[0-9a-f]{40}', args.commit):
        parser.error('expected a version tag and full source commit')
    arch = {'arm64': 'arm64', 'x86_64': 'x86_64'}.get(platform.machine())
    if platform.system() != 'Darwin' or not arch:
        parser.error('requires native macOS ARM64/Intel')
    binary = args.binary.resolve(strict=True)
    description = subprocess.check_output(['file', str(binary)], text=True)
    if 'Mach-O' not in description or arch not in description:
        parser.error('binary architecture does not match this installer')
    env = dict(os.environ, NO_COLOR='1', NERO_ASCII='1', NERO_LOG_FILE=':memory:', NERO_HISTORY_FILE=':memory:')
    check = subprocess.run([str(binary)], input=b'version\nexit\n', capture_output=True, env=env, timeout=15)
    if check.returncode or f'version {args.version}'.encode() not in check.stdout:
        parser.error('binary release version mismatch')
    args.output.mkdir(parents=True, exist_ok=True)
    output = (args.output / f'nero-{args.version}-macos-{arch}.pkg').resolve()
    with tempfile.TemporaryDirectory(prefix='nero-pkg-') as tmp:
        work = Path(tmp)
        payload = work / 'payload'
        (payload / 'usr/local/bin').mkdir(parents=True)
        shutil.copy2(binary, payload / 'usr/local/bin/nero')
        (payload / 'usr/local/bin/nero').chmod(0o755)
        docs = payload / 'usr/local/share/doc/nero'
        docs.mkdir(parents=True)
        shutil.copy2(ROOT / 'LICENSE.txt', docs / 'LICENSE.txt')
        (docs / 'BUILD-INFO.json').write_text(json.dumps({
            'version': args.version, 'commit': args.commit, 'platform': f'macos-{arch}',
            'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
            'signed': False, 'notarized': False,
        }, indent=2) + '\n')
        subprocess.run(['pkgbuild', '--root', str(payload), '--identifier', 'com.thekiran.nero',
                        '--version', match[1], '--install-location', '/', '--ownership', 'recommended',
                        str(work / 'payload.pkg')], check=True)
        resources = work / 'resources'
        resources.mkdir()
        shutil.copy2(ROOT / 'LICENSE.txt', resources / 'License.txt')
        (resources / 'Welcome.html').write_text('<html><body><h1>NERO</h1><p>This installer makes the '
            'NERO command available system-wide. No build tools or terminal installation commands are required.</p>'
            '<p>After installation, open Terminal in any folder and type <b>nero</b>. '
            'Use <b>sudo nero</b> only for operations requiring administrator privileges.</p>'
            '<p>Experimental network diagnostics are for authorized laboratories.</p></body></html>')
        host = 'arm64' if arch == 'arm64' else 'x86_64'
        distribution = f'''<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
  <title>NERO {html.escape(args.version)}</title>
  <welcome file="Welcome.html"/>
  <license file="License.txt"/>
  <options customize="never" require-scripts="false" hostArchitectures="{host}"/>
  <domains enable_localSystem="true" enable_currentUserHome="false" enable_anywhere="false"/>
  <volume-check><allowed-os-versions><os-version min="15.0"/></allowed-os-versions></volume-check>
  <choices-outline><line choice="nero"/></choices-outline>
  <choice id="nero" visible="false" title="NERO"><pkg-ref id="com.thekiran.nero"/></choice>
  <pkg-ref id="com.thekiran.nero" version="{match[1]}" onConclusion="none">payload.pkg</pkg-ref>
</installer-gui-script>
'''
        (work / 'Distribution.xml').write_text(distribution)
        subprocess.run(['productbuild', '--distribution', str(work / 'Distribution.xml'),
                        '--package-path', str(work), '--resources', str(resources), str(output)], check=True)
        expanded = work / 'expanded'
        subprocess.run(['pkgutil', '--expand-full', str(output), str(expanded)], check=True)
        files = list(expanded.glob('**/Payload/usr/local/bin/nero'))
        if len(files) != 1 or files[0].read_bytes() != binary.read_bytes():
            raise RuntimeError('native Installer payload verification failed')
    output.with_name(output.name + '.sha256').write_text(
        f'{hashlib.sha256(output.read_bytes()).hexdigest()}  {output.name}\n')
    print(output)


if __name__ == '__main__': main()

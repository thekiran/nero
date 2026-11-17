"""Opt-in real radio test. Requires prior Location consent and visible networks.
Never requests consent, enables monitor mode, or changes the Wi-Fi association.
Usage: python3 tests/test_macos_wifi_live.py ./nero ./NEROWiFi.app
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

binary, app = (Path(x).resolve() for x in sys.argv[1:])
env = dict(os.environ, NO_COLOR='1', TERM='dumb', NERO_HISTORY_FILE=':memory:',
           NERO_LOG_FILE=':memory:', COLUMNS='140')
with tempfile.TemporaryDirectory(prefix='nero-live-') as directory:
    response = Path(directory) / 'result.json'
    response.touch(mode=0o600)
    subprocess.run(['/usr/bin/open', '-n', '-W', '-g', str(app), '--args',
                    '--scan', '--output', str(response)], check=True, timeout=32)
    data = json.loads(response.read_bytes())
    assert data['status'] == 'authorized', data['status']
    identified = [r for r in data['aps'] if r['bssid'] and r['ssid_hex']]
    assert identified, 'No identified networks; check permission/radio environment'
    print(f'PASS live helper: {len(identified)} APs with SSID and BSSID (identifiers not logged)')

for command in ('wifi scan', 'wifi aps', 'wifi scan'):
    result = subprocess.run([str(binary)], input=command+'\nexit\n', text=True,
                            capture_output=True, env=env, timeout=35)
    assert result.returncode == 0, f'{command} failed: exit {result.returncode}'
    bssids = set(re.findall(r'(?i)\b(?:[0-9a-f]{2}:){5}[0-9a-f]{2}\b', result.stdout))
    assert bssids, f'{command} returned no BSSIDs'
    assert 'active' in result.stdout.lower()
    assert 'wifi scan failed' not in result.stdout and 'wifi aps:' not in result.stdout
    print(f'PASS {command}: {len(bssids)} BSSIDs; fresh helper launch')
print('PASS live startup/permission/CLI integration; no monitor capture invoked')

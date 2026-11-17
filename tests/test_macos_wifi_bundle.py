"""Offline checks of the actual signed helper, not just source plist contents."""
import pathlib
import plistlib
import subprocess
import sys

app = pathlib.Path(sys.argv[1]).resolve()
info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
assert info['CFBundleIdentifier'] == 'com.thekiran.nero.wifi'
assert info['NSLocationUsageDescription']
assert info['NSLocationWhenInUseUsageDescription']
subprocess.run(['codesign', '--verify', '--strict', str(app)], check=True)
result = subprocess.run(['codesign', '-d', '--entitlements', ':-', str(app)],
                        check=True, capture_output=True)
entitlements = plistlib.loads(result.stdout)
assert entitlements.get('com.apple.security.personal-information.location') is True, \
    'Hardened runtime blocks the permission prompt without the Location entitlement'
print('Signed Wi-Fi bundle checks passed (no permission prompt or radio activity)')

import subprocess
for data in (b'0\n', b'999\n', b'1junk\n', b'4294967297\n', b'9'*200+b'\n', b''):
    r = subprocess.run(['/tmp/interfaces'], input=data, capture_output=True, timeout=2)
    assert r.returncode == 1, (data, r.returncode)
r = subprocess.run(['/tmp/interfaces'], input=b'1\n', capture_output=True, timeout=2)
assert r.returncode == 0
print('PASS Linux interface parser valid and six invalid cases')

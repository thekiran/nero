import subprocess, sys, re
binary = sys.argv[1] if len(sys.argv) > 1 else '/tmp/interfaces'
for data in (b'0\n', b'999\n', b'1junk\n', b'4294967297\n', b'9'*200+b'\n', b''):
    r = subprocess.run([binary], input=data, capture_output=True, timeout=2)
    assert r.returncode == 1, (data, r.returncode)
r = subprocess.run([binary], input=b'1\n', capture_output=True, timeout=2)
assert r.returncode == 0, r.stderr
output = re.sub(rb"\x1b\[[0-9;]*[A-Za-z]", b"", r.stdout)
names = re.findall(rb"^\s*\d+\) (\S+)", output, re.M)
assert names and len(names) == len(set(names)), output
assert b"SELECTED:" + names[0] in output, output
print('PASS Linux interface parser valid and six invalid cases')

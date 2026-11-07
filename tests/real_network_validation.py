"""Opt-in macOS loopback socket checks; never scans a network."""
import socket, subprocess, re, errno, time, os, signal, fcntl, resource

def fds():
    result = set()
    for fd in range(min(resource.getrlimit(resource.RLIMIT_NOFILE)[0], 4096)):
        try: fcntl.fcntl(fd, fcntl.F_GETFD); result.add(fd)
        except OSError: pass
    return result

before = fds()
for i in range(25):
    with socket.socket() as server, socket.socket() as client:
        server.bind(('127.0.0.1', 0)); server.listen(1)
        client.settimeout(1); client.connect(server.getsockname())
        peer, _ = server.accept()
        with peer:
            peer.settimeout(1); client.sendall(b'nero'); assert peer.recv(4) == b'nero'
            peer.sendall(b'ok'); assert client.recv(2) == b'ok'
        assert client.recv(1) == b''
    with socket.socket(type=socket.SOCK_DGRAM) as receiver, socket.socket(type=socket.SOCK_DGRAM) as sender:
        receiver.bind(('127.0.0.1', 0)); receiver.settimeout(.03); sender.settimeout(1)
        sender.sendto(b'nero', receiver.getsockname())
        data, source = receiver.recvfrom(32); assert data == b'nero'
        receiver.sendto(b'ok', source); assert sender.recvfrom(32)[0] == b'ok'
        try: receiver.recvfrom(32); raise AssertionError('no timeout')
        except socket.timeout: pass
print('PASS TCP EOF, UDP bidirectional, UDP receive timeout: 25 cycles')
with socket.socket() as reserved, socket.socket() as client:
    reserved.bind(('127.0.0.1', 0)); client.settimeout(1)
    try: client.connect(reserved.getsockname()); raise AssertionError('connected')
    except socket.timeout: print('PASS TCP connect timeout; BLOCKED refusal: local non-listener timed out')
    except OSError as e: assert e.errno == errno.ECONNREFUSED; print('PASS refusal errno', e.errno)
assert fds() == before, (before, fds())
print('PASS socket descriptor set unchanged', sorted(before))
assert socket.getaddrinfo('localhost', 0)
try: socket.getaddrinfo('invalid numeric address', 0, flags=socket.AI_NUMERICHOST); raise AssertionError('resolved')
except socket.gaierror as e: print('PASS local resolver and invalid numeric error', e.errno)
dns_fds = fds()
for _ in range(25): assert socket.getaddrinfo('localhost', 0)
assert fds() == dns_fds
print('PASS resolver descriptors stable after initialization', sorted(dns_fds))
binary = '.build/tests/interfaces'
r = subprocess.run([binary], input=b'0\n', capture_output=True, timeout=3)
out = re.sub(rb'\x1b\[[0-9;]*[A-Za-z]', b'', r.stdout)
match = re.search(rb'(\d+)\) en0\s', out); assert match, out
for data, code in [(match[1]+b'\n', 0), (b'0\n', 1), (b'999\n', 1), (b'1junk\n', 1)]:
    r = subprocess.run([binary], input=data, capture_output=True, timeout=3)
    assert r.returncode == code
    if code == 0: assert b'SELECTED:en0' in r.stdout
    print('PASS interface', data, 'exit', code)
r = subprocess.run(['./nero'], input=b'version\n127.0.0.1 -Syn\nexit\n', capture_output=True, timeout=3)
assert r.returncode == 0 and b'root' in r.stderr and b'sudo' in r.stderr
print('PASS production CLI reports raw-socket privilege requirement, exit 0')
for sig in (signal.SIGINT, signal.SIGTERM):
    p = subprocess.Popen(['./nero'], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        time.sleep(.2); p.send_signal(sig); _, err = p.communicate(timeout=3)
        assert p.returncode == 128+sig, (p.returncode, err)
        print('PASS production signal', sig, 'exit', p.returncode)
    finally:
        if p.poll() is None: p.kill(); p.wait()

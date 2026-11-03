import subprocess, signal, time
for sig in (signal.SIGINT, signal.SIGTERM):
    p = subprocess.Popen(['/tmp/nero-gcc'], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        time.sleep(.2)
        p.send_signal(sig)
        _, err = p.communicate(timeout=3)
        assert p.returncode == 128 + sig, (p.returncode, err)
        print('PASS production CLI', sig, 'exit', p.returncode)
    finally:
        if p.poll() is None: p.kill(); p.wait()

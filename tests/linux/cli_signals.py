import subprocess, signal, sys, time
binary = sys.argv[1] if len(sys.argv) > 1 else '/tmp/nero-gcc'
for sig in (signal.SIGINT, signal.SIGTERM):
    p = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        time.sleep(.2)
        p.send_signal(sig)
        _, err = p.communicate(timeout=3)
        assert p.returncode == 128 + sig, (p.returncode, err)
        print('PASS production CLI', sig, 'exit', p.returncode)
    finally:
        if p.poll() is None: p.kill(); p.wait()

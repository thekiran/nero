"""No live traffic: these executables never link the scanner or traffic engine."""
import os
import pathlib
import pty
import re
import select
import subprocess
import sys
import tempfile
import termios
import time
import unittest

ROOT = pathlib.Path(sys.argv.pop(1)).resolve()
ENV = dict(os.environ, TERM="xterm", UBSAN_OPTIONS="halt_on_error=1", NERO_HISTORY_FILE=":memory:")
INTERFACE_BINARY = "interfaces-mock" if sys.platform.startswith("linux") else "interfaces"
ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")


def run(name, data, env=None):
    return subprocess.run([str(ROOT / name)], input=data, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, env=env or ENV, timeout=15)


def radiotap_header():
    return (bytes([0, 0]) + (16).to_bytes(2, "little") + (0x2A).to_bytes(4, "little") +
            bytes([0, 0]) + (2437).to_bytes(2, "little") + bytes([0, 0]) + bytes([0xD6, 0]))


def pcap_bytes(packets):
    header = ((0xA1B2C3D4).to_bytes(4, "little") + (2).to_bytes(2, "little") +
              (4).to_bytes(2, "little") + b"\x00" * 8 + (65535).to_bytes(4, "little") +
              (0x00010000 | 127).to_bytes(4, "little"))
    records = bytearray(header)
    for index, packet in enumerate(packets):
        records += ((1700000000 + index).to_bytes(4, "little") + b"\x00" * 4 +
                    len(packet).to_bytes(4, "little") + len(packet).to_bytes(4, "little") + packet)
    return bytes(records)


def beacon_packet():
    """One radiotap beacon: TestNet on channel 6, WPA2."""
    bssid = bytes.fromhex("021122334455")
    ssid = b"TestNet"
    rsn = ((1).to_bytes(2, "little") + bytes.fromhex("000fac04") + b"\x01\x00" +
           bytes.fromhex("000fac04") + (1).to_bytes(2, "little") + bytes.fromhex("000fac02") +
           (0).to_bytes(2, "little"))
    frame = ((0x0080).to_bytes(2, "little") + b"\x00\x00" + b"\xff" * 6 + bssid + bssid +
             (0x0010).to_bytes(2, "little") + b"\x00" * 8 + (100).to_bytes(2, "little") +
             (0x0011).to_bytes(2, "little") + bytes([0, len(ssid)]) + ssid +
             bytes([3, 1, 6]) + bytes([48, len(rsn)]) + rsn)
    return radiotap_header() + frame


def beacon_pcap():
    return pcap_bytes([beacon_packet()])


def client_pcap():
    """Beacon plus one to-DS data frame, so the client has a BSSID relationship."""
    bssid = bytes.fromhex("021122334455")
    client = bytes.fromhex("aabbccddeeff")
    frame = ((0x0108).to_bytes(2, "little") + b"\x00\x00" + bssid + client + bssid +
             (0x0020).to_bytes(2, "little"))
    return pcap_bytes([beacon_packet(), radiotap_header() + frame])


class SafeTests(unittest.TestCase):
    def cli(self, data):
        result = run("nero-safe", data)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        self.assertNotIn(b"Sanitizer", result.stderr)
        return ANSI.sub(b"", result.stdout + result.stderr)

    def test_commands_and_notice(self):
        output = self.cli(b"help\nhelp -All\nversion\nhistory\nunknown\nexit\n")
        for expected in (b"LABORATORY USE ONLY", b"RESTRICTED TO LABORATORY USE", b"Available Commands",
                         b"version development", b"Unknown command", b"Exiting"):
            self.assertIn(expected, output)
        self.assertIn(b"help\nhelp -All\nversion\nhistory", output)

    def assert_no_network_symbols(self, name):
        symbols = subprocess.check_output(["nm", "-u", str(ROOT / name)])
        self.assertIsNone(re.search(
            rb"\b_?(socket|connect|sendto|sendmsg|getaddrinfo|gethostbyname)(?:@[^\s]+)?$",
            symbols, re.M), symbols)

    def test_disabled_network_command(self):
        self.assert_no_network_symbols("nero-safe")
        output = self.cli(b"192.0.2.1 -Syn\n192.0.2.1 -mode\nexit\n")
        self.assertIn(b"-Syn: safe test mode; no network traffic generated.", output)
        self.assertIn(b"Network diagnostics: ON", output)
        self.assertNotIn(b"-Syn operation failed", output)
        self.assertIn(b"Exiting", output)

    def test_preflight_read_only(self):
        # The -Syn preflight screen (issue #60) is informational only. On this
        # offline binary the live engine is not built, so it must say so and
        # must never link any packet/socket symbol — the strongest form of
        # "zero calls to packet/socket APIs from the preflight entry point".
        self.assert_no_network_symbols("nero-safe")
        output = self.cli(b"preflight\n0\npreflight\n1\nexit\n")
        self.assertIn(b"-Syn Preflight (READ-ONLY)", output)
        self.assertIn(b"opens no sockets, sends no packets", output)
        self.assertIn(b"not measured", output)
        self.assertIn(b"not a safe transmission rate", output)
        self.assertIn(b"docs/laboratory-safety.md", output)
        # Offline/safe target: live raw mode is unsupported and clearly flagged.
        self.assertIn(b"Not built into this target", output)
        self.assertIn(b"Unsupported in this build", output)
        # Both the Cancel (0) and Return (1) paths leave the screen cleanly.
        self.assertEqual(output.count(b"Returning to shell."), 2)
        # The read-only screen never claims to run or start anything.
        self.assertNotIn(b"workers are running", output)

    def test_mode_off_gates_syn_at_dispatch(self):
        # -mode off must block -Syn at the command handler, consistently with
        # -Sc, before the legacy engine is ever entered. With diagnostics off,
        # -Syn produces the disabled notice and no safe-test line; turning the
        # mode back on restores the normal handler. (In this build -Sc reports
        # "unsupported" independently of the mode; the -Sc gate is exercised by
        # test_mode_off_blocks_both_sc_and_syn against the nero-mock binary.)
        output = self.cli(
            b"-mode off\n192.0.2.1 -Syn\n-mode on\n192.0.2.1 -Syn\nexit\n")
        self.assertEqual(output.count(b"Network diagnostics are disabled."), 1)
        self.assertEqual(
            output.count(b"-Syn: safe test mode; no network traffic generated."), 1)
        # The disabled notice must precede the restored safe-test line.
        self.assertLess(
            output.index(b"Network diagnostics are disabled."),
            output.index(b"-Syn: safe test mode; no network traffic generated."))

    def test_eof_and_incomplete_escape(self):
        for data in (b"", b"unfinished", b"\x1b", b"\x1b["):
            self.cli(data)

    def test_long_input_and_cursor_bounds(self):
        self.cli(b"x" * 4096 + b"\x1b[C" * 512 + b"\nexit\n")
        output = self.cli(b"versiox\x7fn\nexit\n")
        self.assertIn(b"version development", output)

    def test_history_navigation(self):
        output = self.cli(b"version\nhelp\n\x1b[A\x1b[A\x1b[B\nexit\n")
        self.assertEqual(output.count(b"NERO COMMANDS"), 2)
        output = self.cli(b"version\n\x1b[A\nexit\n")
        self.assertEqual(output.count(b"version development"), 2)

    def test_history_capacity_and_clear(self):
        self.cli(b"version\n" * 270 + b"history\nexit\n")
        self.cli(b"clear\n" * 100 + b"exit\n")

    def test_interface_selection(self):
        result = run(INTERFACE_BINARY, b"1\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        output = ANSI.sub(b"", result.stdout)
        self.assertIn(b"Available Network Interfaces", output)
        self.assertIn(b"Select an interface [1-", output)
        names = re.findall(rb"^\s*\d+\) (\S+)", output, re.M)
        self.assertTrue(names)
        self.assertEqual(len(names), len(set(names)))
        self.assertIn(b"SELECTED:" + names[0], output)
        for data in (b"0\n", b"-1\n", b"999\n", b"invalid\n", b""):
            invalid = run(INTERFACE_BINARY, data)
            self.assertEqual(invalid.returncode, 1)
            self.assertIn(b"Invalid selection.", invalid.stderr)

    @unittest.skipUnless(sys.platform == "darwin", "Darwin adapter fixtures")
    def test_interface_fixtures(self):
        for mode, data, code, expected in (
            ("normal", b"1\n", 0, b"SELECTED:fixture0"),
            ("normal", b"2\n", 0, b"SELECTED:fixture5"),
            ("normal", b"3\n", 1, b""),
            ("empty", b"", 1, b""),
            ("error", b"", 1, b"getifaddrs"),
            ("limit", b"512\n", 0, b"SELECTED:fixture99"),
        ):
            result = subprocess.run([str(ROOT / "interfaces-mock")], input=data,
                                    capture_output=True, timeout=10,
                                    env=dict(ENV, INTERFACE_FIXTURE=mode))
            self.assertEqual(result.returncode, code, result.stderr)
            self.assertIn(expected, result.stdout + result.stderr)
            self.assertNotIn(b"Sanitizer", result.stderr)

    @unittest.skipUnless(sys.platform == "darwin", "Darwin adapter fixtures")
    def test_interface_selection_requires_complete_bounded_integer(self):
        for data in (b"1junk\n", b"1 2\n", b"4294967297\n",
                     b"9" * 100 + b"\n", b"1" + b" " * 200 + b"\n",
                     b"\n", b" \t\n"):
            with self.subTest(data=data):
                result = run("interfaces-mock", data)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertRegex(result.stderr, rb"Invalid selection\.|Input too long\.")
                self.assertNotIn(b"SELECTED:", result.stdout)
        for data in (b"1\n", b" 1 \t\n", b"1"):
            with self.subTest(data=data):
                result = run("interfaces-mock", data)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(b"SELECTED:fixture0", result.stdout)

    def test_wifi_tables_adapt_to_terminal_width(self):
        with tempfile.TemporaryDirectory() as tmp:
            capture = pathlib.Path(tmp) / "beacon.pcap"
            capture.write_bytes(beacon_pcap())

            def analyze(columns):
                env = dict(ENV, COLUMNS=str(columns), NERO_ASCII="1")
                result = run("nero-safe", f"wifi analyze {capture}\nexit\n".encode(), env)
                self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
                self.assertNotIn(b"Sanitizer", result.stderr)
                return ANSI.sub(b"", result.stdout + result.stderr).decode(errors="replace")

            narrow = analyze(50)
            self.assertIn("| TestNet", narrow)
            self.assertIn("narrow view:", narrow)
            self.assertNotIn("LAST SEEN", narrow)
            wide = analyze(120)
            self.assertIn("LAST SEEN", wide)
            self.assertNotIn("narrow view:", wide)
            for output, limit in ((narrow, 50), (wide, 120)):
                for line in output.splitlines():
                    if line.startswith(("+", "|", "  (")):
                        self.assertLessEqual(len(line), limit, line)

    def test_wifi_capabilities_offline(self):
        output = self.cli(b"wifi capabilities\nexit\n")
        for expected in (b"NERO WIRELESS CAPABILITIES", b"Offline build",
                         b"unavailable in this offline build", b"nl80211 control"):
            self.assertIn(expected, output)
        self.assertNotIn(b"delivery must be verified", output)

    def test_wifi_monitor_mode_offline(self):
        output = self.cli(b"wifi monitor enable lo0\nwifi monitor disable lo0\n"
                          b"wifi monitor enable lo0 --channel 6\nexit\n")
        self.assertEqual(output.count(b"unavailable in this offline build"), 2)
        self.assertEqual(output.count(b"No interface state was changed."), 2)
        self.assertIn(b"Invalid command parameters", output)

    def test_wifi_signal_offline_and_validation(self):
        output = self.cli(b"wifi signal\nwifi signal --live\nwifi signal --bogus\nexit\n")
        self.assertEqual(output.count(b"unavailable in this offline build"), 2)
        self.assertIn(b"Invalid command parameters", output)
        self.assertNotIn(b"RSSI:", output)

    def test_wifi_scan_band_channel_validation(self):
        output = self.cli(b"wifi scan --band 7\nwifi scan --channel 999\n"
                          b"wifi scan --band 5 --channel 36\nwifi scan --live\nexit\n")
        self.assertEqual(output.count(b"Invalid command parameters"), 2)
        self.assertEqual(output.count(b"unsupported in this offline build"), 2)

    def test_wifi_clients_bssid_filter(self):
        with tempfile.TemporaryDirectory() as tmp:
            capture = pathlib.Path(tmp) / "clients.pcap"
            capture.write_bytes(client_pcap())
            matching = self.cli(f"wifi analyze {capture}\n"
                                f"wifi clients --bssid 02:11:22:33:44:55\nexit\n".encode())
            self.assertIn(b"BSSID filter:   02:11:22:33:44:55", matching)
            self.assertIn(b"1 client device(s) observed", matching)

            missing = self.cli(f"wifi analyze {capture}\n"
                               f"wifi clients --bssid ff:ff:ff:ff:ff:ff\nexit\n".encode())
            self.assertIn(b"BSSID filter:   FF:FF:FF:FF:FF:FF", missing)
            self.assertIn(b"0 client device(s) observed", missing)

    def test_wifi_observed_tables_are_populated(self):
        with tempfile.TemporaryDirectory() as tmp:
            capture = pathlib.Path(tmp) / "clients.pcap"
            capture.write_bytes(client_pcap())
            output = self.cli(f"wifi analyze {capture}\n"
                              f"wifi aps\nwifi clients\nexit\n".encode())
            for expected in (b"ACCESS POINTS  (1 observed)", b"TestNet",
                             b"02:11:22:33:44:55", b"CLIENT DEVICES",
                             b"AA:BB:CC:DD:EE:FF", b"-42", b"1 client device(s) observed",
                             b"ch  6"):
                self.assertIn(expected, output)
            self.assertNotIn(b"no access points", output.lower())

    def test_pty_restores_terminal(self):
        master, slave = pty.openpty()
        before = termios.tcgetattr(slave)
        process = subprocess.Popen([str(ROOT / "nero-safe")], stdin=slave,
                                   stdout=slave, stderr=slave, env=ENV)
        output = bytearray()
        try:
            deadline = time.monotonic() + 10
            while b"\xe2\x96\xba" not in output:
                self.assertLess(time.monotonic(), deadline, output)
                if select.select([master], [], [], 0.1)[0]:
                    output.extend(os.read(master, 65536))
            during = termios.tcgetattr(slave)
            self.assertFalse(during[3] & (termios.ECHO | termios.ICANON))
            for command in (b"version\n", b"clear\n"):
                os.write(master, command)
                response = bytearray()
                deadline = time.monotonic() + 10
                while b"\xe2\x96\xba" not in response:
                    self.assertLess(time.monotonic(), deadline, response)
                    if select.select([master], [], [], 0.1)[0]:
                        response.extend(os.read(master, 65536))
            os.write(master, b"exit\n")
            deadline = time.monotonic() + 10
            while process.poll() is None:
                self.assertLess(time.monotonic(), deadline)
                if select.select([master], [], [], 0.1)[0]:
                    output.extend(os.read(master, 65536))
            process.wait(timeout=1)
            self.assertEqual(process.returncode, 0)
            after = termios.tcgetattr(slave)
            # BSD may set the transient pending-input bit when ICANON is restored.
            after[3] &= ~termios.PENDIN
            before[3] &= ~termios.PENDIN
            self.assertEqual(after, before)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)


unittest.main()

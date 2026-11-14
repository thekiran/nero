"""Portable table-data verification for macOS, Linux and Windows.

This test intentionally avoids termios/pty APIs so the same fixture can run on
all three platforms. It feeds a deterministic pcap through the CLI and checks
that observed access points, clients, filters and inspection fields actually
reach the rendered tables.
"""
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

BINARY = pathlib.Path(sys.argv.pop(1)).resolve()
ENV = dict(os.environ, NO_COLOR="1", NERO_ASCII="1", TERM="dumb", COLUMNS="120",
           NERO_HISTORY_FILE=":memory:", NERO_LOG_FILE=":memory:")


def radiotap_header():
    return (bytes([0, 0]) + struct.pack("<H", 16) + struct.pack("<I", 0x2A) +
            bytes([0, 0]) + struct.pack("<H", 2437) + bytes([0, 0]) + bytes([0xD6, 0]))


def pcap_bytes(packets):
    header = (struct.pack("<I", 0xA1B2C3D4) + struct.pack("<H", 2) +
              struct.pack("<H", 4) + b"\x00" * 8 + struct.pack("<I", 65535) +
              struct.pack("<I", 0x00010000 | 127))
    records = bytearray(header)
    for index, packet in enumerate(packets):
        records += (struct.pack("<I", 1700000000 + index) + b"\x00" * 4 +
                    struct.pack("<I", len(packet)) + struct.pack("<I", len(packet)) + packet)
    return bytes(records)


def beacon_packet():
    bssid = bytes.fromhex("021122334455")
    ssid = b"TestNet"
    rsn = (struct.pack("<H", 1) + bytes.fromhex("000fac04") + b"\x01\x00" +
           bytes.fromhex("000fac04") + struct.pack("<H", 1) +
           bytes.fromhex("000fac02") + struct.pack("<H", 0))
    frame = (struct.pack("<H", 0x0080) + b"\x00\x00" + b"\xff" * 6 + bssid + bssid +
             struct.pack("<H", 0x0010) + b"\x00" * 8 + struct.pack("<H", 100) +
             struct.pack("<H", 0x0011) + bytes([0, len(ssid)]) + ssid +
             bytes([3, 1, 6]) + bytes([48, len(rsn)]) + rsn)
    return radiotap_header() + frame


def client_packet():
    bssid = bytes.fromhex("021122334455")
    client = bytes.fromhex("aabbccddeeff")
    frame = (struct.pack("<H", 0x0108) + b"\x00\x00" + bssid + client + bssid +
             struct.pack("<H", 0x0020))
    return radiotap_header() + frame


class WifiTables(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tempdir = tempfile.TemporaryDirectory()
        cls.capture = pathlib.Path(cls.tempdir.name) / "tables.pcap"
        cls.capture.write_bytes(pcap_bytes([beacon_packet(), client_packet()]))

    @classmethod
    def tearDownClass(cls):
        cls.tempdir.cleanup()

    def cli(self, commands):
        data = ("\n".join(commands) + "\nexit\n").encode()
        result = subprocess.run([str(BINARY)], input=data, capture_output=True,
                                timeout=30, env=ENV)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        self.assertNotIn(b"Sanitizer", result.stderr)
        return (result.stdout + result.stderr).replace(b"\r\n", b"\n")

    def test_observed_rows_and_filters(self):
        # Forward slashes keep the parser's quote/escape handling portable.
        capture = self.capture.as_posix()
        output = self.cli([
            f'wifi analyze "{capture}"',
            "wifi aps",
            "wifi clients",
            "wifi clients --bssid 02:11:22:33:44:55",
            "wifi clients --bssid ff:ff:ff:ff:ff:ff",
            "wifi inspect 02:11:22:33:44:55",
            "wifi inspect aa:bb:cc:dd:ee:ff",
        ])
        for expected in (b"OFFLINE CAPTURE ANALYSIS",
                         b"ACCESS POINTS  (1 observed)",
                         b"TestNet",
                         b"02:11:22:33:44:55",
                         b"CLIENT DEVICES",
                         b"AA:BB:CC:DD:EE:FF",
                         b"-42",
                         b"1 client device(s) observed",
                         b"BSSID filter:   02:11:22:33:44:55",
                         b"BSSID filter:   FF:FF:FF:FF:FF:FF",
                         b"0 client device(s) observed",
                         b"ACCESS POINT",
                         b"RSSI:           -42 dBm",
                         b"Frequency:      2437 MHz",
                         b"CLIENT"):
            self.assertIn(expected, output)


if __name__ == "__main__":
    unittest.main()

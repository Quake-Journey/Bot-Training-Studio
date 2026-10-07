"""Real offline decoder boundary tests; build native tools before this suite."""
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
from .projects import native_home
DECODER = Path(os.environ['BTS_TEST_DECODER']) if 'BTS_TEST_DECODER' in os.environ else \
          native_home()/('decoder.exe' if os.name == 'nt' else 'decoder')


def packet(payload):
    return struct.pack('<i', len(payload)) + payload


HEADER = packet(bytes([12]) + struct.pack('<iiB', 34, 1, 1) +
                b'baseq2\0' + struct.pack('<h', 0) + b'test\0')
END = struct.pack('<i', -1)


class NativeStreamTests(unittest.TestCase):
    def decode(self, tail):
        self.assertTrue(DECODER.is_file(), 'Build offline decoder before native tests')
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'boundary.dm2'
            path.write_bytes(HEADER + tail)
            run = subprocess.run([str(DECODER), str(path), '--maps-only'],
                                 capture_output=True, text=True, timeout=10)
            return json.loads(run.stdout)

    def test_disconnect_is_clean_only_at_end(self):
        for tail in (packet(b'\x07') + END, packet(b'\x07'),
                     packet(b'\x08') + END, packet(b'\x08')):
            with self.subTest(tail=tail):
                result = self.decode(tail)
                self.assertEqual((result['return_code'], result['quality']), (0, 0))
        for tail in (packet(b'\x08\x06') + END, packet(b'\x07\x06') + END):
            with self.subTest(tail=tail):
                self.assertNotEqual(self.decode(tail)['return_code'], 0)
        # Like normal demo playback, reconnect ends this connection. A
        # following packet cannot provide synthetic future/delta observations.
        self.assertEqual(self.decode(packet(b'\x08') + packet(b'\x00') + END)['return_code'], 0)

    def test_partial_packet_length_is_not_clean_eof(self):
        for length in (1, 2, 3):
            with self.subTest(length=length):
                result = self.decode(b'\x01' * length)
                self.assertEqual(result['return_code'], -3)
                self.assertTrue(result['quality'] & 1)
        self.assertEqual(self.decode(b'')['return_code'], 0)
        self.assertEqual(self.decode(END)['return_code'], 0)

    def test_missing_delta_base_stays_rejected(self):
        frame = bytes([20]) + struct.pack('<ii', 2, 1)
        result = self.decode(packet(frame) + END)
        self.assertEqual(result['return_code'], -2)
        self.assertTrue(result['quality'] & 2)


if __name__ == '__main__':
    unittest.main()

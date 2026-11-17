from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class NetworkLeaseTest(unittest.TestCase):
    def test_background_request_lifetime_and_recovery(self):
        with tempfile.TemporaryDirectory(prefix='switchmusic-network-') as directory:
            binary=Path(directory)/'lease'
            subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(ROOT/'tests/network/mock'),'-I'+str(ROOT/'sysmodule/include'),str(ROOT/'tests/network/lease.cpp'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

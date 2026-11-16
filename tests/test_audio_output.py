from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class AudioOutputTest(unittest.TestCase):
    def test_output_service_contract(self):
        with tempfile.TemporaryDirectory(prefix='switchmusic-audout-') as directory:
            binary=Path(directory)/'output'
            subprocess.run(['c++','-std=c++17','-I'+str(ROOT/'tests/audio/mock'),'-I'+str(ROOT/'sysmodule/include'),str(ROOT/'tests/audio/output.cpp'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

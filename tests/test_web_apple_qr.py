from pathlib import Path
import shutil
import subprocess
import unittest
ROOT=Path(__file__).resolve().parents[1]
class WebAppleQRTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('node'),'Node required for browser-flow contract test')
    def test_apple_linking_flows(self):
        subprocess.run(['node','tests/web_apple_qr.cjs'],cwd=ROOT,check=True)

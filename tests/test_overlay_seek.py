from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class OverlaySeekTest(unittest.TestCase):
    def test_controller_touch_and_progress(self):
        with tempfile.TemporaryDirectory(prefix='switchmusic-overlay-seek-') as directory:
            binary=Path(directory)/'seek'
            subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-g',
                '-I'+str(ROOT/'tests/overlay/mock'),'-I'+str(ROOT/'overlay/include'),
                str(ROOT/'tests/overlay/seek_bar.cpp'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

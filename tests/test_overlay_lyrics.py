from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class OverlayLyricsTest(unittest.TestCase):
    def test_five_rendered_slots(self):
        with tempfile.TemporaryDirectory(prefix='switchmusic-overlay-lyrics-') as directory:
            binary=Path(directory)/'lyrics'
            subprocess.run(['c++','-std=c++17','-I'+str(ROOT/'tests/overlay/mock'),'-I'+str(ROOT/'overlay/include'),
                '-I'+str(ROOT/'common/include'),'-I'+str(ROOT/'third_party/json'),str(ROOT/'tests/overlay/lyrics.cpp'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

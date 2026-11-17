from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class PlaybackQueueTest(unittest.TestCase):
    def test_shuffle_repeat_and_pagination(self):
        with tempfile.TemporaryDirectory(prefix='switchmusic-queue-') as directory:
            binary=Path(directory)/'queue'
            subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-g',
                '-I'+str(ROOT/'sysmodule/include'),'-I'+str(ROOT/'third_party/json'),str(ROOT/'tests/playback_queue.cpp'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

"""Exercise the Switch IPC client with a caller-session-quota fixture."""
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class IPCClientTest(unittest.TestCase):
    def test_session_reuse_and_recovery(self):
        with tempfile.TemporaryDirectory(prefix='switchmusic-ipc-client-') as directory:
            binary=Path(directory)/'ipc-client'
            flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','libcurl'],text=True))
            subprocess.run(['c++','-std=gnu++17','-O1','-g','-D__SWITCH__','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                '-I'+str(ROOT/'tests/ipc_support'),'-I'+str(ROOT/'common/include'),'-I'+str(ROOT/'third_party/json'),
                str(ROOT/'common/src/ipc.cpp'),str(ROOT/'common/src/client.cpp'),str(ROOT/'tests/ipc_client.cpp'),'-o',str(binary),*flags,'-pthread'],check=True)
            subprocess.run([str(binary)],check=True,timeout=15)

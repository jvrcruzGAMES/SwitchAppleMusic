"""Run the real IPC server with a host kernel/TLS fixture reproducing the crash."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class IPCServerTest(unittest.TestCase):
    def test_nested_service_call_preserves_reply_descriptors(self):
        with tempfile.TemporaryDirectory(prefix='switchmusic-ipc-server-') as directory:
            binary=Path(directory)/'ipc-server'
            subprocess.run(['c++','-std=gnu++17','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                '-I'+str(ROOT/'tests/ipc_support'),'-I'+str(ROOT/'common/include'),'-I'+str(ROOT/'sysmodule/include'),'-I'+str(ROOT/'third_party/json'),
                str(ROOT/'sysmodule/source/control_ipc.cpp'),str(ROOT/'tests/ipc_server.cpp'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=15)

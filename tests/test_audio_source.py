"""Source extraction works with the DKP image's Python and rejects unsafe archives."""
import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('audio_build',ROOT/'scripts/build-audio-ffmpeg.py')
audio_build=importlib.util.module_from_spec(spec)
spec.loader.exec_module(audio_build)

class SourceExtractionTests(unittest.TestCase):
    def archive(self, path, name, kind=tarfile.REGTYPE):
        with tarfile.open(path,'w') as archive:
            entry=tarfile.TarInfo(name)
            entry.type=kind
            if kind==tarfile.REGTYPE:
                entry.mode=0o755
                content=b'#!/bin/sh\n'
                entry.size=len(content)
                archive.addfile(entry,io.BytesIO(content))
            else:
                entry.linkname='../outside'
                archive.addfile(entry)

    def test_regular_source_and_executable_mode(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);archive=root/'source.tar'
            self.archive(archive,'ffmpeg/configure')
            destination=root/'output';destination.mkdir()
            audio_build.extract_source(archive,destination)
            self.assertEqual((destination/'ffmpeg/configure').read_bytes(),b'#!/bin/sh\n')
            self.assertTrue((destination/'ffmpeg/configure').stat().st_mode & 0o100)

    def test_traversal_links_and_special_files_rejected(self):
        cases=[('../outside',tarfile.REGTYPE),('/absolute',tarfile.REGTYPE),
               ('link',tarfile.SYMTYPE),('link',tarfile.LNKTYPE),('pipe',tarfile.FIFOTYPE)]
        for name,kind in cases:
            with self.subTest(name=name,kind=kind),tempfile.TemporaryDirectory() as folder:
                root=Path(folder);archive=root/'source.tar'
                self.archive(archive,name,kind)
                destination=root/'output';destination.mkdir()
                with self.assertRaises(ValueError):audio_build.extract_source(archive,destination)
                self.assertFalse((root/'outside').exists())

if __name__=='__main__':unittest.main()

"""Keep Toolbox metadata aligned with the module and reject reboot-only packaging."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('sd_package',ROOT/'scripts/package.py')
package=importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)

class ToolboxTests(unittest.TestCase):
    def test_metadata_matches_npdm_and_allows_live_toggling(self):
        metadata=package.validate_toolbox(ROOT/'sysmodule/toolbox.json')
        npdm=json.loads((ROOT/'sysmodule/sysmodule.json').read_text())
        self.assertEqual(int(metadata['tid'],16),int(npdm['program_id'],16))
        self.assertEqual(metadata['name'],'SwitchMusic')
        self.assertIs(metadata['requires_reboot'],False)

    def test_invalid_metadata_cannot_be_packaged(self):
        valid={'name':'SwitchMusic','tid':f'{package.TITLE:016X}','requires_reboot':False}
        cases=[{**valid,'tid':'0000000000000000'}, {**valid,'requires_reboot':True},
               {**valid,'requires_reboot':0}, {**valid,'name':''},
               {k:v for k,v in valid.items() if k!='requires_reboot'}]
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'toolbox.json'
            for metadata in cases:
                with self.subTest(metadata=metadata):
                    path.write_text(json.dumps(metadata))
                    with self.assertRaises(ValueError):package.validate_toolbox(path)

if __name__=='__main__':unittest.main()

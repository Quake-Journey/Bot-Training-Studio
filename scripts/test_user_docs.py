"""Documentation publication gates: verify negative cases without altering tracked guides."""
import importlib.util
from io import BytesIO
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch
import zipfile

spec = importlib.util.spec_from_file_location("guides", Path(__file__).with_name("build_user_docs.py"))
guides = importlib.util.module_from_spec(spec); spec.loader.exec_module(guides)


class GuideTests(unittest.TestCase):
    def test_current_pair(self):
        guides.check()

    def test_stale_source_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            for path in guides.DOCS.iterdir():
                if path.is_file() and (path.name.startswith(("USER_GUIDE_", "Bot_Training_Studio_User_Guide_")) or path.name == "user-guides.json"):
                    shutil.copyfile(path, folder / path.name)
            with patch.object(guides, "DOCS", folder), patch.object(guides, "MANIFEST", folder / "user-guides.json"):
                guides.check()
                with (folder / "USER_GUIDE_EN.md").open("a", encoding="utf-8") as out: out.write("\nChanged source\n")
                with self.assertRaisesRegex(ValueError, "source changed"): guides.check()

    def rewrite(self, member, transform):
        raw = (guides.DOCS / "Bot_Training_Studio_User_Guide_EN.docx").read_bytes()
        result = BytesIO()
        with zipfile.ZipFile(BytesIO(raw)) as source, zipfile.ZipFile(result, "w") as output:
            for name in source.namelist():
                data = source.read(name)
                output.writestr(name, transform(data) if name == member else data)
        return result.getvalue()

    def test_external_relationship_rejected(self):
        data = self.rewrite("word/_rels/document.xml.rels", lambda b: b.replace(b"</Relationships>",
            b'<Relationship Id="linked" Type="image" Target="https://example.invalid/image.png" TargetMode="External"/></Relationships>'))
        with self.assertRaisesRegex(ValueError, "External DOCX"): guides.validate(data)

    def test_automatic_updates_rejected(self):
        data = self.rewrite("word/settings.xml", lambda b: b.replace(b'w:updateFields w:val="false"', b'w:updateFields w:val="true"'))
        with self.assertRaisesRegex(ValueError, "Automatic field"): guides.validate(data)

    def test_external_field_rejected(self):
        data = self.rewrite("word/footer1.xml", lambda b: b.replace(b'w:instr="PAGE"', b'w:instr="INCLUDETEXT remote"'))
        with self.assertRaisesRegex(ValueError, "Unexpected document field"): guides.validate(data)


if __name__ == "__main__": unittest.main()
